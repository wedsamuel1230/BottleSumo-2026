// Host assertions for the Arduino state machine. Pure logic, no hardware.
// Build:
//   c++ -std=c++17 -Wall -Wextra -Werror -Ifirmware/bottlesumo_arduino <this file> -o /tmp/bs-sm-test
//   /tmp/bs-sm-test
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "state_machine.h"
#include "qre.h"
using namespace BS;

static const uint16_t QRE[4] = {700, 700, 700, 700};

static void fresh(Range (&r)[3], uint32_t now, uint16_t left = 0,
                  uint16_t center = 0, uint16_t right = 0) {
  r[0] = {left, now}; r[1] = {center, now}; r[2] = {right, now};
}
static void stopped(Drive d) { assert(d.left == 0 && d.right == 0); }
static void at(Machine &m, uint32_t now, uint8_t edges = 0) { m.beginFrame(now, edges, QRE); }
static void beginRound(Machine &m, uint32_t now) { at(m, now); m.startRound(); }

// The classic search/approach tests predate the opening script and the carry
// arc; they disable both so a failure points at the code they are testing.
static Machine plain() {
  Machine m;
  m.tuning.openingEnabled = false;
  m.tuning.carryEnabled = false;
  return m;
}

int main() {
  // ---- Tuning: the runtime "voltage" knob ----
  Tuning tuning;
  assert(tuning.apply(100) == 100);
  tuning.powerScale = 50;
  assert(tuning.apply(100) == 50);
  assert(tuning.apply(255) == 127);
  assert(tuning.apply(-105) == -52);   // sign is preserved for steering
  tuning.powerScale = 100;
  tuning.maxPwm = 200;
  assert(tuning.apply(255) == 200);
  tuning.maxPwm = 255;

  Range r[3];
  Drive d;

  // ---- Start delay ----
  Machine m = plain();
  at(m, 1000); stopped(m.tick(r, false));
  assert(m.state == State::Stopped);
  m.startRound();
  assert(m.state == State::Starting);
  fresh(r, 1999, 0, 100, 0); at(m, 1999); stopped(m.tick(r, false));
  assert(m.state == State::Starting);
  fresh(r, 2000, 0, 500, 0); at(m, 2000);
  d = m.tick(r, false);
  assert(d.left > 0 && d.right > 0 && m.state == State::Approach);

  // ---- Approach -> Push -> Hold ----
  fresh(r, 2100, 0, 100, 0); at(m, 2100); d = m.tick(r, false);
  assert(d.left > 0 && d.right > 0 && m.state == State::Push);

  // One missed slot is held for CENTRE_HOLD_MS, not read as a lost bottle.
  fresh(r, 2200); at(m, 2200); d = m.tick(r, false);
  assert(m.state == State::Push && d.left > 0);

  fresh(r, 2400); at(m, 2400); stopped(m.tick(r, false));
  assert(m.state == State::Hold);
  fresh(r, 5899, 0, 100, 0); at(m, 5899); stopped(m.tick(r, false));
  assert(m.state == State::Hold);
  fresh(r, 5900, 0, 100, 0); at(m, 5900); d = m.tick(r, false);
  assert(d.left > 0 && d.right > 0);
  at(m, 6300); stopped(m.tick(r, false));   // the ranges are now stale
  assert(m.state == State::Stopped);

  // ---- Opening run: straight, pivot, straight, then hand over ----
  {
    Machine e;
    assert(e.tuning.openingEnabled);
    beginRound(e, 0);
    fresh(r, 1000, 0, 0, 0); at(e, 1000);
    d = e.tick(r, false);
    assert(e.state == State::Opening);
    assert(d.left > 0 && d.right > 0 && d.left == d.right);   // straight

    const uint32_t leg1 = e.tuning.openingForwardMs;
    fresh(r, 1000 + leg1 + 5, 0, 0, 0); at(e, 1000 + leg1 + 5);
    d = e.tick(r, false);
    assert(d.left > 0 && d.right < 0);                        // pivot right

    const uint32_t pivotEnd = leg1 + e.tuning.openingTurnMs;
    fresh(r, 1000 + pivotEnd + 5, 0, 0, 0); at(e, 1000 + pivotEnd + 5);
    d = e.tick(r, false);
    assert(d.left > 0 && d.right > 0 && d.left == d.right);    // leg 2 straight

    const uint32_t scriptEnd = pivotEnd + e.tuning.openingForward2Ms;
    fresh(r, 1000 + scriptEnd + 5, 0, 0, 0); at(e, 1000 + scriptEnd + 5);
    d = e.tick(r, false);
    assert(e.state == State::Search);
    assert(strcmp(reasonName(e.history.newest(0)->reason), "opening done") == 0);
  }

  // ---- Opening aborts the moment the centre head sees the bottle ----
  {
    Machine e;
    beginRound(e, 0);
    fresh(r, 1000, 0, 250, 0); at(e, 1000);
    d = e.tick(r, false);
    assert(e.state == State::Approach);
    assert(d.left > 0 && d.right > 0);
    assert(strcmp(reasonName(e.history.newest(0)->reason),
                  "opening abort: bottle in range") == 0);
  }

  // ---- Opening is still interruptible by a real edge ----
  {
    Machine e;
    beginRound(e, 0);
    fresh(r, 1100); at(e, 1100, 1);
    d = e.tick(r, false);
    assert(e.state == State::Escape);
    assert(d.left < 0 && d.right < 0);
  }

  // ---- Carry: contact turns into an arc, then hands back to Push ----
  {
    Machine e;
    e.tuning.openingEnabled = false;
    beginRound(e, 0);
    fresh(r, 1000, 0, 100, 0); at(e, 1000); e.tick(r, false);
    assert(e.state == State::Push);

    const uint32_t enter = 1000 + e.tuning.carryAfterMs;
    fresh(r, enter, 0, 120, 0); at(e, enter);
    d = e.tick(r, false);
    assert(e.state == State::Carry);
    assert(d.left != d.right);                    // it is arcing, not driving straight
    assert(d.left > 0 && d.right > 0);            // both wheels keep pushing

    // The arc side flips after carrySweepMs.
    const int firstLeft = d.left;
    const uint32_t flip = enter + e.tuning.carrySweepMs;
    fresh(r, flip, 0, 120, 0); at(e, flip);
    d = e.tick(r, false);
    assert(e.state == State::Carry);
    assert(d.left != firstLeft);

    // A short dropout is held; a real loss drops back to the search pattern.
    fresh(r, flip + 10, 0, 0, 0); at(e, flip + 10);
    e.tick(r, false);
    assert(e.state == State::Carry);
    fresh(r, flip + CENTRE_HOLD_MS + 10, 0, 0, 0);
    at(e, flip + CENTRE_HOLD_MS + 10);
    d = e.tick(r, false);
    assert(e.state == State::Search);
    assert(strcmp(reasonName(e.history.newest(0)->reason), "bottle lost") == 0);
  }

  // ---- Carry is bounded, and the 6 s push budget survives carry cycles ----
  {
    Machine e;
    e.tuning.openingEnabled = false;
    beginRound(e, 0);
    fresh(r, 1000, 0, 100, 0); at(e, 1000); e.tick(r, false);
    const uint32_t enter = 1000 + e.tuning.carryAfterMs;
    fresh(r, enter, 0, 120, 0); at(e, enter); e.tick(r, false);
    assert(e.state == State::Carry);
    const uint32_t leave = enter + e.tuning.carryMaxMs;
    fresh(r, leave, 0, 120, 0); at(e, leave); e.tick(r, false);
    assert(e.state == State::Push);
    // contactStart was set at the first contact, so the push budget still ends.
    fresh(r, 7001, 0, 120, 0); at(e, 7001); d = e.tick(r, false);
    assert(e.state == State::Unstick);
    assert(d.left < 0 && d.right < 0);
  }

  // ---- Every corner mask preempts an imminent push ----
  for (uint8_t mask = 1; mask < 16; ++mask) {
    Machine e = plain();
    beginRound(e, 0);
    fresh(r, 1000, 0, 100, 0);
    at(e, 1000); e.tick(r, false);          // leaves the start delay into Push
    at(e, 1005, mask); d = e.tick(r, false);
    const bool front = mask & 3;
    // Front and rear together is a corner straddle during the opening pivot,
    // not a reason to end the round: it escapes with a bounded budget.
    if (front) { assert(d.left < 0 && d.right < 0); assert(e.state == State::Escape); }
    else { assert(d.left > 0 && d.right > 0); assert(e.state == State::Escape); }
  }

  // ---- Edge escape: hold after a push, then survive ----
  {
    Machine e = plain();
    beginRound(e, 0);
    fresh(r, 1000, 0, 100, 0);
    at(e, 1000); e.tick(r, false);
    at(e, 1010, 1); e.tick(r, false);       // enter Escape
    fresh(r, 1050); at(e, 1050, 0); e.tick(r, false);
    fresh(r, 1090); at(e, 1090, 0); stopped(e.tick(r, false));
    assert(e.state == State::Hold);
    fresh(r, 1100); at(e, 1100, 8); d = e.tick(r, false);   // rear reflex preempts hold
    assert(d.left > 0 && d.right > 0);
    assert(e.state == State::Escape);
  }

  // ---- Escape budget turns a broken threshold into a visible fault ----
  {
    Machine e = plain();
    e.tuning.maxEscapesPerRound = 3;
    beginRound(e, 0);
    fresh(r, 1000); at(e, 1000);
    e.escapeCount = 3;                       // three already spent this round
    at(e, 1010, 1); d = e.tick(r, false);
    stopped(d);
    assert(e.state == State::Fault);
    assert(strcmp(reasonName(e.history.newest(0)->reason), "escape budget exhausted") == 0);
    // The same button press that starts a round clears the fault.
    e.clearFault();
    assert(e.state == State::Stopped);
    assert(e.escapeCount == 3);              // cleared by startRound, not by clearFault
    beginRound(e, 1100);
    assert(e.escapeCount == 0);
  }

  // ---- Edge filter: a single noisy sample is not an edge ----
  {
    EdgeFilter f;
    f.confirmMs = 25;
    assert(f.update(0, 1) == 0);
    assert(f.update(10, 1) == 0);
    assert(f.update(25, 1) == 1);            // held for 25 ms -> confirmed
    assert(f.update(50, 0) == 1);            // release starts pending
    assert(f.update(75, 0) == 0);
    assert(f.update(80, 1) == 0);            // 5 ms blip is ignored
    assert(f.update(120, 0) == 0);
    assert(f.update(130, 2) == 0);
    assert(f.update(160, 2) == 2);
    f.reset();
    assert(f.confirmed == 0);
  }

  // ---- Manual threshold overrides the auto midpoint ----
  {
    QreCalibration c;
    for (uint8_t i = 0; i < 4; ++i) {
      c.floorMin[i] = 100; c.floorMax[i] = 200;    // floor cluster low
      c.edgeMin[i] = 900;  c.edgeMax[i] = 1000;    // void cluster high
    }
    assert(c.valid());
    assert(c.edgeIsHigh(0));
    assert(c.threshold(0) == 550);
    assert(c.edge(0, 700, false));                 // above 550 + 40
    assert(!c.edge(0, 580, false));                // inside the hysteresis band
    assert(c.edge(0, 580, true));                  // but not once it is already latched

    c.setManual(0, 1500, true);                    // pin it high
    assert(c.manual(0));
    assert(c.threshold(0) == 1500);
    assert(!c.edge(0, 700, false));                // 700 no longer counts
    assert(c.edge(0, 1600, false));
    c.setManual(1, 50, false);                     // invert channel 1
    assert(!c.edgeIsHigh(1));
    assert(c.edge(1, 5, false));                   // below 50 - 40
    assert(c.valid());
    c.clearManual(0);
    assert(!c.manual(0) && c.threshold(0) == 550);

    // A hand-set channel does not need the 300-count gap.
    QreCalibration bad;
    for (uint8_t i = 0; i < 4; ++i) {
      bad.floorMin[i] = 100; bad.floorMax[i] = 200;
      bad.edgeMin[i] = 900;  bad.edgeMax[i] = 1000;
    }
    bad.edgeMin[0] = 250; bad.edgeMax[0] = 300;   // only 50 counts of gap
    assert(!bad.valid());
    bad.setManual(0, 900, true);
    assert(bad.valid());
  }

  // ---- Side target steering ----
  {
    Machine e = plain();
    beginRound(e, 0);
    fresh(r, 1000, 400, 0, 0);
    at(e, 1000); d = e.tick(r, false); assert(d.left < 0 && d.right > 0);
    fresh(r, 1100, 0, 0, 400);
    at(e, 1100); d = e.tick(r, false); assert(d.left > 0 && d.right < 0);
    fresh(r, 1200); at(e, 1200); d = e.tick(r, false);
    assert(d.left != 0 || d.right != 0);
  }

  // ---- Round timeout and emergency stop ----
  {
    Machine e = plain();
    beginRound(e, 0);
    fresh(r, 1000, 0, 500, 0); at(e, 1000); e.tick(r, false);
    fresh(r, 120000); at(e, 120000); stopped(e.tick(r, false));
    assert(e.state == State::Stopped);
    beginRound(e, 120001);
    fresh(r, 121001); at(e, 121001); stopped(e.tick(r, true));
    assert(e.state == State::Stopped);
  }

  // ---- Stale sensor data ends the round (not a hardware fault) ----
  {
    Machine e = plain();
    beginRound(e, 0);
    fresh(r, 1000, 0, 500, 0); at(e, 1000); e.tick(r, false);
    fresh(r, 1100, 0, 500, 0); r[1].sampledMs = 0;
    at(e, 1100); stopped(e.tick(r, false));
    assert(e.state == State::Stopped);
    assert(strcmp(reasonName(e.history.newest(0)->reason), "stale sensor data") == 0);
  }

  // ---- Long push -> unstick -> turn -> search ----
  {
    Machine e = plain();
    beginRound(e, 0);
    fresh(r, 1000, 0, 100, 0); at(e, 1000); e.tick(r, false);
    fresh(r, 6999, 0, 100, 0); at(e, 6999); e.tick(r, false); assert(e.state == State::Push);
    fresh(r, 7000, 0, 100, 0); at(e, 7000); d = e.tick(r, false);
    assert(e.state == State::Unstick && d.left < 0 && d.right < 0);
    fresh(r, 7149); at(e, 7149); e.tick(r, false); assert(e.state == State::Unstick);
    fresh(r, 7150); at(e, 7150); e.tick(r, false); assert(e.state == State::Turn);
    fresh(r, 7400); at(e, 7400); e.tick(r, false); assert(e.state == State::Search);
  }

  // ---- A stuck edge holds instead of ending the round ----
  {
    Machine e = plain();
    beginRound(e, 0);
    fresh(r, 1000); at(e, 1000, 1); e.tick(r, false);
    const uint32_t cap = 1000 + e.tuning.escapeMaxMs;
    fresh(r, cap - 1); at(e, cap - 1, 1); d = e.tick(r, false);
    assert(d.left < 0 && e.state == State::Escape);
    fresh(r, cap); at(e, cap, 1); stopped(e.tick(r, false));
    assert(e.state == State::Hold);
    assert(e.state != State::Stopped);
  }

  // ---- A ghost target does not hold Approach for the whole round ----
  {
    Machine e = plain();
    beginRound(e, 0);
    fresh(r, 1000, 0, 500, 0); at(e, 1000); e.tick(r, false);
    assert(e.state == State::Approach);
    const uint32_t cap = 1000 + APPROACH_MAX_MS;
    fresh(r, cap - 1, 0, 500, 0); at(e, cap - 1); e.tick(r, false);
    assert(e.state == State::Approach);
    fresh(r, cap, 0, 500, 0); at(e, cap); e.tick(r, false);
    assert(e.state == State::Search);
  }

  // ---- Carry hands back when the bottle is no longer held ----
  {
    Machine e;
    e.tuning.openingEnabled = false;
    beginRound(e, 0);
    fresh(r, 1000, 0, 100, 0); at(e, 1000); e.tick(r, false);
    const uint32_t enter = 1000 + e.tuning.carryAfterMs;
    fresh(r, enter, 0, 120, 0); at(e, enter); e.tick(r, false);
    assert(e.state == State::Carry);
    // Three samples beyond carryFarMm hand back to the normal approach.
    for (uint32_t t2 = enter + 10; t2 <= enter + 30; t2 += 10) {
      fresh(r, t2, 0, uint16_t(e.tuning.carryFarMm + 40), 0);
      at(e, t2); e.tick(r, false);
    }
    assert(e.state == State::Approach);   // 300 mm away, so it closes the gap
  }

  // ---- millis() rollover ----
  {
    Machine e = plain();
    beginRound(e, UINT32_MAX - 500);
    fresh(r, 499, 0, 500, 0); at(e, 499); d = e.tick(r, false);
    assert(d.left > 0 && d.right > 0);
    fresh(r, 119499); at(e, 119499); stopped(e.tick(r, false));
  }

  // ---- History: every transition is recorded and readable ----
  {
    Machine e = plain();
    beginRound(e, 1000);
    assert(e.history.count == 1);
    fresh(r, 1999, 0, 100, 0); at(e, 1999); e.tick(r, false);
    fresh(r, 2000, 0, 500, 0); at(e, 2000); e.tick(r, false);
    assert(e.history.count == 3);
    const Transition *newest = e.history.newest(0);
    assert(newest && newest->to == State::Approach);
    assert(newest->from == State::Search);
    assert(newest->atMs == 2000);
    assert(strcmp(reasonName(newest->reason), "target seen") == 0);
    assert(strcmp(stateName(newest->to), "approach") == 0);
    assert(e.history.newest(1)->to == State::Search);
    assert(e.history.newest(2)->to == State::Starting);
    assert(e.history.newest(3) == nullptr);
    // Ring buffer wraps without corrupting the newest entry.
    History bare;
    assert(bare.count == 0);
    for (uint16_t i = 0; i < HISTORY_MAX + 10; ++i) {
      bare.push(i, State::Search, State::Push, Reason::TargetSeen, uint8_t(i & 15), QRE);
    }
    assert(bare.count == HISTORY_MAX);
    assert(bare.newest(0)->atMs == HISTORY_MAX + 9);
    assert(bare.newest(0)->to == State::Push);
    assert(bare.newest(HISTORY_MAX - 1)->atMs == 10);
    assert(bare.newest(HISTORY_MAX) == nullptr);
  }

  // ---- Holding a push does not spam the history ----
  {
    Machine e = plain();
    beginRound(e, 0);
    fresh(r, 1000, 0, 100, 0); at(e, 1000); e.tick(r, false);
    const uint8_t before = e.history.count;
    for (uint32_t t = 1010; t <= 1500; t += 5) {
      fresh(r, t, 0, 100, 0); at(e, t); e.tick(r, false);
    }
    assert(e.state == State::Push);
    assert(e.history.count == before);
  }

  // ---- Power scale reaches the wheels ----
  {
    Machine e = plain();
    beginRound(e, 0);
    fresh(r, 1000, 0, 100, 0); at(e, 1000); e.tick(r, false);
    fresh(r, 1100, 0, 100, 0); at(e, 1100); d = e.tick(r, false);
    assert(d.left > 0);
    const int full = d.left;
    e.tuning.powerScale = 50;
    fresh(r, 1200, 0, 100, 0); at(e, 1200); d = e.tick(r, false);
    assert(d.left > 0 && d.left < full);
  }

  // ---- Display codes ----
  assert(stateDisplay(State::Stopped) == 0);
  assert(stateDisplay(State::Search) == 2);
  assert(stateDisplay(State::Opening) == DISPLAY_OPENING);
  assert(stateDisplay(State::Carry) == DISPLAY_CARRY);
  assert(stateDisplay(State::Updating) == DISPLAY_UPDATING);
  assert(stateDisplay(State::Fault) == 25);
  assert(searchDisplayPhase(0, 0) == DISPLAY_SEARCH_TURN);
  assert(searchDisplayPhase(320, 0) == DISPLAY_SEARCH_OBSERVE);
  assert(searchDisplayPhase(500, 0) == DISPLAY_SEARCH_ADVANCE);
  assert(searchDisplayPhase(800, 0) == DISPLAY_SEARCH_OBSERVE);

  puts("arduino state machine assertions passed");
}
