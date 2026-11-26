// Host assertions for the movement-test FG maths and balance controller.
// Pure logic, no hardware.
// Build:
//   c++ -std=c++17 -Wall -Wextra -Werror -Ifirmware/movement_test \
//       tests/movement-balance-test.cpp firmware/movement_test/balance.cpp \
//       -o /tmp/bs-balance-test
//   /tmp/bs-balance-test
#include <assert.h>
#include <stdio.h>

#include "balance.h"

using namespace bench;

static int failures = 0;

static void check(bool ok, const char *what) {
  if (!ok) {
    printf("FAIL %s\n", what);
    ++failures;
  }
}

// One tick of a two-motor plant: rpm follows the duty through a per-motor gain
// (milli-rpm per duty count), then the controller sees the quantised rpm.
static uint16_t plantRpm(int duty, int gainMilli) {
  const int rpm = duty * gainMilli / 1000;
  return rpm > 0 ? uint16_t(rpm) : 0;
}

static void testSpeedMath() {
  // 9 pulses per revolution: 13.5 Hz is 90 rpm.
  check(hzFromPeriodUs(74074) == 13, "hz from period");
  check(rpmFromPeriodUs(74074, 9) == 90, "rpm from period");
  check(rpmFromPeriodUs(111111, 9) == 60, "9 pulses in 1 s = 60 rpm");
  check(rpmFromPeriodUs(0, 9) == 0, "zero period");
  check(rpmFromPeriodUs(74074, 0) == 0, "zero ppr");
  check(hzFromPeriodUs(0) == 0, "zero hz");
  // The bench read 20 kHz on GPIO9/12: the PWM carrier, not a motor pulse.
  check(!fgPeriodValid(50), "20 kHz carrier rejected");
  check(fgPeriodValid(74074), "13.5 Hz accepted");
  check(!fgPeriodValid(0), "zero period rejected");
  check(fgPeriodValid(1000000), "1 Hz floor accepted");
  check(!fgPeriodValid(1000001), "below 1 Hz rejected");
}

static void testPeriodGate() {
  PeriodGate gate;
  check(gate.update(71000) == 0, "warm-up 1");
  check(gate.update(71000) == 0, "warm-up 2");
  check(gate.update(71000) == 71000, "steady sample accepted");
  check(gate.update(50000) == 0, "30 % outlier rejected");
  check(gate.update(71000) == 0, "recovering 1");
  check(gate.update(71000) == 0, "recovering 2");
  check(gate.update(71000) == 71000, "accepted again");
  check(gate.update(50) == 0, "carrier period rejected");
  check(gate.update(71000) == 0, "reset by the carrier");
  check(gate.update(71000) == 0, "second sample after reset");
  check(gate.update(71000) == 71000, "accepted after reset");
}

static void testTrimTable() {
  TrimTable t;
  check(t.at(0, 64) == 0, "default point");
  t.trim[0][0] = -6;
  t.trim[0][1] = 6;
  check(t.at(0, 64) == -6, "first point");
  check(t.at(0, 128) == 6, "second point");
  check(t.at(0, 96) == 0, "interpolates between points");
  check(t.at(0, 40) == -6, "below the first point clamps");
  check(t.at(0, 255) == t.trim[0][3], "above the last uses the last");
  check(t.at(1, 64) == 0, "wheels are independent");
  check(t.pointFor(120) == 1, "120 edits 128");
  check(t.pointFor(40) == 0, "40 edits 64");
  check(t.pointFor(250) == 3, "250 edits 255");
}

static void testTrimClamp() {
  check(trimmedDuty(120, -6) == 114, "trim subtracts");
  check(trimmedDuty(3, -40) == 0, "no wrap to full speed");
  check(trimmedDuty(250, 40) == 255, "ceiling");
  check(trimmedDuty(0, 0) == 0, "zero duty");
}

static void testIdleCases() {
  int16_t trim[2] = {0, 0};
  const BalanceConfig cfg;
  check(!balanceStep(trim, 90, 90, cfg), "equal rpm is a no-op");
  check(!balanceStep(trim, 0, 90, cfg), "left stopped");
  check(!balanceStep(trim, 90, 0, cfg), "right stopped");
  check(trim[0] == 0 && trim[1] == 0, "trims untouched");

  check(!balanceStep(trim, 91, 90, cfg), "1 rpm is inside the deadband");
  check(balanceStep(trim, 93, 90, cfg), "3 rpm is outside the deadband");
  check(trim[0] == -1 && trim[1] == 1, "slow wheel gets the offset");
}

static void testConverges() {
  // Mirrored gearboxes with a 60 milli-gain spread, commanded at duty 120.
  const int gainMilli[2] = {720, 780};
  int16_t trim[2] = {0, 0};
  const BalanceConfig cfg;
  uint16_t rpm[2] = {0, 0};
  int16_t lastTrim[2] = {0, 0};

  for (int tick = 0; tick < 300; ++tick) {
    rpm[0] = plantRpm(trimmedDuty(120, trim[0]), gainMilli[0]);
    rpm[1] = plantRpm(trimmedDuty(120, trim[1]), gainMilli[1]);
    lastTrim[0] = trim[0];
    lastTrim[1] = trim[1];
    balanceStep(trim, rpm[0], rpm[1], cfg);
  }

  const int gap = rpm[0] > rpm[1] ? rpm[0] - rpm[1] : rpm[1] - rpm[0];
  printf("converged: rpm L=%u R=%u gap=%d trim L=%d R=%d\n", rpm[0], rpm[1], gap,
         trim[0], trim[1]);
  check(gap <= int(cfg.deadbandRpm), "gap settles inside the deadband");
  check(trim[0] >= -cfg.limit && trim[0] <= cfg.limit, "left trim bounded");
  check(trim[1] >= -cfg.limit && trim[1] <= cfg.limit, "right trim bounded");
  check(lastTrim[0] != 0 || lastTrim[1] != 0, "controller acted at all");
}

static void testLargeMismatch() {
  // A 400 milli-gain spread needs a large offset but is still reachable; the
  // loop must settle inside the deadband without winding past the limit.
  const int gainMilli[2] = {500, 900};
  int16_t trim[2] = {0, 0};
  const BalanceConfig cfg;
  uint16_t rpm[2] = {0, 0};
  for (int tick = 0; tick < 1000; ++tick) {
    rpm[0] = plantRpm(trimmedDuty(120, trim[0]), gainMilli[0]);
    rpm[1] = plantRpm(trimmedDuty(120, trim[1]), gainMilli[1]);
    balanceStep(trim, rpm[0], rpm[1], cfg);
  }
  const int gap = rpm[0] > rpm[1] ? rpm[0] - rpm[1] : rpm[1] - rpm[0];
  printf("large mismatch: rpm L=%u R=%u gap=%d trim L=%d R=%d\n", rpm[0], rpm[1],
         gap, trim[0], trim[1]);
  check(trim[0] >= -cfg.limit && trim[0] <= cfg.limit, "left stays bounded");
  check(trim[1] >= -cfg.limit && trim[1] <= cfg.limit, "right stays bounded");
  check(gap <= int(cfg.deadbandRpm), "large mismatch still settles");
}

static void testSaturatesAtLimit() {
  // An unreachable target (one wheel cannot be slowed enough) must pin the
  // trims at the limit, not wind up further.
  const int gainMilli[2] = {100, 900};
  int16_t trim[2] = {0, 0};
  const BalanceConfig cfg;
  for (int tick = 0; tick < 1000; ++tick) {
    const uint16_t rpm[2] = {plantRpm(trimmedDuty(120, trim[0]), gainMilli[0]),
                             plantRpm(trimmedDuty(120, trim[1]), gainMilli[1])};
    balanceStep(trim, rpm[0], rpm[1], cfg);
  }
  printf("unreachable: trim L=%d R=%d\n", trim[0], trim[1]);
  check(trim[0] == cfg.limit, "slow wheel pinned at +limit");
  check(trim[1] == -cfg.limit, "fast wheel pinned at -limit");
}

int main() {
  testSpeedMath();
  testPeriodGate();
  testTrimTable();
  testTrimClamp();
  testIdleCases();
  testConverges();
  testLargeMismatch();
  testSaturatesAtLimit();
  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("all balance checks passed\n");
  return 0;
}
