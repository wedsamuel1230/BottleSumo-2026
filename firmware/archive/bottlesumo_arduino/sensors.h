// sensors.h — QRE sampling, self-calibration, persistence, and the VL53L0X
// ranging task logic. No game logic lives here.
#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "config.h"
#include "qre.h"
#include "state_machine.h"
#include "vl53l0x.h"

namespace BS {

// ---------------------------------------------------------------------------
// QRE1113 edge sensors
// ---------------------------------------------------------------------------
inline void readQre(uint16_t out[4]) {
  for (uint8_t i = 0; i < 4; ++i) out[i] = uint16_t(analogRead(PIN_QRE[i]));
}

struct CalibrationRecord { uint32_t magic; QreCalibration values; };

inline bool loadCalibration(QreCalibration &out) {
  Preferences prefs;
  if (!prefs.begin("bottlesumo", true)) return false;
  CalibrationRecord record{};
  const size_t bytes = prefs.getBytes("qre", &record, sizeof(record));
  prefs.end();
  if (bytes != sizeof(record) || record.magic != CAL_MAGIC || !record.values.valid()) return false;
  out = record.values;
  return true;
}

inline bool saveCalibration(const QreCalibration &values) {
  if (!values.valid()) return false;
  Preferences prefs;
  if (!prefs.begin("bottlesumo", false)) return false;
  CalibrationRecord record{CAL_MAGIC, values};
  const size_t written = prefs.putBytes("qre", &record, sizeof(record));
  prefs.end();
  return written == sizeof(record);
}

// ---------------------------------------------------------------------------
// Tuning persistence (the "voltage" knobs)
// ---------------------------------------------------------------------------
struct TuningRecord { uint32_t magic; Tuning values; };

inline bool loadTuning(Tuning &out) {
  Preferences prefs;
  if (!prefs.begin("bottlesumo", true)) return false;
  TuningRecord record{};
  const size_t bytes = prefs.getBytes("tuning", &record, sizeof(record));
  prefs.end();
  if (bytes != sizeof(record) || record.magic != TUNING_MAGIC) return false;
  if (record.values.powerScale == 0 || record.values.powerScale > 100) return false;
  if (record.values.maxPwm == 0 || record.values.maxPwm > 255) return false;
  out = record.values;
  return true;
}

inline bool saveTuning(const Tuning &values) {
  Preferences prefs;
  if (!prefs.begin("bottlesumo", false)) return false;
  TuningRecord record{TUNING_MAGIC, values};
  const size_t written = prefs.putBytes("tuning", &record, sizeof(record));
  prefs.end();
  return written == sizeof(record);
}

// ---------------------------------------------------------------------------
// VL53L0X time-of-flight heads
//
// Only one head ranges at a time — no overlapping optical exposures. Which
// heads exist is fixed by TOF_HEADS in config.h; the schedule only ever visits
// those, and the center head keeps two slots when three heads are fitted so the
// front target is acquired earlier.
// ---------------------------------------------------------------------------
struct TofState {
  VL53L0X sensor[3];
  Range ranges[3];
  uint8_t index = 0;
  uint8_t hits[3] = {};
  uint8_t schedule[4] = {};
  uint8_t scheduleLen = 0;
  uint8_t slot = 0;
  uint32_t started = 0;
  bool busy = false;
  const char *fail = nullptr; // failing stage in begin(), for the serial log
  uint8_t failHead = 0xFF;

  bool begin() {
    static_assert(TOF_HEAD_COUNT >= 1 && TOF_HEAD_COUNT <= 3,
                  "TOF_HEADS must list one to three heads");
    // Target tracking reads the center reading only; side heads add bearing.
    constexpr bool centerFitted = [] {
      for (uint8_t k = 0; k < TOF_HEAD_COUNT; ++k)
        if (TOF_HEADS[k] == 1) return true;
      return false;
    }();
    static_assert(centerFitted, "TOF_HEADS must include the center head (1)");
    fail = nullptr;
    failHead = 0xFF;
    // XSHUT may not power-cycle the module on this board (a hard pull-up or a
    // missing connection). After an MCU reset the sensor then still answers at
    // the address the previous boot assigned, so probing 0x29 fails forever.
    // Move a stray module back to 0x29 through its address register (0x8A)
    // before the normal probe.
    auto ack = [](uint8_t address) {
      Wire.beginTransmission(address);
      return Wire.endTransmission() == 0;
    };
    if (!ack(0x29)) {
      for (uint8_t address = 0x30; address <= 0x32; ++address) {
        if (!ack(address)) continue;
        Wire.beginTransmission(address);
        Wire.write(uint8_t(0x8A));
        Wire.write(uint8_t(0x29));
        Wire.endTransmission();
        delay(5);
        if (ack(0x29)) break;
      }
    }
    // Hold every XSHUT low first: an unused head must stay in shutdown instead
    // of floating high and answering at 0x29. Only fitted heads are released.
    for (uint8_t h = 0; h < 3; ++h) {
      pinMode(PIN_XSHUT[h], OUTPUT);
      digitalWrite(PIN_XSHUT[h], LOW);
    }
    delay(10);
    for (uint8_t k = 0; k < TOF_HEAD_COUNT; ++k) {
      const uint8_t h = TOF_HEADS[k];
      // Release XSHUT to the module's own pull-up; never drive it high.
      pinMode(PIN_XSHUT[h], INPUT);
      delay(10);
      failHead = h;
      if (!sensor[h].begin(Wire))                       { fail = "probe at 0x29"; return false; }
      if (!sensor[h].init())                            { fail = "init";          return false; }
      if (!sensor[h].setAddress(TOF_ADDRESS[h]))        { fail = "setAddress";    return false; }
      if (!sensor[h].setMeasurementTimingBudget(33000)) { fail = "timing budget"; return false; }
      if (!sensor[h].healthy())                         { fail = "I2C transport"; return false; }
    }
    if (TOF_HEAD_COUNT == 3) {
      static const uint8_t order[4] = {1, 0, 1, 2};
      for (uint8_t i = 0; i < 4; ++i) schedule[i] = order[i];
      scheduleLen = 4;
    } else {
      for (uint8_t i = 0; i < TOF_HEAD_COUNT; ++i) schedule[i] = TOF_HEADS[i];
      scheduleLen = TOF_HEAD_COUNT;
    }
    return true;
  }

  // Every fitted head has a sample younger than RANGE_STALE_MS. Absent heads
  // are not consulted, so a one-head build can still start a round.
  bool fresh(uint32_t now) const {
    for (uint8_t k = 0; k < TOF_HEAD_COUNT; ++k) {
      const Range &r = ranges[TOF_HEADS[k]];
      if (!r.sampledMs || now - r.sampledMs > RANGE_STALE_MS) return false;
    }
    return true;
  }

  // Non-blocking. Call from the ranging task. Returns false on a transport
  // fault; `ranges` always holds the latest confirmed snapshot.
  bool service(uint32_t now) {
    if (!busy) {
      if (scheduleLen == 0) return false;
      const uint8_t head = schedule[slot % scheduleLen];
      if (!sensor[head].startSingle()) return false;
      index = head;
      started = now;
      busy = true;
      return true;
    }
    if (now - started > 80) return false;   // measurement timeout
    bool ready = false;
    if (!sensor[index].dataReady(ready)) return false;
    if (!ready) return true;

    uint16_t mm = 0;
    uint8_t status = 0;
    if (!sensor[index].readResult(mm, status)) return false;
    const bool target = status == 11 && mm >= 35 && mm <= 900;
    hits[index] = target ? uint8_t(min(2, int(hits[index]) + 1)) : 0;
    // Two consecutive valid samples confirm a target; one sample is not enough.
    ranges[index] = Range{uint16_t(hits[index] == 2 ? mm : 0), now};
    slot = uint8_t((slot + 1) % scheduleLen);
    busy = false;
    return true;
  }
};

} // namespace BS
