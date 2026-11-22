// qre.h — QRE1113 threshold logic and the edge debounce, with no hardware or
// Arduino dependency at all, so both can be exercised on a host in
// tests/arduino-state-machine-test.cpp.
#pragma once
#include <stdint.h>
#include "config.h"

namespace BS {

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

  // Manual override, one channel at a time. This is the escape hatch for a
  // surface where the automatic midpoint is wrong: bit i of manualMask switches
  // channel i from the captured midpoint to manualThreshold[i], with polarity
  // from bit i of manualHigh. Never silently ignored — the UI shows which
  // channels are hand-set.
  uint16_t manualThreshold[4] = {};
  uint8_t  manualMask = 0;
  uint8_t  manualHigh = 0;

  void reset() {
    for (uint8_t i = 0; i < 4; ++i) {
      floorMin[i] = floorMax[i] = edgeMin[i] = edgeMax[i] = 0;
      manualThreshold[i] = 0;
    }
    manualMask = 0;
    manualHigh = 0;
  }

  bool manual(uint8_t i) const { return (manualMask >> i) & 1; }

  void setManual(uint8_t i, uint16_t threshold, bool high) {
    manualThreshold[i] = threshold;
    if (high) manualHigh |= uint8_t(1 << i);
    else      manualHigh &= uint8_t(~(1 << i));
    manualMask |= uint8_t(1 << i);
  }

  void clearManual(uint8_t i) { manualMask &= uint8_t(~(1 << i)); }

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
      if (qre[i] < low) low = qre[i];
      if (qre[i] > high) high = qre[i];
    }
  }

  // Same acceptance rule as the ESP-IDF firmware: every channel needs a
  // floor/void gap of at least 300 ADC counts. A hand-set channel skips that
  // test, which is the whole point of the override.
  bool valid() const {
    for (uint8_t i = 0; i < 4; ++i) {
      if (manual(i)) {
        if (manualThreshold[i] < 1 || manualThreshold[i] > 4094) return false;
        continue;
      }
      if (floorMin[i] > floorMax[i] || edgeMin[i] > edgeMax[i] ||
          floorMax[i] > 4095 || edgeMax[i] > 4095) return false;
      const int gap = edgeMin[i] > floorMax[i] ? int(edgeMin[i]) - int(floorMax[i])
                                               : int(floorMin[i]) - int(edgeMax[i]);
      if (gap < 300) return false;
    }
    return true;
  }

  bool edgeIsHigh(uint8_t i) const {
    if (manual(i)) return ((manualHigh >> i) & 1) != 0;
    return edgeMin[i] > floorMax[i];
  }

  uint16_t threshold(uint8_t i) const {
    if (manual(i)) return manualThreshold[i];
    const bool high = edgeIsHigh(i);
    return high ? uint16_t((int(edgeMin[i]) + int(floorMax[i])) / 2)
                : uint16_t((int(floorMin[i]) + int(edgeMax[i])) / 2);
  }

  // Hysteresis around the active threshold, confirmed by the previous sample.
  bool edge(uint8_t i, uint16_t adc, bool previous) const {
    const bool high = edgeIsHigh(i);
    const int t = int(threshold(i));
    return high ? int(adc) > t + (previous ? -EDGE_HYSTERESIS : EDGE_HYSTERESIS)
                : int(adc) < t + (previous ? EDGE_HYSTERESIS : -EDGE_HYSTERESIS);
  }

  // Separation quality: distance between clusters divided by the wider
  // cluster's spread. Midpoints are used because only min/max are stored.
  float separationRatio(uint8_t i) const {
    const int safeMid = (int(floorMin[i]) + int(floorMax[i])) / 2;
    const int edgeMid = (int(edgeMin[i]) + int(edgeMax[i])) / 2;
    const int diff = edgeMid > safeMid ? edgeMid - safeMid : safeMid - edgeMid;
    int spread = int(floorMax[i]) - int(floorMin[i]);
    const int edgeSpread = int(edgeMax[i]) - int(edgeMin[i]);
    if (edgeSpread > spread) spread = edgeSpread;
    if (spread < 1) spread = 1;
    return float(diff) / float(spread);
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

// ---------------------------------------------------------------------------
// Edge debounce
//
// The raw comparison runs every 5 ms. A single noisy sample must not send the
// robot into Escape — that is exactly how last year's round was lost: one false
// edge, then a reverse that never stopped. A channel is only reported as an
// edge after the raw bit has held for confirmMs. confirmMs = 0 disables it.
// ---------------------------------------------------------------------------
struct EdgeFilter {
  uint8_t  confirmMs = 25;
  uint8_t  confirmed = 0;
  uint8_t  pending = 0;
  uint32_t pendingSince[4] = {};

  uint8_t update(uint32_t now, uint8_t raw) {
    for (uint8_t i = 0; i < 4; ++i) {
      const bool bit = ((raw >> i) & 1) != 0;
      const bool current = ((confirmed >> i) & 1) != 0;
      if (bit == current) { pending &= uint8_t(~(1u << i)); continue; }
      if (((pending >> i) & 1) == 0) { pending |= uint8_t(1u << i); pendingSince[i] = now; }
      if (confirmMs == 0 || now - pendingSince[i] >= confirmMs) {
        confirmed ^= uint8_t(1u << i);
        pending &= uint8_t(~(1u << i));
      }
    }
    return confirmed;
  }

  void reset() {
    confirmed = pending = 0;
    for (uint8_t i = 0; i < 4; ++i) pendingSince[i] = 0;
  }
};

} // namespace BS
