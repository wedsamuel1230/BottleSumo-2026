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
#include "config.h"
#include "state_machine.h"
#include "sensors.h"
#include "shared.h"
#include "telemetry.h"
#include "web.h"

using namespace BS;

namespace BS {
Live live;
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

// Bench drive (web sliders only; physically selected at boot with BUTTON2).
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
bool writeDuty(uint8_t i, uint8_t duty) {
  if (!pwmAttached[i]) return duty == 0;
  if (!ledcWrite(PIN_PWM[i], duty)) {
    // A driver failure must still remove the PWM signal from the pin.
    ledcDetach(PIN_PWM[i]);
    pwmAttached[i] = false;
    pinMode(PIN_PWM[i], OUTPUT);
    digitalWrite(PIN_PWM[i], LOW);
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
  const int desired = magnitude > 255 ? 255 : magnitude;
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
    case Command::Start:
      if (canStartRound()) machine.startRound();
      break;
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
  bool startRaw = false, startStable = false;
  uint32_t startChanged = 0;
  bool stopTracking = false, windowArmed = false;
  uint32_t stopHoldStart = 0;
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

    // ---- Buttons ----
    const bool stopHeld = digitalRead(PIN_BTN_STOP) == HIGH;
    const bool startHeld = digitalRead(PIN_BTN_START) == HIGH;
    if (startHeld != startRaw) { startRaw = startHeld; startChanged = now; }
    if (now - startChanged >= 30 && startHeld != startStable) {
      startStable = startHeld;
      if (startHeld && !stopHeld && machine.state == State::Fault) {
        // A budget/edge fault is cleared by the same press that starts the
        // round, so a misfire does not need a reboot.
        machine.clearFault();
        faulted = false;
      }
      if (startHeld && !stopHeld && canStartRound()) {
        bool fresh = tofHealthy;
        for (uint8_t i = 0; i < 3; ++i) {
          if (!tof.ranges[i].sampledMs || now - tof.ranges[i].sampledMs > RANGE_STALE_MS) fresh = false;
        }
        if (fresh) machine.startRound();
      }
    }

    // ---- OTA window: hold BUTTON2 for 2 s while stopped ----
    if (stopHeld) {
      machine.stopRound(Reason::ButtonStop);
      if (!stopTracking) { stopHoldStart = now; stopTracking = true; }
      lockConfig();
      if (!windowArmed && now - stopHoldStart >= 2000 && !live.updating) {
        live.otaWindowArmed = true;
        live.otaWindowUntil = now + OTA_WINDOW_MS;
        windowArmed = true;
      }
      unlockConfig();
    } else {
      stopTracking = windowArmed = false;
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
    Serial.println("ToF init failed (XSHUT wiring / I2C)");
  } else {
    tofHealthy = true;
    Serial.println("ToF: 3 heads initialised");
  }
  while (true) {
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
        } else if (used == 1 && line[0] == 'x') {
          sendRequest(Command::Stop);
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
    bool fresh = tofHealthy;
    for (uint8_t i = 0; i < 3; ++i) {
      if (!tof.ranges[i].sampledMs || millis() - tof.ranges[i].sampledMs > RANGE_STALE_MS) fresh = false;
    }
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
    pinMode(PIN_PWM[i], OUTPUT);
    digitalWrite(PIN_PWM[i], LOW);
    pinMode(PIN_DIR[i], OUTPUT);
    digitalWrite(PIN_DIR[i], !FORWARD_LEVEL[i]);
  }
  pinMode(PIN_BTN_START, INPUT);
  pinMode(PIN_BTN_STOP, INPUT);
  for (uint8_t pin : PIN_LED) { pinMode(pin, OUTPUT); digitalWrite(pin, LOW); }
  for (uint8_t i = 0; i < 4; ++i) pinMode(PIN_QRE[i], INPUT);
  for (uint8_t i = 0; i < 2; ++i) pinMode(PIN_FG[i], INPUT);

  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("BottleSumo 2026 — Arduino firmware");

  // Bench mode is a physical boot selection; the network cannot change it.
  lockConfig();
  live.benchMode = digitalRead(PIN_BTN_STOP) == HIGH;
  unlockConfig();
  if (live.benchMode) Serial.println("BENCH mode selected (BUTTON2 held at boot)");

  nvs_flash_init();

  psramOk = esp_psram_get_size() == PSRAM_EXPECTED_MB * 1024 * 1024;
  if (!psramOk) {
    faulted = true;
    Serial.printf("PSRAM check failed (expected %u MB)\n", PSRAM_EXPECTED_MB);
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
