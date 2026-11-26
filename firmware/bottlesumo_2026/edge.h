// edge.h — IR threshold math and edge debounce. No Arduino dependency, so the
// rules can be exercised on a host.
#pragma once
#include <stdint.h>
#include "config.h"

namespace bs {

// ---------------------------------------------------------------------------
// Calibration
//
// Two 3-second captures:
//   floor — robot on the real table, all four windows over the surface
//   void  — robot supported at the same height, all four over the drop
// Polarity is not assumed: some surfaces read higher off the edge, some lower.
// A channel may also be pinned to a hand-set threshold and polarity.
// ---------------------------------------------------------------------------
struct Calibration {
  uint16_t floorMin[4] = {}, floorMax[4] = {}, edgeMin[4] = {}, edgeMax[4] = {};
  uint16_t manualValue[4] = {};
  uint8_t manualMask = 0;
  uint8_t manualHigh = 0;

  void reset() {
    for (uint8_t i = 0; i < 4; ++i) {
      floorMin[i] = floorMax[i] = edgeMin[i] = edgeMax[i] = 0;
      manualValue[i] = 0;
    }
    manualMask = 0;
    manualHigh = 0;
  }

  bool manual(uint8_t i) const { return (manualMask >> i) & 1; }

  void setManual(uint8_t i, uint16_t value, bool high) {
    manualValue[i] = value;
    if (high) manualHigh |= uint8_t(1u << i);
    else      manualHigh &= uint8_t(~(1u << i));
    manualMask |= uint8_t(1u << i);
  }

  void clearManual(uint8_t i) { manualMask &= uint8_t(~(1u << i)); }

  void resetCapture(bool floor) {
    for (uint8_t i = 0; i < 4; ++i) {
      (floor ? floorMin : edgeMin)[i] = 4095;
      (floor ? floorMax : edgeMax)[i] = 0;
    }
  }

  // A fresh capture starts from zero so an old run cannot widen the envelope.
  void clearCapture() {
    for (uint8_t i = 0; i < 4; ++i) {
      floorMin[i] = edgeMin[i] = 4095;
      floorMax[i] = edgeMax[i] = 0;
    }
  }

  void sample(bool floor, const uint16_t ir[4]) {
    for (uint8_t i = 0; i < 4; ++i) {
      uint16_t &low = (floor ? floorMin : edgeMin)[i];
      uint16_t &high = (floor ? floorMax : edgeMax)[i];
      if (ir[i] < low) low = ir[i];
      if (ir[i] > high) high = ir[i];
    }
  }

  bool captured(bool floor) const {
    for (uint8_t i = 0; i < 4; ++i) {
      if ((floor ? floorMin : edgeMin)[i] > (floor ? floorMax : edgeMax)[i]) return false;
    }
    return true;
  }

  // Every channel needs a floor/void gap of CAL_MIN_GAP. A manual channel
  // skips the gap test, which is its purpose.
  bool valid() const {
    for (uint8_t i = 0; i < 4; ++i) {
      if (manual(i)) {
        if (manualValue[i] < 1 || manualValue[i] > 4094) return false;
        continue;
      }
      if (floorMin[i] > floorMax[i] || edgeMin[i] > edgeMax[i]) return false;
      const int gap = edgeMin[i] > floorMax[i] ? int(edgeMin[i]) - int(floorMax[i])
                                               : int(floorMin[i]) - int(edgeMax[i]);
      if (gap < int(CAL_MIN_GAP)) return false;
    }
    return true;
  }

  bool autoEdgeIsHigh(uint8_t i) const { return edgeMin[i] > floorMax[i]; }

  bool edgeIsHigh(uint8_t i) const {
    if (manual(i)) return ((manualHigh >> i) & 1) != 0;
    return autoEdgeIsHigh(i);
  }

  // The captured midpoint, independent of a manual override. Zero when the
  // channel was never captured; the web page shows it next to the manual value
  // so an override can be checked against the real clusters.
  uint16_t autoThreshold(uint8_t i) const {
    return autoEdgeIsHigh(i) ? uint16_t((int(edgeMin[i]) + int(floorMax[i])) / 2)
                             : uint16_t((int(floorMin[i]) + int(edgeMax[i])) / 2);
  }

  uint16_t threshold(uint8_t i) const {
    if (manual(i)) return manualValue[i];
    return autoThreshold(i);
  }

  // Hysteresis keyed on the previous confirmed bit, not the raw sample.
  bool edge(uint8_t i, uint16_t adc, bool previous) const {
    const int t = int(threshold(i));
    const int h = int(EDGE_HYSTERESIS);
    return edgeIsHigh(i) ? int(adc) > t + (previous ? -h : h)
                         : int(adc) < t + (previous ? h : -h);
  }

  uint8_t rawEdges(const uint16_t ir[4], uint8_t previous) const {
    uint8_t bits = 0;
    for (uint8_t i = 0; i < 4; ++i) {
      if (edge(i, ir[i], ((previous >> i) & 1) != 0)) bits |= uint8_t(1u << i);
    }
    return bits;
  }
};

// ---------------------------------------------------------------------------
// Debounce: a raw bit must hold for EDGE_CONFIRM_MS before it is reported.
// One noisy sample must not start an escape that loses the round.
// ---------------------------------------------------------------------------
struct EdgeFilter {
  uint8_t confirmed = 0;
  uint8_t pending = 0;
  uint32_t pendingSince[4] = {};

  uint8_t update(uint32_t now, uint8_t raw) {
    for (uint8_t i = 0; i < 4; ++i) {
      const bool bit = ((raw >> i) & 1) != 0;
      const bool current = ((confirmed >> i) & 1) != 0;
      if (bit == current) {
        pending &= uint8_t(~(1u << i));
        continue;
      }
      if (((pending >> i) & 1) == 0) {
        pending |= uint8_t(1u << i);
        pendingSince[i] = now;
      }
      if (now - pendingSince[i] >= EDGE_CONFIRM_MS) {
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

} // namespace bs
