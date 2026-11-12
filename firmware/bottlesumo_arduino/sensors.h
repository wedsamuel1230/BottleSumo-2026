// sensors.h — QRE sampling, self-calibration, persistence, and the VL53L0X
// ranging task logic. No game logic lives here.
#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "config.h"
#include "state_machine.h"
#include "vl53l0x.h"

namespace BS {

// ---------------------------------------------------------------------------
// QRE1113 edge sensors
// ---------------------------------------------------------------------------
inline void readQre(uint16_t out[4]) {
  for (uint8_t i = 0; i < 4; ++i) out[i] = uint16_t(analogRead(PIN_QRE[i]));
}

// ---------------------------------------------------------------------------
// Self-calibration
//
// Two 3-second captures:
//   floor — robot fully on the table, sampling the real surface and seam tape
//   void  — robot supported at normal sensor height with all four windows over
//           the drop
// The threshold is the midpoint between the two clusters; polarity is detected
// automatically (some surfaces read higher off the edge, some lower).
// ---------------------------------------------------------------------------
struct QreCalibration {
  uint16_t floorMin[4] = {}, floorMax[4] = {}, edgeMin[4] = {}, edgeMax[4] = {};

  void reset() {
    for (uint8_t i = 0; i < 4; ++i) {
      floorMin[i] = floorMax[i] = edgeMin[i] = edgeMax[i] = 0;
    }
  }

  void resetCapture(bool floor) {
    for (uint8_t i = 0; i < 4; ++i) {
      (floor ? floorMin : edgeMin)[i] = 4095;
      (floor ? floorMax : edgeMax)[i] = 0;
    }
  }

  void sample(bool floor, const uint16_t qre[4]) {
    for (uint8_t i = 0; i < 4; ++i) {
      uint16_t &low  = (floor ? floorMin : edgeMin)[i];
      uint16_t &high = (floor ? floorMax : edgeMax)[i];
      low  = min(low, qre[i]);
      high = max(high, qre[i]);
    }
  }

  // Same acceptance rule as the ESP-IDF firmware: every channel needs a
  // floor/void gap of at least 300 ADC counts.
  bool valid() const {
    for (uint8_t i = 0; i < 4; ++i) {
      if (floorMin[i] > floorMax[i] || edgeMin[i] > edgeMax[i] ||
          floorMax[i] > 4095 || edgeMax[i] > 4095) return false;
      const int gap = edgeMin[i] > floorMax[i] ? edgeMin[i] - floorMax[i]
                                               : floorMin[i] - edgeMax[i];
      if (gap < 300) return false;
    }
    return true;
  }

  bool edgeIsHigh(uint8_t i) const { return edgeMin[i] > floorMax[i]; }

  uint16_t threshold(uint8_t i) const {
    const bool high = edgeIsHigh(i);
    return high ? uint16_t((edgeMin[i] + floorMax[i]) / 2)
                : uint16_t((floorMin[i] + edgeMax[i]) / 2);
  }

  // 40-count hysteresis, confirmed by the previous sample.
  bool edge(uint8_t i, uint16_t adc, bool previous) const {
    const bool high = edgeIsHigh(i);
    const int threshold = high ? (edgeMin[i] + floorMax[i]) / 2
                               : (floorMin[i] + edgeMax[i]) / 2;
    return high ? adc > threshold + (previous ? -40 : 40)
                : adc < threshold + (previous ? 40 : -40);
  }

  // Separation quality: distance between clusters divided by the wider
  // cluster's spread. Midpoints are used because only min/max are stored.
  float separationRatio(uint8_t i) const {
    const uint16_t safeMid = uint16_t((floorMin[i] + floorMax[i]) / 2);
    const uint16_t edgeMid = uint16_t((edgeMin[i] + edgeMax[i]) / 2);
    const float separation = float(abs(int(edgeMid) - int(safeMid)));
    const uint16_t spread = max(floorMax[i] - floorMin[i], edgeMax[i] - edgeMin[i]);
    return separation / float(max(1, int(spread)));
  }

  // 3 = good (>=5), 2 = usable (3..5), 1 = risky (1.5..3), 0 = fail (<1.5)
  uint8_t quality(uint8_t i) const {
    const float ratio = separationRatio(i);
    if (ratio >= 5.0f) return 3;
    if (ratio >= 3.0f) return 2;
    if (ratio >= 1.5f) return 1;
    return 0;
  }
};

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
