/* Autonomous senior BottleSumo, 2026 Rev.1. See docs/competition/competition-firmware.md.
 * Pinout follows bottlesumo_v2_test; three XSHUT lines were confirmed separately.
 * Motors stay disabled until hardware confirmation, QRE calibration and BUTTON1.
 */
#include <Arduino.h>
#include <Wire.h>
#include <VL53L0X.h>
#include <Preferences.h>
#include "strategy.h"

using namespace BottleSumo;
constexpr uint8_t QRE_PINS[] = {1, 2, 4, 5}; // FL, FR, RL, RR, confirmed by user.
constexpr uint8_t PWM_PINS[] = {10, 13};
constexpr uint8_t DIR_PINS[] = {11, 14}; // Motor1 left, Motor2 right.
constexpr uint8_t FG_PINS[] = {9, 12};
constexpr uint8_t XSHUT_PINS[] = {6, 7, 15}; // Left, center, right.
constexpr uint8_t TOF_ADDRESSES[] = {0x30, 0x31, 0x32};
constexpr uint8_t I2C_SDA_PIN = 17, I2C_SCL_PIN = 16;
constexpr uint8_t VL53L0X_GPIO_PIN = 18; // Input only, as in the test sketch.
constexpr uint8_t STATUS_LED_PIN = 40, BUTTON1_PIN = 41, BUTTON2_PIN = 42;

// Set true only after the commissioning checks in the guide, then rebuild.
constexpr bool HARDWARE_CONFIRMED = false;
constexpr bool FORWARD_LEVEL[] = {HIGH, HIGH}; // Measure each wheel independently.
constexpr bool FG_CONNECTED = false; // Enable only after confirming ESP32-safe FG levels.
constexpr uint32_t PWM_HZ = 20000, CONTROL_MS = 5, REVERSAL_OFF_MS = 25;
constexpr uint32_t CALIBRATION_MS = 3000;

VL53L0X tof[3];
Range ranges[3];
Controller controller;
Calibration calibration;
Preferences preferences;
struct CalibrationRecord { uint32_t magic; Calibration values; };
constexpr uint32_t CALIBRATION_MAGIC = 0x42534D33; // BSM3: four corners, 12-bit ADC.
bool calibrated = false, floorCaptured = false, hardwareFault = false;
bool pwmAttached[2] = {};
uint16_t qre[4] = {};
uint8_t edges = 0, tofIndex = 0, targetHits[3] = {};
bool tofBusy = false, tofStarting = false;
uint8_t tofStopVariable[3] = {};
uint32_t tofStarted = 0, lastTofPoll = 0, lastControl = 0, lastReport = 0;
char calibrationMode = 0;
uint32_t calibrationStarted = 0;
struct MotorOutput { int8_t direction = -1; uint8_t duty = 0; uint32_t zeroSince = 0; };
MotorOutput motors[2];
volatile uint32_t fgEdges[2] = {};
portMUX_TYPE fgMux = portMUX_INITIALIZER_UNLOCKED;
void leftFgISR();
void rightFgISR();

bool writeDuty(uint8_t i, uint8_t duty) {
  if (!pwmAttached[i]) return duty == 0;
  if (ledcWrite(PWM_PINS[i], duty)) return true;
  ledcDetach(PWM_PINS[i]);
  pwmAttached[i] = false;
  pinMode(PWM_PINS[i], OUTPUT); digitalWrite(PWM_PINS[i], LOW);
  return false;
}

void stopMotors() {
  for (uint8_t i = 0; i < 2; ++i) {
    writeDuty(i, 0);
    if (motors[i].duty) motors[i].zeroSince = millis();
    motors[i].duty = 0;
  }
}

void fault(const char *reason) {
  stopMotors(); controller.stop(); hardwareFault = true;
  Serial.printf("FAULT: %s. Motors stopped; correct fault and reset.\n", reason);
}

bool setWheel(uint8_t i, int16_t requested, uint32_t now) {
  auto &motor = motors[i];
  if (!requested) {
    if (motor.duty) motor.zeroSince = now;
    motor.duty = 0; return writeDuty(i, 0);
  }
  const int8_t direction = requested > 0 ? 1 : -1;
  if (direction != motor.direction) {
    if (motor.duty) {
      motor.duty = 0; motor.zeroSince = now;
      return writeDuty(i, 0);
    }
    if (now - motor.zeroSince < REVERSAL_OFF_MS) return true;
    digitalWrite(DIR_PINS[i], direction > 0 ? FORWARD_LEVEL[i] : !FORWARD_LEVEL[i]);
    motor.direction = direction;
  }
  const uint8_t desired = static_cast<uint8_t>(constrain(abs(requested), 0, 255));
  motor.duty = desired > motor.duty ? min<int>(desired, motor.duty + 12) : desired;
  return writeDuty(i, motor.duty);
}

// Explicit byte-count checks keep a truncated I2C read from becoming a target.
bool readTofRegisters(uint8_t reg, uint8_t *data, uint8_t count) {
  const uint8_t address = TOF_ADDRESSES[tofIndex];
  Wire.beginTransmission(address); Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(address, count) != count) return false;
  for (uint8_t i = 0; i < count; ++i) data[i] = Wire.read();
  return true;
}

bool writeTofRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(TOF_ADDRESSES[tofIndex]);
  Wire.write(reg); Wire.write(value);
  return Wire.endTransmission() == 0;
}

void serviceTof(uint32_t now) {
  if (hardwareFault || now - lastTofPoll < 2) return;
  lastTofPoll = now;
  if (!tofBusy) {
    // Pololu 1.3.1 readRangeSingleMillimeters startup, split from its blocking wait.
    const uint8_t startup[][2] = {{0x80, 1}, {0xFF, 1}, {0, 0},
      {0x91, tofStopVariable[tofIndex]}, {0, 1}, {0xFF, 0}, {0x80, 0}, {0, 1}};
    for (const auto &reg : startup) {
      if (!writeTofRegister(reg[0], reg[1])) { fault("ToF start I2C"); return; }
    }
    tofStarted = millis(); tofBusy = tofStarting = true; return;
  }
  if (now - tofStarted > 80) { fault("ToF measurement timeout"); return; }
  if (tofStarting) {
    uint8_t start;
    if (!readTofRegisters(VL53L0X::SYSRANGE_START, &start, 1)) {
      fault("ToF start-status I2C"); return;
    }
    if (!(start & 1)) tofStarting = false;
    return;
  }
  uint8_t ready;
  if (!readTofRegisters(VL53L0X::RESULT_INTERRUPT_STATUS, &ready, 1)) {
    fault("ToF ready I2C"); return;
  }
  if (!(ready & 7)) return;
  uint8_t frame[12];
  if (!readTofRegisters(VL53L0X::RESULT_RANGE_STATUS, frame, sizeof(frame))) {
    fault("ToF frame I2C"); return;
  }
  if (!writeTofRegister(VL53L0X::SYSTEM_INTERRUPT_CLEAR, 1)) {
    fault("ToF clear I2C"); return;
  }
  const uint16_t mm = (static_cast<uint16_t>(frame[10]) << 8) | frame[11];
  // Raw device status 11 means range complete; this is not ST's processed status 0.
  const bool target = ((frame[0] & 0x78) >> 3) == 11 && mm >= 35 && mm <= 900;
  targetHits[tofIndex] = target ? min<int>(targetHits[tofIndex] + 1, 2) : 0;
  ranges[tofIndex] = {static_cast<uint16_t>(targetHits[tofIndex] >= 2 ? mm : 0), millis()};
  tofIndex = (tofIndex + 1) % 3; tofBusy = false;
}

void initTof() {
  for (auto pin : XSHUT_PINS) { pinMode(pin, OUTPUT); digitalWrite(pin, LOW); }
  delay(10);
  for (uint8_t i = 0; i < 3; ++i) {
    pinMode(XSHUT_PINS[i], INPUT); delay(10); // Release to the module's own XSHUT pull-up.
    tof[i].setTimeout(100);
    if (!tof[i].init()) { fault("ToF init / XSHUT wiring"); return; }
    tof[i].setAddress(TOF_ADDRESSES[i]);
    if (tof[i].last_status != 0 || !tof[i].setMeasurementTimingBudget(33000)) {
      fault("ToF address / timing budget"); return;
    }
    Wire.beginTransmission(TOF_ADDRESSES[i]);
    if (Wire.endTransmission() != 0) { fault("ToF assigned address missing"); return; }
    tofIndex = i;
    if (!writeTofRegister(0x80, 1) || !writeTofRegister(0xFF, 1) ||
        !writeTofRegister(0, 0) || !readTofRegisters(0x91, &tofStopVariable[i], 1) ||
        !writeTofRegister(0, 1) || !writeTofRegister(0xFF, 0) || !writeTofRegister(0x80, 0)) {
      fault("ToF single-shot setup I2C"); return;
    }
  }
  tofIndex = 0;
  // Only one head ranges at a time: no overlapping optical exposures.
}

void readFloor() {
  uint8_t nextEdges = 0;
  for (uint8_t i = 0; i < 4; ++i) {
    qre[i] = analogRead(QRE_PINS[i]);
    if (calibrated && calibration.edge(i, qre[i], edges & (1 << i))) nextEdges |= 1 << i;
  }
  edges = nextEdges;
}

void beginCalibration(char mode) {
  if (controller.state != State::Stopped || calibrationMode) return;
  stopMotors();
  if (mode == 'e' && !floorCaptured) { Serial.println("Capture floor with f first."); return; }
  calibrated = false; calibrationMode = mode; calibrationStarted = millis();
  for (uint8_t i = 0; i < 4; ++i) {
    (mode == 'f' ? calibration.floorMin : calibration.edgeMin)[i] = 4095;
    (mode == 'f' ? calibration.floorMax : calibration.edgeMax)[i] = 0;
  }
  Serial.println(mode == 'f' ? "3s FLOOR capture: include safe table and seam. Motors off."
                             : "3s VOID capture: all sensors see drop at normal height. Motors off.");
}

void serviceCalibration(uint32_t now) {
  if (!calibrationMode) return;
  for (uint8_t i = 0; i < 4; ++i) {
    auto &low = (calibrationMode == 'f' ? calibration.floorMin : calibration.edgeMin)[i];
    auto &high = (calibrationMode == 'f' ? calibration.floorMax : calibration.edgeMax)[i];
    low = min(low, qre[i]); high = max(high, qre[i]);
  }
  if (now - calibrationStarted < CALIBRATION_MS) return;
  if (calibrationMode == 'f') {
    floorCaptured = true; Serial.println("Floor captured. Capture void with e.");
  } else if ((calibrated = calibration.valid())) {
    CalibrationRecord record = {CALIBRATION_MAGIC, calibration};
    if (!preferences.begin("bottlesumo", false)) {
      calibrated = false; Serial.println("Calibration storage open failed.");
    } else {
      const bool saved = preferences.putBytes("qre", &record, sizeof(record)) == sizeof(record);
      preferences.end();
      if (!saved) calibrated = false;
      Serial.println(saved ? "Calibration saved." : "Calibration storage write failed.");
    }
  } else Serial.println("Calibration rejected: every floor/void gap must be >=300 ADC counts.");
  calibrationMode = 0;
}

void printStatus() {
  uint32_t left, right;
  portENTER_CRITICAL(&fgMux); left = fgEdges[0]; right = fgEdges[1]; portEXIT_CRITICAL(&fgMux);
  Serial.printf("state=%u confirmed=%d calibrated=%d fault=%d edges=0x%X QRE=%u,%u,%u,%u "
                "ToF(L,C,R)=%u,%u,%u FG=%lu,%lu\n",
                static_cast<unsigned>(controller.state), HARDWARE_CONFIRMED, calibrated,
                hardwareFault, edges, qre[0], qre[1], qre[2], qre[3],
                ranges[0].mm, ranges[1].mm, ranges[2].mm,
                static_cast<unsigned long>(left), static_cast<unsigned long>(right));
}

void setup() {
  for (uint8_t i = 0; i < 2; ++i) {
    pinMode(PWM_PINS[i], OUTPUT); digitalWrite(PWM_PINS[i], LOW);
    pinMode(DIR_PINS[i], OUTPUT); digitalWrite(DIR_PINS[i], !FORWARD_LEVEL[i]);
  }
  Serial.begin(115200);
  for (uint8_t i = 0; i < 2; ++i) pwmAttached[i] = ledcAttach(PWM_PINS[i], PWM_HZ, 8);
  stopMotors();
  if (!pwmAttached[0] || !pwmAttached[1]) fault("PWM attach");
  for (auto pin : QRE_PINS) pinMode(pin, INPUT);
  for (auto pin : FG_PINS) pinMode(pin, INPUT);
  if (FG_CONNECTED) {
    attachInterrupt(digitalPinToInterrupt(FG_PINS[0]), leftFgISR, RISING);
    attachInterrupt(digitalPinToInterrupt(FG_PINS[1]), rightFgISR, RISING);
  }
  pinMode(BUTTON1_PIN, INPUT); pinMode(BUTTON2_PIN, INPUT); // External pull-downs.
  pinMode(STATUS_LED_PIN, OUTPUT); digitalWrite(STATUS_LED_PIN, LOW);
  pinMode(VL53L0X_GPIO_PIN, INPUT);
  analogReadResolution(12);
  CalibrationRecord record = {};
  if (preferences.begin("bottlesumo", true)) {
    const size_t bytes = preferences.getBytes("qre", &record, sizeof(record));
    preferences.end();
    if (bytes == sizeof(record) && record.magic == CALIBRATION_MAGIC && record.values.valid()) {
      calibration = record.values; calibrated = floorCaptured = true;
    }
  }
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN); Wire.setClock(400000); Wire.setTimeOut(3);
  if (!hardwareFault) initTof();
  Serial.println("BottleSumo competition: f=floor, e=void, q=status, x=stop. BUTTON1=start, BUTTON2=stop.");
}

void loop() {
  // Bound serial work; no steering or tuning commands are accepted during a round.
  for (uint8_t i = 0; i < 8 && Serial.available(); ++i) {
    const char command = Serial.read();
    if (command == 'x') { controller.stop(); stopMotors(); calibrationMode = 0; }
    else if (controller.state == State::Stopped) {
      if (command == 'f' || command == 'e') beginCalibration(command);
      if (command == 'q') printStatus();
    }
  }
  if (digitalRead(BUTTON2_PIN)) {
    controller.stop(); stopMotors(); calibrationMode = 0;
  }
  const uint32_t now = millis();
  if (now - lastControl >= CONTROL_MS) {
    lastControl = now;
    readFloor(); serviceCalibration(now);
    static bool buttonStable = false, buttonRaw = false;
    static uint32_t buttonChanged = 0;
    const bool pressed = digitalRead(BUTTON1_PIN);
    if (pressed != buttonRaw) { buttonRaw = pressed; buttonChanged = now; }
    if (now - buttonChanged >= 30 && pressed != buttonStable) {
      buttonStable = pressed;
      if (pressed && !digitalRead(BUTTON2_PIN) && controller.state == State::Stopped &&
          !calibrationMode && !hardwareFault && calibrated && HARDWARE_CONFIRMED && !edges) {
        bool fresh = true;
        for (const auto &r : ranges) if (now - r.sampledMs > RANGE_STALE_MS || !r.sampledMs) fresh = false;
        if (fresh) controller.start(now);
      }
    }
    const Drive drive = controller.tick(now, edges, ranges, hardwareFault);
    if (!setWheel(0, drive.left, now) || !setWheel(1, drive.right, now)) fault("PWM write");
    const bool active = controller.state != State::Stopped;
    digitalWrite(STATUS_LED_PIN, hardwareFault ? (now / 100) % 2
                                             : (active ? HIGH : (calibrated ? (now / 500) % 2 : LOW)));
  }
  // Edge and stop handling runs before every bounded I2C service step.
  serviceTof(millis());
  if (controller.state == State::Stopped && now - lastReport >= 1000) {
    lastReport = now; printStatus();
  }
}
