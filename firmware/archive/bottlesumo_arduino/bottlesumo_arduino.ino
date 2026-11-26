// bottlesumo_arduino.ino — BottleSumo 2026 on ESP32-S3 N16R8, Arduino IDE.
//
// This is the Arduino port of firmware/bottlesumo_idf. Same pinout, same
// three-head ToF schedule, same competition strategy, plus a configuration
// web UI, self-calibration and OTA with rollback.
//
// Task split. Core 1 is reserved for the two hard real-time tasks, so Wi-Fi
// and HTTP work on core 0 can never delay a control tick or a ranging slot:
//   control  core 1, prio 20, 5 ms   QRE ADC, state machine, PWM, telemetry
//   tof      core 1, prio 10, 1 ms   VL53L0X single-shot schedule, no blocking
//   web      core 0, prio 3          HTTP UI, UDP telemetry, OTA window
//   service  core 0, prio 2          Serial CLI and NVS writes
// Only controlTask ever calls setWheel(); every other task talks to it
// through a queue or through `live` under configMutex.
#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_task_wdt.h>
#include <esp_ota_ops.h>
#include <esp_psram.h>
#include <esp_random.h>
#include <nvs_flash.h>
#include <Preferences.h>
#include "config.h"
#include "state_machine.h"
#include "sensors.h"
#include "shared.h"
#include "telemetry.h"
#include "web.h"

using namespace BS;

namespace BS {

void saveTrimTable() {
  Preferences prefs;
  if (!prefs.begin("trimtab", false)) return;
  prefs.putBytes("t", trimTable.trim, sizeof(trimTable.trim));
  prefs.end();
}

void loadTrimTable() {
  Preferences prefs;
  if (prefs.begin("trimtab", true)) {
    prefs.getBytes("t", trimTable.trim, sizeof(trimTable.trim));
    prefs.end();
  }
  for (uint8_t w = 0; w < 2; ++w) {
    for (uint8_t k = 0; k < TrimTable::N; ++k) {
      if (trimTable.trim[w][k] > 40) trimTable.trim[w][k] = 40;
      if (trimTable.trim[w][k] < -40) trimTable.trim[w][k] = -40;
    }
  }
}

Live live;
TrimTable trimTable;
volatile bool tofRestartRequest = false;
I2cProbeResult i2cProbe;
QueueHandle_t telemetryQueue = nullptr;
QueueHandle_t requestQueue = nullptr;
SemaphoreHandle_t configMutex = nullptr;
} // namespace BS

namespace {

Machine machine;
TofState tof;
EdgeFilter edgeFilter;
uint16_t qre[4] = {};
uint8_t edges = 0;
uint32_t bootId = 0;
bool psramOk = false;
bool tofHealthy = false;
bool pwmOk = false;
bool faulted = false;
bool storagePending = false;
bool clearing = false;
uint8_t lastHistoryHead = 0;
uint8_t lastHistoryCount = 0;
uint8_t lastDisplay = 255;
uint32_t stateChangedMs = 0;

// Bench drive (web sliders only; physically selected at boot with SW2).
int16_t benchDrive[2] = {};
uint32_t benchLease = 0;

struct MotorOutput { int8_t direction = -1; uint8_t duty = 0; uint32_t zeroSince = 0; };
MotorOutput motors[2];
bool pwmAttached[2] = {};
volatile uint32_t fgCount[2] = {};

struct SaveRequest { uint8_t kind; CalibrationRecord cal; TuningRecord tuning; };
QueueHandle_t saveQueue = nullptr;

// ---------------------------------------------------------------------------
// Motor output
// ---------------------------------------------------------------------------
// Resting level of the driver's PWM input. An active-low input switches on
// when the pin is low, so the pin must sit high whenever no speed is
// commanded, including the not-attached and detach failure paths.
inline uint8_t idlePwmLevel(uint8_t i) { return PWM_ACTIVE_LOW[i] ? HIGH : LOW; }

// The polarity is applied in duty space, not with ledcOutputInvert(): the
// written duty stays a plain number that is easy to check against the pin.
inline uint8_t driverDuty(uint8_t i, uint8_t duty) {
  return PWM_ACTIVE_LOW[i] ? uint8_t(255 - duty) : duty;
}

bool writeDuty(uint8_t i, uint8_t duty) {
  if (!pwmAttached[i]) return duty == 0;
  if (!ledcWrite(PIN_PWM[i], driverDuty(i, duty))) {
    // A driver failure must still remove the PWM signal from the pin.
    ledcDetach(PIN_PWM[i]);
    pwmAttached[i] = false;
    pinMode(PIN_PWM[i], OUTPUT);
    digitalWrite(PIN_PWM[i], idlePwmLevel(i));
    return false;
  }
  return true;
}

void stopOutputs() {
  for (uint8_t i = 0; i < 2; ++i) {
    motors[i].duty = 0;
    writeDuty(i, 0);
  }
}

bool setWheel(uint8_t i, int requested, uint32_t now) {
  MotorOutput &m = motors[i];
  if (!requested) {
    if (m.duty) m.zeroSince = now;
    m.duty = 0;
    return writeDuty(i, 0);
  }
  const int direction = requested > 0 ? 1 : -1;
  if (direction != m.direction) {
    if (m.duty) { m.duty = 0; m.zeroSince = now; return writeDuty(i, 0); }
    if (now - m.zeroSince < REVERSAL_OFF_MS) return true;   // dead time
    digitalWrite(PIN_DIR[i], direction > 0 ? FORWARD_LEVEL[i] : !FORWARD_LEVEL[i]);
    m.direction = int8_t(direction);
  }
  const int magnitude = requested < 0 ? -requested : requested;
  // Fixed per-wheel offset from the bench trim table; the FG tacho is not
  // usable on this build, so this is the only wheel matching.
  const int biased = magnitude + int(trimTable.at(i, magnitude));
  const int desired = biased < 0 ? 0 : (biased > 255 ? 255 : biased);
  m.duty = uint8_t(desired > m.duty
                   ? (int(m.duty) + RAMP_STEP < desired ? int(m.duty) + RAMP_STEP : desired)
                   : desired);
  return writeDuty(i, m.duty);
}

// ---------------------------------------------------------------------------
// Commands from the web / serial UI
// ---------------------------------------------------------------------------
bool canStartRound() {
  lockConfig();
  const bool ok = machine.state == State::Stopped && live.calibrated &&
                  live.floorCaptured && !live.updating && !live.benchMode &&
                  !faulted && HARDWARE_CONFIRMED && edges == 0;
  unlockConfig();
  return ok;
}

void beginCalibration(char mode, uint32_t now) {
  lockConfig();
  const bool idle = (machine.state == State::Stopped) && !live.updating;
  const bool alreadyCapturing = live.calibrationMode != 0;
  const bool floorOk = (mode == 'f') || live.floorCaptured;
  if (idle && !alreadyCapturing && floorOk) {
    live.calibrationMode = mode;
    live.calibrationStart = now;
    live.calibration.resetCapture(mode == 'f');
    unlockConfig();
    machine.stopRound(Reason::CalibrationCancelled);
    return;
  }
  unlockConfig();
}

void handleRequest(const Request &r, uint32_t now) {
  switch (r.command) {
    case Command::Start: {
      if (!canStartRound()) {
        lockConfig();
        const bool calibratedNow = live.calibrated;
        unlockConfig();
        Serial.printf("START refused: state=%s confirmed=%d calibrated=%d fault=%d\n",
                      stateName(machine.state), HARDWARE_CONFIRMED, calibratedNow,
                      faulted);
        break;
      }
      if (!(tofHealthy && tof.fresh(now))) {
        Serial.println("START refused: ToF has no fresh sample");
        break;
      }
      const MapType map = r.a == 1 ? MapType::SeniorB
                        : r.a == 2 ? MapType::SingleBoard
                                   : MapType::SeniorA;
      const Reason why = r.why == Reason::None ? Reason::WebStart : r.why;
      machine.startRound(map, why);
      lockConfig();
      live.mapType = uint8_t(map);
      unlockConfig();
      Serial.printf("START %s via %s\n", mapName(map), reasonName(why));
      break;
    }
    case Command::Stop:
      machine.stopRound(Reason::WebStop);
      break;
    case Command::ClearFault:
      faulted = false;
      machine.clearFault();
      break;
    case Command::CalibrateFloor:
      beginCalibration('f', now);
      break;
    case Command::CalibrateVoid:
      beginCalibration('e', now);
      break;
    case Command::CalibrateCancel:
      lockConfig();
      live.calibrationMode = 0;
      unlockConfig();
      break;
    case Command::ApplyTuning:
      lockConfig();
      machine.tuning = live.tuning;
      unlockConfig();
      break;
    case Command::BenchDrive:
      lockConfig();
      if (live.benchMode) {
        benchDrive[0] = r.a;
        benchDrive[1] = r.b;
        benchLease = now;
      }
      unlockConfig();
      break;
    case Command::Reset:
      ESP.restart();
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Control task
// ---------------------------------------------------------------------------
void controlTask(void *arg) {
  (void)arg;
  esp_task_wdt_add(nullptr);
  TickType_t wake = xTaskGetTickCount();
  uint32_t lastBegin = millis();
  uint32_t lastControlMax = 0;
  uint32_t deadlineMisses = 0;
  uint32_t sequence = 0;
  struct ButtonState { bool raw = false, stable = false; uint32_t changed = 0; };
  ButtonState switches[2];
  bool startPending[2] = {false, false};
  TaskHealth health;

  while (true) {
    const uint32_t began = millis();
    const uint32_t now = began;

    // ---- QRE sample + edge detection ----
    readQre(qre);
    lockConfig();
    const bool calibrated = live.calibrated;
    const QreCalibration cal = live.calibration;
    unlockConfig();
    uint8_t rawEdges = 0;
    if (calibrated) {
      for (uint8_t i = 0; i < 4; ++i) {
        if (cal.edge(i, qre[i], (edges >> i) & 1)) rawEdges |= uint8_t(1 << i);
      }
    }
    // Debounce: a raw edge has to hold for edgeDebounceMs before the state
    // machine sees it. One noisy sample is not a table edge.
    edgeFilter.confirmMs = machine.tuning.edgeDebounceMs;
    edges = edgeFilter.update(now, rawEdges);

    // ---- Switches: SW2 starts map A, SW3 starts map B; either stops ----
    // A start fires on release, so the same press cannot look like an
    // emergency stop on the next tick. A press during a round stops at once.
    static const uint8_t kSwitchPins[2] = {PIN_SW2, PIN_SW3};
    const bool stopHeld = digitalRead(PIN_SW2) == HIGH || digitalRead(PIN_SW3) == HIGH;
    for (uint8_t i = 0; i < 2; ++i) {
      ButtonState &b = switches[i];
      const bool held = digitalRead(kSwitchPins[i]) == HIGH;
      if (held != b.raw) { b.raw = held; b.changed = now; }
      if (now - b.changed < 30 || held == b.stable) continue;
      b.stable = held;

      if (held) {
        if (machine.active()) {
          machine.stopRound(Reason::ButtonStop);
        } else {
          if (machine.state == State::Fault) {
            // A budget/edge fault is cleared by the same press that starts
            // the round, so a misfire does not need a reboot.
            machine.clearFault();
            faulted = false;
          }
          startPending[i] = true;
        }
        continue;
      }
      if (!startPending[i]) continue;
      startPending[i] = false;

      const MapType map = i == 0 ? MapType::SeniorA : MapType::SeniorB;
      if (!canStartRound()) {
        // A refused start must say why; otherwise the robot looks dead.
        lockConfig();
        const bool floorCaptured = live.floorCaptured;
        const bool benchMode = live.benchMode;
        const bool updating = live.updating;
        unlockConfig();
        Serial.printf("START refused: state=%s confirmed=%d calibrated=%d "
                      "floor=%d bench=%d updating=%d fault=%d edges=0x%X tof=%d\n",
                      stateName(machine.state), HARDWARE_CONFIRMED, calibrated,
                      floorCaptured, benchMode, updating, faulted, edges,
                      tofHealthy && tof.fresh(now));
      } else if (!(tofHealthy && tof.fresh(now))) {
        Serial.println("START refused: ToF has no fresh sample");
      } else {
        machine.startRound(map, Reason::ButtonStart);
        lockConfig();
        live.mapType = uint8_t(map);
        unlockConfig();
        Serial.printf("START %s via %s\n", mapName(map), i == 0 ? "SW2" : "SW3");
      }
    }

    // ---- Commands ----
    Request request;
    while (xQueueReceive(requestQueue, &request, 0)) handleRequest(request, now);

    // ---- Calibration capture (motors stay at zero) ----
    lockConfig();
    if (live.calibrationMode && !live.updating) {
      live.calibration.sample(live.calibrationMode == 'f', qre);
      if (now - live.calibrationStart >= CALIBRATION_MS) {
        if (live.calibrationMode == 'f') {
          live.floorCaptured = true;
          live.calibrationMode = 0;
          Serial.println("floor captured; now capture the void with 'e'");
        } else if (live.calibration.valid()) {
          SaveRequest save{1, CalibrationRecord{CAL_MAGIC, live.calibration}, TuningRecord{}};
          storagePending = true;
          clearing = true;
          xQueueOverwrite(saveQueue, &save);
          live.calibrationMode = 0;
        } else {
          live.calibrationMode = 0;
          Serial.println("calibration rejected: every floor/void gap must be >= 300 counts");
        }
      }
    }
    if (clearing && !storagePending) {
      clearing = false;
      live.calibrated = live.calibration.valid() && !faulted;
    }
    const char calMode = live.calibrationMode;
    unlockConfig();

    // ---- Decision ----
    Drive requested{};
    lockConfig();
    const bool updating = live.updating;
    const bool bench = live.benchMode;
    const bool benchActiveNow = bench && (now - benchLease < 300) &&
                                (benchDrive[0] || benchDrive[1]);
    const bool armed = live.otaWindowArmed;
    unlockConfig();

    if (updating) {
      machine.stopRound(Reason::OtaStart);
      stopOutputs();
    } else if (bench) {
      if (benchActiveNow && !stopHeld) requested = Drive{benchDrive[0], benchDrive[1]};
      else if (now - benchLease >= 300) { benchDrive[0] = benchDrive[1] = 0; }
    } else {
      machine.beginFrame(now, edges, qre);
      requested = machine.tick(tof.ranges, stopHeld || faulted);
    }

    if (!setWheel(0, requested.left, now) || !setWheel(1, requested.right, now)) {
      stopOutputs();
      faulted = true;
    }

    // ---- Telemetry ----
    uint8_t display = stateDisplay(machine.state);
    if (machine.state == State::Fault) display = 25;
    else if (updating) display = DISPLAY_UPDATING;
    else if (calMode == 'f') display = DISPLAY_CAL_FLOOR;
    else if (calMode == 'e') display = DISPLAY_CAL_VOID;
    else if (bench) display = benchActiveNow ? DISPLAY_BENCH_DRIVE : DISPLAY_BENCH_IDLE;
    else if (machine.state == State::Search) {
      display = (tof.ranges[0].mm || tof.ranges[2].mm)
                ? DISPLAY_TARGET_SEEN
                : searchDisplayPhase(now, machine.roundStart);
    }
    if (display != lastDisplay) { lastDisplay = display; stateChangedMs = now; }

    Telemetry t{};
    t.magic = TELEMETRY_MAGIC;
    t.version = PROTOCOL_VERSION;
    t.size = sizeof(Telemetry);
    t.boot = bootId;
    t.sequence = sequence++;
    t.uptime_ms = now;
    t.state = display;
    t.state_elapsed_ms = now - stateChangedMs;
    t.edges = edges;
    const bool showFault = faulted || machine.state == State::Fault;
    t.flags = uint8_t((showFault ? Fault : 0) |
                      (calibrated ? Calibrated : 0) |
                      (HARDWARE_CONFIRMED ? HardwareConfirmed : 0) |
                      (updating ? Updating : 0) |
                      (bench ? Bench : 0) |
                      (armed ? OtaWindow : 0));
    for (uint8_t i = 0; i < 4; ++i) t.qre[i] = qre[i];
    for (uint8_t i = 0; i < 3; ++i) {
      t.distance[i] = tof.ranges[i].mm;
      t.range_age_ms[i] = uint16_t(now - tof.ranges[i].sampledMs < 65535
                                   ? now - tof.ranges[i].sampledMs : 65535);
    }
    t.requested[0] = requested.left;
    t.requested[1] = requested.right;
    for (uint8_t i = 0; i < 2; ++i) {
      t.applied[i] = int16_t(motors[i].direction * motors[i].duty);
      t.fg[i] = fgCount[i];
    }
    t.control_max_us = lastControlMax;
    t.deadline_misses = deadlineMisses;

    const uint32_t duration = millis() - began;
    if (duration > lastControlMax) lastControlMax = duration;
    if (duration > 5000 || now - lastBegin > 7) deadlineMisses++;
    lastBegin = now;

    xQueueOverwrite(telemetryQueue, &t);

    // ---- History mirror for the web task ----
    if (machine.history.head != lastHistoryHead || machine.history.count != lastHistoryCount) {
      lastHistoryHead = machine.history.head;
      lastHistoryCount = machine.history.count;
      lockConfig();
      live.history = machine.history;
      unlockConfig();
    }

    // ---- Fault + escape mirror ----
    lockConfig();
    live.faulted = showFault;
    live.escapeCount = machine.escapeCount;
    unlockConfig();

    // ---- Status LED ----
    const bool ledOn = showFault ? ((now / 100) % 2)
                                 : (machine.state != State::Stopped);
    for (uint8_t pin : PIN_LED) digitalWrite(pin, ledOn);

    health.sample(0, now);
    esp_task_wdt_reset();
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(CONTROL_PERIOD_MS));
  }
}

// ---------------------------------------------------------------------------
// ToF task
// ---------------------------------------------------------------------------
void tofTask(void *arg) {
  (void)arg;
  TickType_t wake = xTaskGetTickCount();
  TaskHealth health;
  if (!tof.begin()) {
    faulted = true;
    if (tof.failHead < 3) {
      Serial.printf("ToF init failed (XSHUT wiring / I2C): head %u, GPIO%u, %s\n",
                    tof.failHead, PIN_XSHUT[tof.failHead],
                    tof.fail ? tof.fail : "begin");
    } else {
      Serial.println("ToF init failed (XSHUT wiring / I2C)");
    }
    lockConfig();
    snprintf(live.tofError, sizeof live.tofError, "%s", tof.fail ? tof.fail : "begin");
    live.tofFailHead = tof.failHead;
    unlockConfig();
  } else {
    tofHealthy = true;
    Serial.printf("ToF: %u head(s) initialised\n", TOF_HEAD_COUNT);
  }
  while (true) {
    if (i2cProbe.request) {
      i2cProbe.request = false;
      i2cProbe.ready = false;
      for (uint8_t a = 8; a < 120; ++a) {
        Wire.beginTransmission(a);
        i2cProbe.code[a - 8] = Wire.endTransmission();
      }
      i2cProbe.model29 = -1;
      Wire.beginTransmission(0x29);
      Wire.write(uint8_t(0xC0));
      if (Wire.endTransmission(false) == 0 && Wire.requestFrom(uint8_t(0x29), uint8_t(1)) == 1) {
        i2cProbe.model29 = Wire.read();
      }
      i2cProbe.ready = true;
    }
    if (tofRestartRequest) {
      tofRestartRequest = false;
      tofHealthy = tof.begin();
      faulted = !tofHealthy;
      lockConfig();
      live.tofError[0] = 0;
      live.tofFailHead = 0xFF;
      if (!tofHealthy) {
        snprintf(live.tofError, sizeof live.tofError, "%s", tof.fail ? tof.fail : "begin");
        live.tofFailHead = tof.failHead;
      }
      unlockConfig();
      Serial.printf("ToF restart: %s\n", tofHealthy ? "ok" : "failed");
    }
    lockConfig();
    const bool updating = live.updating;
    unlockConfig();
    if (updating) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    const uint32_t now = millis();
    if (!tof.service(now)) {
      faulted = true;
      tofHealthy = false;
      vTaskDelay(pdMS_TO_TICKS(200));
    }
    health.sample(1, now);
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(1));
  }
}

// ---------------------------------------------------------------------------
// Service task: Serial CLI + NVS writes
// ---------------------------------------------------------------------------
void serviceTask(void *arg) {
  (void)arg;
  char line[128] = {};
  size_t used = 0;
  TaskHealth health;
  while (true) {
    health.sample(3, millis());
    SaveRequest save;
    if (xQueueReceive(saveQueue, &save, pdMS_TO_TICKS(10))) {
      bool ok = false;
      if (save.kind == 1) ok = saveCalibration(save.cal.values);
      else if (save.kind == 2) ok = saveTuning(save.tuning.values);
      lockConfig();
      if (save.kind == 1) {
        live.calibration = save.cal.values;
        live.calibrated = ok && !faulted;
        live.floorCaptured = true;
      }
      storagePending = false;
      unlockConfig();
      Serial.println(ok ? "saved to NVS" : "NVS write failed");
    }

    while (Serial.available()) {
      const char c = char(Serial.read());
      if (c == '\r') continue;
      if (c == '\n') {
        line[used] = 0;
        if (used == 1 && (line[0] == 'f' || line[0] == 'e')) {
          sendRequest(line[0] == 'f' ? Command::CalibrateFloor : Command::CalibrateVoid);
        } else if (used == 1 && line[0] == '1') {
          sendRequest(Command::Start, 0, 0, Reason::SerialStart);
        } else if (used == 1 && line[0] == '2') {
          sendRequest(Command::Start, 1, 0, Reason::SerialStart);
        } else if (used == 1 && line[0] == '3') {
          sendRequest(Command::Start, 2, 0, Reason::SerialStart);
        } else if (used == 1 && line[0] == 'x') {
          sendRequest(Command::Stop, 0, 0, Reason::SerialStop);
        } else if (used == 1 && line[0] == 'c') {
          sendRequest(Command::ClearFault);
        } else if (used == 1 && (line[0] == 'q' || line[0] == '?')) {
          Telemetry t{};
          xQueuePeek(telemetryQueue, &t, 0);
          Serial.printf("state=%u elapsed=%lu flags=%u edges=0x%X QRE=%u,%u,%u,%u "
                        "ToF=%u,%u,%u applied=%d,%d max_us=%lu misses=%lu\n",
                        t.state, (unsigned long)t.state_elapsed_ms, t.flags, t.edges,
                        t.qre[0], t.qre[1], t.qre[2], t.qre[3],
                        t.distance[0], t.distance[1], t.distance[2],
                        t.applied[0], t.applied[1],
                        (unsigned long)t.control_max_us, (unsigned long)t.deadline_misses);
          Serial.println("1=senior A, 2=senior B, 3=single board, x=stop, "
                         "c=clear fault, f=floor, e=void, q=status");
        } else if (used == 1) {
          Serial.println("unknown command; 1, 2, 3, x, c, f, e, q");
        }
        used = 0;
      } else if (used < sizeof(line) - 1) {
        line[used++] = c;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Boot verification for OTA rollback
// ---------------------------------------------------------------------------
void bootVerification() {
  esp_ota_img_states_t otaState;
  if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &otaState) != ESP_OK) return;
  if (otaState != ESP_OTA_IMG_PENDING_VERIFY) {
    lockConfig();
    live.bootVerified = true;
    unlockConfig();
    return;
  }
  Serial.println("OTA image pending verification, running self-test...");
  bool healthy = false;
  const uint32_t start = millis();
  while (millis() - start < BOOT_VERIFY_MS) {
    const bool fresh = tofHealthy && tof.fresh(millis());
    lockConfig();
    const bool webUp = live.webTicks > 0;   // web task has run
    unlockConfig();
    if (!faulted && psramOk && fresh && webUp) { healthy = true; break; }
    delay(50);
  }
  if (healthy) {
    const esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
    Serial.println(result == ESP_OK ? "self-test passed; image confirmed"
                                    : "self-test passed but confirm failed");
  } else {
    Serial.println("self-test failed; rolling back to the previous image");
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
  lockConfig();
  live.bootVerified = healthy;
  unlockConfig();
}

} // namespace

void setup() {
  for (uint8_t i = 0; i < 2; ++i) {
    // Park the PWM input at its resting level first: with an active-low
    // driver, a low output would run the wheel.
    pinMode(PIN_PWM[i], OUTPUT);
    digitalWrite(PIN_PWM[i], idlePwmLevel(i));
    pinMode(PIN_DIR[i], OUTPUT);
    digitalWrite(PIN_DIR[i], !FORWARD_LEVEL[i]);
  }
  pinMode(PIN_SW2, INPUT);
  pinMode(PIN_SW3, INPUT);
  for (uint8_t pin : PIN_LED) { pinMode(pin, OUTPUT); digitalWrite(pin, LOW); }
  for (uint8_t i = 0; i < 4; ++i) pinMode(PIN_QRE[i], INPUT);
  for (uint8_t i = 0; i < 2; ++i) pinMode(PIN_FG[i], INPUT);

  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("BottleSumo 2026 — Arduino firmware");

  // Bench mode is a physical boot selection; the network cannot change it.
  lockConfig();
  live.benchMode = digitalRead(PIN_SW2) == HIGH;
  unlockConfig();
  if (live.benchMode) Serial.println("BENCH mode selected (SW2 held at boot)");

  nvs_flash_init();
  loadTrimTable();

  psramOk = esp_psram_get_size() == PSRAM_EXPECTED_MB * 1024 * 1024;
  if (!psramOk) {
    faulted = true;
    Serial.printf("PSRAM check failed (expected %lu MB)\n",
                  static_cast<unsigned long>(PSRAM_EXPECTED_MB));
  }

  lockConfig();
  if (loadCalibration(live.calibration)) {
    live.calibrated = live.floorCaptured = true;
    Serial.println("calibration loaded from NVS");
  } else {
    Serial.println("no usable calibration in NVS");
  }
  if (loadTuning(live.tuning)) Serial.println("drive tuning loaded from NVS");
  live.bootId = bootId = esp_random();
  unlockConfig();
  machine.tuning = live.tuning;

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  pwmOk = ledcAttach(PIN_PWM[0], PWM_HZ, PWM_RES_BITS) &&
          ledcAttach(PIN_PWM[1], PWM_HZ, PWM_RES_BITS);
  pwmAttached[0] = pwmAttached[1] = pwmOk;
  if (!pwmOk) {
    faulted = true;
    Serial.println("PWM attach failed");
  }
  stopOutputs();
  Serial.printf("Motors: %lu Hz, %u-bit, active-low=%d,%d, attached=%d,%d, "
                "confirmed=%d calibrated=%d bench=%d sw2=%d sw3=%d\n",
                static_cast<unsigned long>(PWM_HZ), PWM_RES_BITS,
                PWM_ACTIVE_LOW[0], PWM_ACTIVE_LOW[1],
                pwmAttached[0], pwmAttached[1], HARDWARE_CONFIRMED,
                live.calibrated, live.benchMode,
                digitalRead(PIN_SW2), digitalRead(PIN_SW3));

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  Wire.setTimeOut(5);

  telemetryQueue = xQueueCreate(1, sizeof(Telemetry));
  requestQueue = xQueueCreate(8, sizeof(Request));
  saveQueue = xQueueCreate(1, sizeof(SaveRequest));
  configMutex = xSemaphoreCreateMutex();
  if (!telemetryQueue || !requestQueue || !saveQueue || !configMutex) {
    Serial.println("queue creation failed; halting");
    while (true) delay(1000);
  }

  webBegin();

  // Core 1: hard real-time pair only. Core 0: Wi-Fi/lwIP plus the soft tasks.
  xTaskCreatePinnedToCore(controlTask, "control", STACK_CONTROL, nullptr, PRIO_CONTROL, nullptr, CORE_REALTIME);
  xTaskCreatePinnedToCore(tofTask, "tof", STACK_TOF, nullptr, PRIO_TOF, nullptr, CORE_REALTIME);
  xTaskCreatePinnedToCore(webTask, "web", STACK_WEB, nullptr, PRIO_WEB, nullptr, CORE_NETWORK);
  xTaskCreatePinnedToCore(serviceTask, "service", STACK_SERVICE, nullptr, PRIO_SERVICE, nullptr, CORE_NETWORK);

  // Runs after the workers start so the ToF and web tasks can produce the
  // evidence the self-test needs.
  bootVerification();
}

void loop() {
  delay(100);
}
