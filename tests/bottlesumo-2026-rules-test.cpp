// Host assertions for the 2026 IR rules: calibration polarity, hysteresis,
// debounce, the 3+ stop rule and the operator's edge reaction table.
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
  check(edgeFails(0b0111), "three bits stop");
  check(edgeFails(0b1111), "four bits stop");
  check(!edgeFails(0b0011), "front pair does not stop");

  const Reaction clear = reactionFor(0b0000);
  check(clear.left == EDGE_FORWARD && clear.right == EDGE_FORWARD, "all clear forward 70");

  const Reaction front = reactionFor(0b0011); // FL + FR
  check(front.left == -EDGE_BACKWARD && front.right == -EDGE_BACKWARD, "front pair backs off");

  const Reaction rear = reactionFor(0b1100); // RL + RR
  check(rear.left == EDGE_FORWARD && rear.right == EDGE_FORWARD, "rear pair drives on");

  const Reaction fl = reactionFor(0b0001);
  check(fl.left == EDGE_TURN_30 && fl.right == -EDGE_TURN_30, "FL turns right 30");

  const Reaction fr = reactionFor(0b0010);
  check(fr.left == -EDGE_TURN_30 && fr.right == EDGE_TURN_30, "FR turns left 30");

  const Reaction rl = reactionFor(0b0100);
  check(rl.left == EDGE_TURN_30 && rl.right == -EDGE_TURN_30, "RL (A0) turns right 30");

  const Reaction rr = reactionFor(0b1000);
  check(rr.left == -EDGE_TURN_30 && rr.right == EDGE_TURN_30, "RR (A3) turns left 30");

  const Reaction left = reactionFor(0b0101); // FL + RL
  check(left.left == EDGE_TURN_50 && left.right == -EDGE_TURN_50, "left side turns right 50");

  const Reaction right = reactionFor(0b1010); // FR + RR
  check(right.left == -EDGE_TURN_50 && right.right == EDGE_TURN_50, "right side turns left 50");

  const Reaction diagA = reactionFor(0b0110); // RL + FR = A0 + A2
  check(diagA.left == EDGE_SLIGHT_FAST && diagA.right == EDGE_SLIGHT_SLOW,
        "A0+A2 slight right 40");

  const Reaction diagB = reactionFor(0b1001); // FL + RR = A1 + A3
  check(diagB.left == EDGE_SLIGHT_SLOW && diagB.right == EDGE_SLIGHT_FAST,
        "A1+A3 slight left 40");

  const Reaction stop = reactionFor(0b0111);
  check(stop.left == 0 && stop.right == 0, "3+ reaction is stop");
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

  Calibration captured;
  captured.reset();
  captured.resetCapture(true);
  captured.resetCapture(false);
  for (uint16_t v = 800; v <= 900; v += 20) capture(captured, true, v);
  for (uint16_t v = 3000; v <= 3200; v += 40) capture(captured, false, v);
  captured.setManual(0, 80, true);
  check(captured.threshold(0) == 80, "manual value hides the auto threshold");
  check(captured.autoThreshold(0) > 900 && captured.autoThreshold(0) < 3000,
        "auto threshold stays visible under manual override");
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
