#include "balance.h"

namespace bench {

bool fgPeriodValid(uint32_t periodUs) {
  return periodUs >= FG_MIN_PERIOD_US && periodUs <= FG_MAX_PERIOD_US;
}

uint32_t PeriodGate::update(uint32_t periodUs) {
  if (!fgPeriodValid(periodUs)) {
    count = 0;
    return 0;
  }
  history[0] = history[1];
  history[1] = history[2];
  history[2] = periodUs;
  if (count < 3) ++count;
  if (count < 3) return 0;

  // Median of three; the spread test keeps a burst of noise from passing.
  uint32_t a = history[0], b = history[1], c = history[2];
  if (a > b) { const uint32_t t = a; a = b; b = t; }
  if (b > c) { const uint32_t t = b; b = c; c = t; }
  if (a > b) { const uint32_t t = a; a = b; b = t; }
  const uint32_t median = b;
  if (c - a > median / 4) return 0;   // >25 % spread is not a steady tacho
  return median;
}

uint16_t hzFromPeriodUs(uint32_t periodUs) {
  if (periodUs == 0) return 0;
  return uint16_t(1000000UL / periodUs);
}

uint16_t rpmFromPeriodUs(uint32_t periodUs, uint8_t pulsesPerRev) {
  if (periodUs == 0 || pulsesPerRev == 0) return 0;
  const uint64_t perRevUs = uint64_t(periodUs) * pulsesPerRev;
  const uint64_t rpm = 60000000ULL / perRevUs;
  return rpm > 65535 ? 65535 : uint16_t(rpm);
}

int trimmedDuty(int magnitude, int16_t trim) {
  const int biased = magnitude + int(trim);
  if (biased < 0) return 0;
  if (biased > 255) return 255;
  return biased;
}

int8_t TrimTable::at(uint8_t wheel, int commandDuty) const {
  if (commandDuty <= duty[0]) return trim[wheel][0];
  for (uint8_t i = 1; i < N; ++i) {
    if (commandDuty <= duty[i]) {
      const int span = int(duty[i]) - int(duty[i - 1]);
      const int offset = commandDuty - int(duty[i - 1]);
      const int low = trim[wheel][i - 1];
      const int high = trim[wheel][i];
      return int8_t(low + (high - low) * offset / span);
    }
  }
  return trim[wheel][N - 1];
}

uint8_t TrimTable::pointFor(int commandDuty) const {
  uint8_t best = 0;
  int bestDistance = 1000;
  for (uint8_t i = 0; i < N; ++i) {
    const int d = commandDuty > int(duty[i]) ? commandDuty - int(duty[i])
                                             : int(duty[i]) - commandDuty;
    if (d < bestDistance) {
      bestDistance = d;
      best = i;
    }
  }
  return best;
}

bool balanceStep(int16_t trim[2], uint16_t rpmLeft, uint16_t rpmRight,
                 const BalanceConfig &cfg) {
  // One wheel stopped is not a speed difference; it is a stall or a spin-up.
  if (rpmLeft == 0 || rpmRight == 0) return false;

  const int diff = int(rpmLeft) - int(rpmRight);
  if (diff > int(cfg.deadbandRpm)) {
    bool changed = false;
    if (trim[0] > -cfg.limit) { trim[0] -= cfg.step; changed = true; }
    if (trim[1] < cfg.limit) { trim[1] += cfg.step; changed = true; }
    return changed;
  }
  if (diff < -int(cfg.deadbandRpm)) {
    bool changed = false;
    if (trim[0] < cfg.limit) { trim[0] += cfg.step; changed = true; }
    if (trim[1] > -cfg.limit) { trim[1] -= cfg.step; changed = true; }
    return changed;
  }
  return false;
}

}  // namespace bench
