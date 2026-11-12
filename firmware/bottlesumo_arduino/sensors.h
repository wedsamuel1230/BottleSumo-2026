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
// Only one head ranges at a time — no overlapping optical exposures. The
// schedule visits the center head twice per cycle so the front target is
// acquired earlier.
// ---------------------------------------------------------------------------
struct TofState {
  VL53L0X sensor[3];
  Range ranges[3];
  uint8_t index = 0;
  uint8_t hits[3] = {};
  uint8_t schedule[4] = {1, 0, 1, 2};
  uint8_t slot = 0;
  uint32_t started = 0;
  bool busy = false;

  bool begin() {
    for (uint8_t i = 0; i < 3; ++i) {
      pinMode(PIN_XSHUT[i], OUTPUT);
      digitalWrite(PIN_XSHUT[i], LOW);
    }
    delay(10);
    for (uint8_t i = 0; i < 3; ++i) {
      // Release XSHUT to the module's own pull-up; never drive it high.
      pinMode(PIN_XSHUT[i], INPUT);
      delay(10);
      if (!sensor[i].begin(Wire)) return false;
      if (!sensor[i].init()) return false;
      if (!sensor[i].setAddress(TOF_ADDRESS[i])) return false;
      if (!sensor[i].setMeasurementTimingBudget(33000)) return false;
      if (!sensor[i].healthy()) return false;
    }
    return true;
  }

  // Non-blocking. Call from the ranging task. Returns false on a transport
  // fault; `ranges` always holds the latest confirmed snapshot.
  bool service(uint32_t now) {
    if (!busy) {
      const uint8_t head = schedule[slot];
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
    slot = uint8_t((slot + 1) % 4);
    busy = false;
    return true;
  }
};

} // namespace BS
