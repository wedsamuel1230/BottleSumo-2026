// robot.cpp — motors, IR sampling and the single VL53L0X head.
#include "robot.h"

#include <Wire.h>
#include "config.h"
#include "vl53l0x.h"

namespace bs {
namespace {

struct Wheel {
  bool attached = false;
  int8_t direction = 0;   // 1 forward, -1 reverse, 0 unknown
  uint8_t duty = 0;       // commanded magnitude, 0..100
  uint32_t idleSince = 0;
};

Wheel wheels[2];

constexpr uint16_t idleDuty() { return (1u << PWM_BITS) - 1; }

uint16_t driverDuty(uint8_t pct) {
  const uint32_t span = (1u << PWM_BITS) - 1;
  const uint32_t on = (span * pct) / 100u;
  return uint16_t(PWM_ACTIVE_LOW[0] ? span - on : on);
}

void writeWheel(uint8_t i, uint8_t pct) {
  // PWM_ACTIVE_LOW is per driver; the duty inversion is the same bit width.
  const uint32_t span = (1u << PWM_BITS) - 1;
  const uint32_t on = (span * pct) / 100u;
  ledcWrite(PIN_PWM[i], uint16_t(PWM_ACTIVE_LOW[i] ? span - on : on));
}

void setDirection(uint8_t i, int8_t direction) {
  const bool forward = direction > 0;
  digitalWrite(PIN_DIR[i], forward ? FORWARD_LEVEL[i] : !FORWARD_LEVEL[i]);
  wheels[i].direction = direction;
}

// Percent command with a reversal dwell: never flip DIR while the driver is
// still conducting.
void applyWheel(uint8_t i, int8_t pct, uint32_t now) {
  Wheel &w = wheels[i];
  if (pct > 100) pct = 100;
  if (pct < -100) pct = -100;
  const int8_t want = pct > 0 ? 1 : (pct < 0 ? -1 : 0);
  const uint8_t magnitude = uint8_t(pct < 0 ? -pct : pct);

  if (want != 0 && w.direction != 0 && want != w.direction && w.duty != 0) {
    writeWheel(i, 0);
    w.duty = 0;
    w.idleSince = now;
    return;
  }
  if (want != w.direction && w.duty != 0) {
    writeWheel(i, 0);
    w.duty = 0;
    w.idleSince = now;
    return;
  }
  if (want != w.direction && now - w.idleSince < REVERSAL_OFF_MS) {
    writeWheel(i, 0);
    w.duty = 0;
    return;
  }
  if (want != w.direction) setDirection(i, want);
  writeWheel(i, magnitude);
  w.duty = magnitude;
}

// ---------------------------------------------------------------------------
// ToF
// ---------------------------------------------------------------------------
VL53L0X tof;
bool tofOk = false;
bool tofBusy = false;
bool tofStarted = false;
uint32_t tofStartedAt = 0;
uint8_t tofHits = 0;
uint16_t tofLast = 0;
const char *tofFail = nullptr;

bool i2cAck(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

// After a warm reset the module keeps the address the previous boot assigned,
// so probing 0x29 would fail forever. Move it back through register 0x8A.
void recoverAddress() {
  if (i2cAck(0x29)) return;
  for (uint8_t address = 0x30; address <= 0x32; ++address) {
    if (!i2cAck(address)) continue;
    Wire.beginTransmission(address);
    Wire.write(uint8_t(0x8A));
    Wire.write(uint8_t(0x29));
    Wire.endTransmission();
    delay(5);
    if (i2cAck(0x29)) return;
  }
}

} // namespace

void robotBegin() {
  // Park the PWM input at its resting level before anything else: with an
  // active-low driver a low output would run the wheel.
  for (uint8_t i = 0; i < 2; ++i) {
    pinMode(PIN_PWM[i], OUTPUT);
    digitalWrite(PIN_PWM[i], PWM_ACTIVE_LOW[i] ? HIGH : LOW);
    pinMode(PIN_DIR[i], OUTPUT);
    digitalWrite(PIN_DIR[i], !FORWARD_LEVEL[i]);
  }
  for (uint8_t pin : PIN_LED) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
  }
  for (uint8_t pin : PIN_IR) pinMode(pin, INPUT);
  pinMode(PIN_BUTTON_A, INPUT);
  pinMode(PIN_BUTTON_B, INPUT);

  wheels[0] = wheels[1] = Wheel{};
  for (uint8_t i = 0; i < 2; ++i) {
    wheels[i].attached = ledcAttach(PIN_PWM[i], PWM_HZ, PWM_BITS);
    writeWheel(i, 0);
  }

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  Wire.setTimeOut(5);
}

void robotStop() {
  for (uint8_t i = 0; i < 2; ++i) {
    writeWheel(i, 0);
    wheels[i].duty = 0;
  }
}

void motorCommand(int8_t left, int8_t right, uint32_t now) {
  applyWheel(0, left, now);
  applyWheel(1, right, now);
}

void irRead(uint16_t out[4]) {
  for (uint8_t i = 0; i < 4; ++i) out[i] = uint16_t(analogRead(PIN_IR[i]));
}

bool tofBegin() {
  tofFail = nullptr;
  pinMode(PIN_TOF_XSHUT, OUTPUT);
  digitalWrite(PIN_TOF_XSHUT, LOW);
  delay(10);
  recoverAddress();
  pinMode(PIN_TOF_XSHUT, INPUT); // the module's own pull-up releases reset
  delay(10);
  if (!tof.begin(Wire)) { tofFail = "probe 0x29"; return false; }
  if (!tof.init()) { tofFail = "init"; return false; }
  if (!tof.setAddress(TOF_ADDRESS)) { tofFail = "setAddress"; return false; }
  if (!tof.setMeasurementTimingBudget(33000)) { tofFail = "timing budget"; return false; }
  if (!tof.healthy()) { tofFail = "I2C transport"; return false; }
  tofOk = true;
  tofBusy = false;
  tofHits = 0;
  return true;
}

bool tofService(uint32_t now, uint16_t &mm) {
  if (!tofOk) return false;
  if (!tofBusy) {
    if (!tof.startSingle()) {
      tofOk = false;
      tofFail = "startSingle";
      return false;
    }
    tofBusy = true;
    tofStartedAt = now;
    return false;
  }
  if (now - tofStartedAt > TOF_TIMEOUT_MS) {
    tofOk = false;
    tofFail = "timeout";
    tofBusy = false;
    return false;
  }
  bool ready = false;
  if (!tof.dataReady(ready)) { tofOk = false; tofFail = "dataReady"; return false; }
  if (!ready) return false;

  uint16_t value = 0;
  uint8_t status = 0;
  if (!tof.readResult(value, status)) {
    tofOk = false;
    tofFail = "readResult";
    tofBusy = false;
    return false;
  }
  tofBusy = false;
  const bool valid = status == 11 && value >= TOF_MIN_MM && value <= TOF_MAX_MM;
  tofHits = valid ? uint8_t(min<int>(TOF_CONFIRM, tofHits + 1)) : 0;
  tofLast = tofHits >= TOF_CONFIRM ? value : 0;
  mm = tofLast;
  return true;
}

bool tofHealthy() { return tofOk; }
const char *tofError() { return tofFail ? tofFail : ""; }

} // namespace bs
