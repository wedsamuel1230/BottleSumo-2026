// control.cpp — one manager, one 5 ms task, one function per state.
//
// Priority order inside tick():
//   OTA inhibit > 3+ edge failure > round timeout > edge escape > state.
//
// Edge failure is the user's rule: three or more IR channels off the table at
// once is a lost round, not an escape.
#include "control.h"

#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <stdio.h>
#include "edge.h"
#include "robot.h"
#include "rules.h"
#include "timing.h"

namespace bs {
namespace {

constexpr uint32_t CONTROL_BUDGET_US = 1500;
constexpr uint32_t DRIVE_LEASE_MS = 500;   // Debug dead-man
constexpr uint32_t ESCAPE_CLEAR_MS = 60;

struct Button {
  bool stable = false;
  bool candidate = false;
  uint32_t changedAt = 0;

  // Returns true on the press edge only.
  bool update(bool raw, uint32_t now) {
    if (raw != candidate) {
      candidate = raw;
      changedAt = now;
      return false;
    }
    if (candidate != stable && now - changedAt >= BUTTON_DEBOUNCE_MS) {
      stable = candidate;
      return stable;
    }
    return false;
  }
};

struct Record {
  uint32_t magic;
  Calibration cal;
  uint8_t floorDone;
  uint8_t voidDone;
};

struct Machine {
  State state = State::Init;
  Division division = Division::None;
  uint32_t stateSince = 0;
  uint32_t roundStart = 0;

  Calibration cal;
  bool floorDone = false;
  bool voidDone = false;
  EdgeFilter filter;
  uint8_t edges = 0;
  uint16_t ir[4] = {};

  uint16_t tofMm = 0;      // tick view, copied under the lock
  uint32_t tofAt = 0;
  uint16_t sharedMm = 0;   // written by controlTofSample only
  uint32_t sharedAt = 0;
  uint32_t lastTargetAt = 0;
  uint16_t lastTargetMm = 0;

  uint8_t segment = 0;
  uint32_t segmentSince = 0;

  bool advancing = true;
  int8_t spinDirection = 1;
  uint32_t phaseSince = 0;

  uint32_t escapeSince = 0;
  uint8_t escapeBits = 0;
  int8_t escapeLeft = 0;
  int8_t escapeRight = 0;

  uint32_t holdSince = 0;
  uint32_t lastDriveAt = 0;

  bool capturing = false;
  bool captureFloor = false;
  uint32_t captureSince = 0;
  int8_t progress = -1;

  bool inhibited = false;
  char event[24] = "boot";

  Snapshot snap;
  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
};

Machine m;
QueueHandle_t commands = nullptr;
Preferences prefs;

void change(State next, const char *why) {
  m.state = next;
  m.stateSince = millis();
  snprintf(m.event, sizeof(m.event), "%s", why ? why : "");
}

bool matchState() {
  return m.state != State::Init && m.state != State::Idle &&
         m.state != State::Debug && m.state != State::Failed &&
         m.state != State::RoundEnd;
}

bool freshToF(uint32_t now) { return m.tofAt && now - m.tofAt <= TOF_STALE_MS; }

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------
void loadCalibration() {
  Record record{};
  if (!prefs.begin(NVS_NAMESPACE, true)) return;
  const size_t bytes = prefs.getBytes("cal", &record, sizeof(record));
  prefs.end();
  if (bytes != sizeof(record) || record.magic != CAL_MAGIC) return;
  m.cal = record.cal;
  m.floorDone = record.floorDone != 0;
  m.voidDone = record.voidDone != 0;
}

void saveCalibration() {
  if (!prefs.begin(NVS_NAMESPACE, false)) return;
  const Record record{CAL_MAGIC, m.cal, uint8_t(m.floorDone ? 1 : 0),
                      uint8_t(m.voidDone ? 1 : 0)};
  prefs.putBytes("cal", &record, sizeof(record));
  prefs.end();
}

bool calibrated() {
  if (m.cal.manualMask == 0x0F) return m.cal.valid();
  return m.floorDone && m.voidDone && m.cal.valid();
}

// ---------------------------------------------------------------------------
// States
// ---------------------------------------------------------------------------
void stopAnd(State next, const char *why) {
  robotStop();
  change(next, why);
}

void fail(const char *why) {
  stopAnd(State::Failed, why);
  Serial.printf("FAIL: %s edges=0x%X ir=%u,%u,%u,%u\n", why, m.edges,
                m.ir[0], m.ir[1], m.ir[2], m.ir[3]);
}

void enterIdle(const char *why) {
  m.division = Division::None;
  stopAnd(State::Idle, why);
}

void enterStartDelay(const char *why) {
  robotStop();
  m.roundStart = millis();
  change(State::StartDelay, why);
}

void enterRoute() {
  m.segment = 0;
  m.segmentSince = millis();
  change(State::Route, "route");
}

void enterSearch(const char *why) {
  m.advancing = true;
  m.phaseSince = millis();
  change(State::Search, why);
}

void enterAttack() {
  m.lastTargetAt = millis();
  m.lastTargetMm = m.tofMm;
  change(State::Attack, "target");
}

void enterEscape() {
  m.escapeBits = m.edges;
  m.escapeSince = millis();
  change(State::Escape, "edge");
}

void enterHold() {
  m.holdSince = millis();
  stopAnd(State::Hold, "survive");
}

void enterRoundEnd(const char *why) {
  m.division = Division::None;
  stopAnd(State::RoundEnd, why);
}

void enterDebug() {
  robotStop();
  m.lastDriveAt = millis();
  change(State::Debug, "debug");
}

void exitEscape(const char *why) {
  enterSearch(why);
  m.advancing = false;
  m.phaseSince = millis();
  m.spinDirection = (m.escapeBits & 0x5) ? 1 : -1; // left edge -> spin right
}

void tickEscape(uint32_t now) {
  if (m.edges == 0) {
    if (now - m.escapeSince >= ESCAPE_CLEAR_MS) exitEscape("edge clear");
    else robotStop();
    return;
  }
  if (now - m.escapeSince >= ESCAPE_MAX_MS) {
    exitEscape("escape max");
    return;
  }
  const Reaction r = reactionFor(m.edges);
  m.escapeLeft = r.left;
  m.escapeRight = r.right;
  motorCommand(r.left, r.right, now);
}

void tickSearch(uint32_t now) {
  if (freshToF(now) && m.tofMm <= TOF_TRIGGER_MM) {
    enterAttack();
    return;
  }
  const uint32_t elapsed = now - m.phaseSince;
  if (m.advancing) {
    if (elapsed >= SEARCH_ADVANCE_MS) {
      m.advancing = false;
      m.phaseSince = now;
    }
    motorCommand(SEARCH_ADVANCE, SEARCH_ADVANCE, now);
    return;
  }
  if (elapsed >= SEARCH_SPIN_MS) {
    m.advancing = true;
    m.phaseSince = now;
    m.spinDirection = int8_t(-m.spinDirection);
    robotStop();
    return;
  }
  motorCommand(int8_t(m.spinDirection * SEARCH_SPIN),
               int8_t(-m.spinDirection * SEARCH_SPIN), now);
}

void tickAttack(uint32_t now) {
  const bool fresh = freshToF(now);
  if (fresh && m.tofMm > TOF_RELEASE_MM) {
    enterSearch("target far");
    return;
  }
  if (!fresh) {
    if (m.lastTargetMm && m.lastTargetMm <= TOF_CLOSE_MM) enterHold();
    else enterSearch("target lost");
    return;
  }
  m.lastTargetAt = now;
  m.lastTargetMm = m.tofMm;
  motorCommand(ATTACK_SPEED, ATTACK_SPEED, now);
}

const Segment *routeTable(uint8_t &count) {
  if (m.division == Division::B) {
    count = sizeof(ROUTE_B) / sizeof(ROUTE_B[0]);
    return ROUTE_B;
  }
  count = sizeof(ROUTE_A) / sizeof(ROUTE_A[0]);
  return ROUTE_A;
}

void tickRoute(uint32_t now) {
  uint8_t count = 0;
  const Segment *table = routeTable(count);
  if (m.segment >= count) {
    enterSearch("route done");
    return;
  }
  const Segment &s = table[m.segment];
  if (now - m.segmentSince >= s.ms) {
    ++m.segment;
    m.segmentSince = now;
    if (m.segment >= count) enterSearch("route done");
    return;
  }
  motorCommand(s.left, s.right, now);
}

void tickCalibration(uint32_t now) {
  robotStop();
  const uint32_t elapsed = now - m.captureSince;
  m.progress = int8_t(elapsed >= CALIBRATION_MS ? 100 : (100ul * elapsed) / CALIBRATION_MS);
  m.cal.sample(m.captureFloor, m.ir);
  if (elapsed < CALIBRATION_MS) return;

  m.capturing = false;
  m.progress = -1;
  if (m.captureFloor) m.floorDone = true;
  else m.voidDone = true;
  saveCalibration();
  snprintf(m.event, sizeof(m.event), "%s",
           m.cal.valid() ? "cal ok" : "cal weak");
}

void handleEdgeGuard(uint32_t now) {
  if (!matchState()) return;
  if (edgeFails(m.edges)) {
    fail("3+ edges");
    return;
  }
  if (m.edges && m.state != State::Escape) enterEscape();
  if (m.state == State::Escape) tickEscape(now);
}

void handleCommands() {
  Command command{};
  while (commands && xQueueReceive(commands, &command, 0) == pdTRUE) {
    switch (command.kind) {
      case CommandKind::StartA:
      case CommandKind::StartB:
        if (m.state == State::Idle && !m.inhibited) {
          m.division = command.kind == CommandKind::StartA ? Division::A : Division::B;
          enterStartDelay(calibrated() ? "start" : "start warn");
        }
        break;
      case CommandKind::Stop:
        if (m.state == State::Debug || m.state == State::Failed ||
            m.state == State::RoundEnd || m.state == State::Init) {
          enterIdle("stop");
        } else {
          enterRoundEnd("stop");
        }
        break;
      case CommandKind::DebugEnter:
        if (m.state == State::Idle && !m.inhibited) enterDebug();
        break;
      case CommandKind::DebugExit:
        if (m.state == State::Debug) enterIdle("debug exit");
        break;
      case CommandKind::Drive:
        if (m.state == State::Debug) {
          m.lastDriveAt = millis();
          motorCommand(command.left, command.right, millis());
        }
        break;
      case CommandKind::CalibrateFloor:
      case CommandKind::CalibrateVoid:
        if (m.state == State::Idle || m.state == State::Debug ||
            m.state == State::Failed || m.state == State::RoundEnd) {
          m.captureFloor = command.kind == CommandKind::CalibrateFloor;
          m.cal.resetCapture(m.captureFloor);
          m.captureSince = millis();
          m.capturing = true;
          m.progress = 0;
          robotStop();
          snprintf(m.event, sizeof(m.event), "%s",
                   m.captureFloor ? "cal floor" : "cal void");
        }
        break;
      case CommandKind::Manual:
        if (command.channel < 4 && !matchState() && !m.capturing) {
          m.cal.setManual(command.channel, command.value, command.high);
          saveCalibration();
          snprintf(m.event, sizeof(m.event), "manual ch%u", command.channel);
        }
        break;
      case CommandKind::ClearManual:
        if (command.channel < 4 && !matchState() && !m.capturing) {
          m.cal.clearManual(command.channel);
          saveCalibration();
          snprintf(m.event, sizeof(m.event), "auto ch%u", command.channel);
        }
        break;
    }
  }
}

void tickDebug(uint32_t now) {
  if (now - m.lastDriveAt > DRIVE_LEASE_MS) robotStop();
}

void publish(uint32_t now) {
  Snapshot snap;
  snap.state = m.state;
  snap.division = m.division;
  snap.stateMs = now - m.stateSince;
  snap.roundMs = m.roundStart ? now - m.roundStart : 0;
  snap.edges = m.edges;
  for (uint8_t i = 0; i < 4; ++i) {
    snap.ir[i] = m.ir[i];
    snap.threshold[i] = ((m.floorDone && m.voidDone) || m.cal.manual(i))
                            ? m.cal.threshold(i) : 0;
  }
  snap.tofMm = m.tofMm;
  snap.tofFresh = freshToF(now);
  snap.calibrated = calibrated();
  snap.tofOk = tofHealthy();
  snap.calibrationProgress = m.capturing ? m.progress : -1;
  snap.inhibited = m.inhibited;
  snap.controlMaxUs = g_controlTiming.maxUs;
  snap.controlMisses = g_controlTiming.misses;
  snprintf(snap.event, sizeof(snap.event), "%s", m.event);

  portENTER_CRITICAL(&m.mux);
  m.snap = snap;
  portEXIT_CRITICAL(&m.mux);
}

} // namespace

const char *stateName(State state) {
  switch (state) {
    case State::Init: return "init";
    case State::Idle: return "idle";
    case State::Debug: return "debug";
    case State::StartDelay: return "start-delay";
    case State::Route: return "route";
    case State::Search: return "search";
    case State::Attack: return "attack";
    case State::Escape: return "escape";
    case State::Hold: return "hold";
    case State::Failed: return "failed";
    case State::RoundEnd: return "round-end";
  }
  return "?";
}

const char *divisionName(Division division) {
  switch (division) {
    case Division::A: return "A";
    case Division::B: return "B";
    default: return "-";
  }
}

void controlBegin() {
  loadCalibration();
  commands = xQueueCreate(8, sizeof(Command));
  m.state = State::Idle;
  m.stateSince = millis();
  snprintf(m.event, sizeof(m.event), "ready");
  Serial.printf("control ready, cal=%s floor=%d void=%d\n",
                calibrated() ? "valid" : "weak", int(m.floorDone), int(m.voidDone));
}

void controlPost(const Command &command) {
  if (!commands) return;
  xQueueSend(commands, &command, 0);
}

void controlTofSample(uint16_t mm, uint32_t at) {
  portENTER_CRITICAL(&m.mux);
  m.sharedMm = mm;
  m.sharedAt = at;
  portEXIT_CRITICAL(&m.mux);
}

Snapshot controlSnapshot() {
  Snapshot copy;
  portENTER_CRITICAL(&m.mux);
  copy = m.snap;
  portEXIT_CRITICAL(&m.mux);
  return copy;
}

void controlSetInhibited(bool inhibited) {
  m.inhibited = inhibited;
  if (inhibited) {
    robotStop();
    if (matchState() || m.state == State::Debug) enterIdle("ota inhibit");
  }
}

void controlTask(void *arg) {
  (void)arg;
  TickType_t wake = xTaskGetTickCount();
  Button a, b;
  for (;;) {
    const uint32_t startUs = micros();
    const uint32_t now = millis();

    uint16_t tofMm = 0;
    uint32_t tofAt = 0;
    portENTER_CRITICAL(&m.mux);
    tofMm = m.sharedMm;
    tofAt = m.sharedAt;
    portEXIT_CRITICAL(&m.mux);
    m.tofMm = tofMm;
    m.tofAt = tofAt;

    irRead(m.ir);
    const uint8_t raw = m.cal.rawEdges(m.ir, m.filter.confirmed);
    m.edges = m.filter.update(now, raw);

    handleCommands();

    if (a.update(digitalRead(PIN_BUTTON_A) == HIGH, now)) {
      Command c;
      c.kind = m.state == State::Idle ? CommandKind::StartA : CommandKind::Stop;
      controlPost(c);
    }
    if (b.update(digitalRead(PIN_BUTTON_B) == HIGH, now)) {
      Command c;
      c.kind = m.state == State::Idle ? CommandKind::StartB : CommandKind::Stop;
      controlPost(c);
    }

    if (m.capturing) {
      tickCalibration(now);
    } else if (m.inhibited) {
      robotStop();
    } else {
      handleEdgeGuard(now);
      switch (m.state) {
        case State::Init:
        case State::Idle:
        case State::StartDelay:
          if (m.state == State::StartDelay && now - m.stateSince >= START_DELAY_MS) enterRoute();
          else robotStop();
          break;
        case State::Debug:
          tickDebug(now);
          break;
        case State::Route:
          tickRoute(now);
          break;
        case State::Search:
          tickSearch(now);
          break;
        case State::Attack:
          tickAttack(now);
          break;
        case State::Escape:
          break; // handled by handleEdgeGuard
        case State::Hold:
          robotStop();
          if (now - m.holdSince >= HOLD_MS) enterSearch("hold done");
          break;
        case State::Failed:
        case State::RoundEnd:
          robotStop();
          break;
      }
      if (matchState() && m.roundStart && now - m.roundStart >= ROUND_MS) {
        enterRoundEnd("timeout");
      }
    }

    publish(now);
    timingRecord(g_controlTiming, micros() - startUs, CONTROL_BUDGET_US);
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(CONTROL_MS));
  }
}

} // namespace bs
