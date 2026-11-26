// control.cpp — one manager, one 5 ms task, one function per state.
//
// Priority order inside tick():
//   OTA inhibit > 3+ edge round end > round timeout > edge escape > state.
//
// Three or more IR channels off the table at once is a lost round: enter
// RoundEnd and stop, not Escape.
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

struct RouteRecord {
  uint32_t magic;
  uint8_t count[2];
  Segment seg[2][ROUTE_MAX_SEGMENTS];
};

struct Machine {
  State state = State::Init;
  Division division = Division::None;
  uint32_t stateSince = 0;
  uint32_t roundStart = 0;

  Calibration cal;
  RouteConfig routes[2] = {};
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

  bool clearing = false;
  uint32_t clearSince = 0;

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
         m.state != State::Debug && m.state != State::RoundEnd;
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

RouteConfig defaultRoute(uint8_t div) {
  RouteConfig cfg{};
  const Segment *src = div ? ROUTE_B : ROUTE_A;
  const uint8_t count = div ? uint8_t(sizeof(ROUTE_B) / sizeof(ROUTE_B[0]))
                            : uint8_t(sizeof(ROUTE_A) / sizeof(ROUTE_A[0]));
  cfg.count = count > ROUTE_MAX_SEGMENTS ? ROUTE_MAX_SEGMENTS : count;
  for (uint8_t i = 0; i < cfg.count; ++i) cfg.seg[i] = src[i];
  return cfg;
}

bool validRoute(const RouteConfig &cfg) {
  if (cfg.count < 1 || cfg.count > ROUTE_MAX_SEGMENTS) return false;
  for (uint8_t i = 0; i < cfg.count; ++i) {
    const Segment &s = cfg.seg[i];
    if (s.left < -100 || s.left > 100) return false;
    if (s.right < -100 || s.right > 100) return false;
    if (s.ms < 10 || s.ms > 10000) return false;
  }
  return true;
}

void loadRoutes() {
  m.routes[0] = defaultRoute(0);
  m.routes[1] = defaultRoute(1);
  RouteRecord rec{};
  if (!prefs.begin(NVS_NAMESPACE, true)) return;
  const size_t bytes = prefs.getBytes("route", &rec, sizeof(rec));
  prefs.end();
  if (bytes != sizeof(rec) || rec.magic != ROUTE_MAGIC) return;
  for (uint8_t d = 0; d < 2; ++d) {
    RouteConfig cfg{};
    cfg.count = rec.count[d];
    if (cfg.count > ROUTE_MAX_SEGMENTS) continue;
    for (uint8_t i = 0; i < cfg.count; ++i) cfg.seg[i] = rec.seg[d][i];
    if (validRoute(cfg)) m.routes[d] = cfg;
  }
}

void saveRoutes() {
  if (!prefs.begin(NVS_NAMESPACE, false)) return;
  RouteRecord rec{};
  rec.magic = ROUTE_MAGIC;
  for (uint8_t d = 0; d < 2; ++d) {
    rec.count[d] = m.routes[d].count;
    for (uint8_t i = 0; i < ROUTE_MAX_SEGMENTS; ++i) rec.seg[d][i] = m.routes[d].seg[i];
  }
  prefs.putBytes("route", &rec, sizeof(rec));
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

void enterIdle(const char *why) {
  m.division = Division::None;
  stopAnd(State::Idle, why);
}

void enterStartDelay(const char *why) {
  robotStop();
  robotResetForward();
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
  m.clearing = false;
  change(State::Escape, "edge");
}

void enterHold() {
  m.holdSince = millis();
  stopAnd(State::Hold, "survive");
}

void enterRoundEnd(const char *why) {
  m.division = Division::None;
  stopAnd(State::RoundEnd, why);
  Serial.printf("ROUND END %s: forward %lu ms, %lu cm\n", why ? why : "",
                (unsigned long)robotForwardMs(),
                (unsigned long)robotForwardCm());
}

void enterDebug() {
  robotStop();
  robotResetForward();
  m.lastDriveAt = millis();
  change(State::Debug, "debug");
}

void tickEscape(uint32_t now) {
  if (m.edges == 0) {
    if (!m.clearing) {
      m.clearing = true;
      m.clearSince = now;
    }
    if (now - m.clearSince >= ESCAPE_CLEAR_MS) enterSearch("edge clear");
    else robotStop();
    return;
  }
  m.clearing = false;
  const Reaction r = reactionFor(m.edges);
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
  const RouteConfig &cfg = m.routes[m.division == Division::B ? 1 : 0];
  count = cfg.count;
  return cfg.seg;
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
  // The first leg is the opening charge: always 100/100 straight. Its
  // duration still comes from the table; later legs use their own left/right.
  if (m.segment == 0) motorCommand(100, 100, now);
  else motorCommand(s.left, s.right, now);
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
    Serial.printf("ROUND END: 3+ edges=0x%X ir=%u,%u,%u,%u\n", m.edges,
                  m.ir[0], m.ir[1], m.ir[2], m.ir[3]);
    enterRoundEnd("3+ edges");
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
        if (m.state == State::Debug || m.state == State::RoundEnd ||
            m.state == State::Init) {
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
            m.state == State::RoundEnd) {
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
      case CommandKind::RouteSet:
        if (command.routeDiv < 2 && !matchState() && !m.capturing &&
            validRoute(command.route)) {
          m.routes[command.routeDiv] = command.route;
          saveRoutes();
          snprintf(m.event, sizeof(m.event), "route %c saved",
                   command.routeDiv ? 'B' : 'A');
        }
        break;
      case CommandKind::RouteReset:
        if (command.routeDiv < 2 && !matchState() && !m.capturing) {
          m.routes[command.routeDiv] = defaultRoute(command.routeDiv);
          saveRoutes();
          snprintf(m.event, sizeof(m.event), "route %c default",
                   command.routeDiv ? 'B' : 'A');
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
  snap.forwardMs = robotForwardMs();
  snap.forwardCm = robotForwardCm();
  snap.manualMask = m.cal.manualMask;
  snap.manualHigh = m.cal.manualHigh;
  snap.routes[0] = m.routes[0];
  snap.routes[1] = m.routes[1];
  for (uint8_t i = 0; i < 4; ++i) {
    snap.ir[i] = m.ir[i];
    snap.threshold[i] = ((m.floorDone && m.voidDone) || m.cal.manual(i))
                            ? m.cal.threshold(i) : 0;
    snap.autoThreshold[i] = (m.floorDone && m.voidDone) ? m.cal.autoThreshold(i) : 0;
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
  loadRoutes();
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
        case State::RoundEnd:
          robotStop();
          if (now - m.stateSince >= ROUND_END_MS) enterIdle("auto idle");
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
