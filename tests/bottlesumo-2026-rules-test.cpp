// Host assertions for the 2026 IR rules: calibration polarity, hysteresis,
// debounce, the 3+ channel failure rule and the 2025 escape table.
//
// Build:
//   c++ -std=c++17 -Wall -Wextra -Werror -Ifirmware/bottlesumo_2026 \
//       tests/bottlesumo-2026-rules-test.cpp -o /tmp/bs2026-rules
//   /tmp/bs2026-rules
#include <stdio.h>

#include "config.h"
#include "edge.h"
#include "rules.h"

using namespace bs;

static int failures = 0;

static void check(bool ok, const char *what) {
  if (!ok) {
    printf("FAIL %s\n", what);
    ++failures;
  }
}

static void capture(Calibration &cal, bool floor, uint16_t value) {
  for (uint8_t i = 0; i < 4; ++i) cal.sample(floor, (uint16_t[4]){value, value, value, value});
}

static void testEscapeTable() {
  check(edgeFails(0b0111), "three bits fail");
  check(edgeFails(0b1111), "four bits fail");
  check(!edgeFails(0b0011), "two bits do not fail");

  const Reaction front = reactionFor(0b0011); // FL + FR
  check(front.left == -ESCAPE_SPEED && front.right == -ESCAPE_SPEED, "front pair backs off");

  const Reaction rear = reactionFor(0b1100); // RL + RR
  check(rear.left == ESCAPE_SPEED && rear.right == ESCAPE_SPEED, "rear pair drives on");

  const Reaction fl = reactionFor(0b0001);
  check(fl.left == ESCAPE_SPEED && fl.right == -ESCAPE_SPEED, "front-left turns right");

  const Reaction fr = reactionFor(0b0010);
  check(fr.left == -ESCAPE_SPEED && fr.right == ESCAPE_SPEED, "front-right turns left");

  const Reaction left = reactionFor(0b0101); // FL + RL
  check(left.left == ESCAPE_SPEED && left.right == -ESCAPE_SPEED, "left side turns right");

  const Reaction right = reactionFor(0b1010); // FR + RR
  check(right.left == -ESCAPE_SPEED && right.right == ESCAPE_SPEED, "right side turns left");

  const Reaction rl = reactionFor(0b0100);
  const Reaction rr = reactionFor(0b1000);
  check(rl.left == ESCAPE_SPEED && rl.right == ESCAPE_SPEED, "rear-left drives on");
  check(rr.left == ESCAPE_SPEED && rr.right == ESCAPE_SPEED, "rear-right drives on");
}

static void testCalibrationHighPolarity() {
  Calibration cal;
  cal.reset();
  cal.resetCapture(true);
  cal.resetCapture(false);
  for (uint16_t v = 800; v <= 900; v += 20) capture(cal, true, v);
  for (uint16_t v = 3000; v <= 3200; v += 40) capture(cal, false, v);

  check(cal.valid(), "high polarity gap accepted");
  check(cal.edgeIsHigh(0), "edge reads high");
  const int t = cal.threshold(0);
  check(t > 900 && t < 3000, "threshold between clusters");
  check(cal.edge(0, 3500, false), "void sample is an edge");
  check(!cal.edge(0, 1500, false), "mid sample is not an edge");
  check(!cal.edge(0, uint16_t(t + 10), false), "hysteresis blocks a small rise");
  check(cal.edge(0, uint16_t(t + 10), true), "held edge keeps hysteresis");
}

static void testCalibrationLowPolarity() {
  Calibration cal;
  cal.reset();
  cal.resetCapture(true);
  cal.resetCapture(false);
  for (uint16_t v = 3000; v <= 3100; v += 20) capture(cal, true, v);
  for (uint16_t v = 900; v <= 1000; v += 20) capture(cal, false, v);

  check(cal.valid(), "low polarity gap accepted");
  check(!cal.edgeIsHigh(0), "edge reads low");
  check(cal.edge(0, 500, false), "low void sample is an edge");
  check(!cal.edge(0, 2500, false), "table sample is not an edge");
}

static void testCalibrationRejectsTinyGap() {
  Calibration cal;
  cal.reset();
  cal.resetCapture(true);
  cal.resetCapture(false);
  for (uint8_t i = 0; i < 4; ++i) {
    cal.floorMin[i] = 1000;
    cal.floorMax[i] = 1050;
    cal.edgeMin[i] = 1100;
    cal.edgeMax[i] = 1150;
  }
  check(!cal.valid(), "gap below CAL_MIN_GAP rejected");
  cal.setManual(0, 1200, true);
  check(!cal.valid(), "one manual channel is not enough for four bad ones");
}

static void testManualOverride() {
  Calibration cal;
  cal.reset();
  cal.setManual(2, 1500, false);
  check(cal.manual(2), "manual flag set");
  check(cal.threshold(2) == 1500, "manual threshold used");
  check(!cal.edgeIsHigh(2), "manual polarity low");
  cal.clearManual(2);
  check(!cal.manual(2), "manual flag cleared");
}

static void testDebounce() {
  EdgeFilter filter;
  check(filter.update(0, 0b0001) == 0, "first raw sample is pending");
  check(filter.update(10, 0b0001) == 0, "pending before confirm window");
  check(filter.update(15, 0b0001) == 0b0001, "confirmed after 15 ms");
  check(filter.update(16, 0b0000) == 0b0001, "release is also debounced");
  check(filter.update(31, 0b0000) == 0, "release confirmed after 15 ms");
}

int main() {
  testEscapeTable();
  testCalibrationHighPolarity();
  testCalibrationLowPolarity();
  testCalibrationRejectsTinyGap();
  testManualOverride();
  testDebounce();
  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("bottlesumo 2026 rules: all checks passed\n");
  return 0;
}
