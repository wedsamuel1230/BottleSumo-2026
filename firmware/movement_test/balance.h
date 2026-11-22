// balance.h — FG speed maths, duty trim and the two-motor balance step.
//
// Pure integer logic with no Arduino dependency, so the host test
// (tests/movement-balance-test.cpp) exercises the same code the sketch runs.
#pragma once
#include <stdint.h>

namespace bench {

struct BalanceConfig {
  int16_t limit = 40;        // max |trim| per wheel, duty counts
  uint16_t deadbandRpm = 2;  // no correction inside this rpm gap
  int16_t step = 1;          // duty counts per update
};

// FG edge period to speed. The listing gives 9 pulses per motor revolution.
constexpr uint32_t FG_MIN_PERIOD_US = 2000;     // 500 Hz ceiling
constexpr uint32_t FG_MAX_PERIOD_US = 1000000;  // 1 Hz floor

// A period outside this range is not a motor pulse. 20 kHz (50 us) is the PWM
// carrier itself, which an unconnected or floating FG pin picks up.
bool fgPeriodValid(uint32_t periodUs);

// Three samples must agree before a speed is believed: a floating FG input
// produces plausible-looking periods now and then. update() returns the median
// accepted period, or 0 while the history is short or unsteady.
struct PeriodGate {
  uint32_t history[3] = {0, 0, 0};
  uint8_t count = 0;
  uint32_t update(uint32_t periodUs);
};

uint16_t hzFromPeriodUs(uint32_t periodUs);
uint16_t rpmFromPeriodUs(uint32_t periodUs, uint8_t pulsesPerRev);

// Commanded magnitude plus a balance offset, clamped to 0..255 so a negative
// trim cannot wrap a uint8_t command to full speed.
int trimmedDuty(int magnitude, int16_t trim);

// Fixed per-wheel duty offsets, interpolated between four duty points. This is
// the fallback when no usable tacho exists: tune it once at the bench and it
// keeps the two wheels even without feedback.
struct TrimTable {
  static constexpr uint8_t N = 4;
  uint8_t duty[N] = {64, 128, 192, 255};
  int8_t trim[2][N] = {};
  int8_t at(uint8_t wheel, int commandDuty) const;
  uint8_t pointFor(int commandDuty) const;
};

// One controller update, called every FG_SAMPLE_MS while both wheels turn.
// It moves the trims in opposite directions by `step` until the two wheel
// speeds are inside `deadbandRpm`. Integral only: the plant is slow and the
// rpm reading is quantised, so a proportional term would mostly add noise.
// Returns true when a trim changed.
bool balanceStep(int16_t trim[2], uint16_t rpmLeft, uint16_t rpmRight,
                 const BalanceConfig &cfg);

}  // namespace bench
