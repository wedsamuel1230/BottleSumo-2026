// Host assertions for the Arduino state machine. Pure logic, no hardware.
// Build:
//   c++ -std=c++17 -Wall -Wextra -Werror tests/arduino-state-machine-test.cpp -o /tmp/bs-sm-test
//   /tmp/bs-sm-test
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../firmware/bottlesumo_arduino/state_machine.h"
using namespace BS;

static const uint16_t QRE[4] = {700, 700, 700, 700};

static void fresh(Range (&r)[3], uint32_t now, uint16_t left = 0,
                  uint16_t center = 0, uint16_t right = 0) {
  r[0] = {left, now}; r[1] = {center, now}; r[2] = {right, now};
}
static void stopped(Drive d) { assert(d.left == 0 && d.right == 0); }
static void at(Machine &m, uint32_t now, uint8_t edges = 0) { m.beginFrame(now, edges, QRE); }
static void beginRound(Machine &m, uint32_t now) { at(m, now); m.startRound(); }

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

  // ---- Start delay ----
  Machine m;
  Range r[3];
  at(m, 1000); stopped(m.tick(r, false));
  assert(m.state == State::Stopped);
  m.startRound();
  assert(m.state == State::Starting);
  fresh(r, 1999, 0, 100, 0); at(m, 1999); stopped(m.tick(r, false));
  assert(m.state == State::Starting);
  fresh(r, 2000, 0, 500, 0); at(m, 2000);
  Drive d = m.tick(r, false);
  assert(d.left > 0 && d.right > 0 && m.state == State::Approach);

  // ---- Approach -> Push -> Hold ----
  fresh(r, 2100, 0, 100, 0); at(m, 2100); d = m.tick(r, false);
  assert(d.left > 0 && d.right > 0 && m.state == State::Push);
  fresh(r, 2200); at(m, 2200); stopped(m.tick(r, false));
  assert(m.state == State::Hold);
  fresh(r, 5699, 0, 100, 0); at(m, 5699); stopped(m.tick(r, false));
  assert(m.state == State::Hold);
  fresh(r, 5700, 0, 100, 0); at(m, 5700); d = m.tick(r, false);
  assert(d.left > 0 && d.right > 0);
  at(m, 6000); stopped(m.tick(r, false));   // the ranges are now stale
  assert(m.state == State::Stopped);

  // ---- Every corner mask preempts an imminent push ----
  for (uint8_t mask = 1; mask < 16; ++mask) {
    Machine e;
    beginRound(e, 0);
    fresh(r, 1000, 0, 100, 0);
    at(e, 1000); e.tick(r, false);          // leaves the start delay into Push
    at(e, 1005, mask); d = e.tick(r, false);
    const bool front = mask & 3, rear = mask & 12;
    if (front && rear) { stopped(d); assert(e.state == State::Stopped); }
    else if (front) { assert(d.left < 0 && d.right < 0); assert(e.state == State::Escape); }
    else { assert(d.left > 0 && d.right > 0); assert(e.state == State::Escape); }
  }

  // ---- Edge escape: hold after a push, then survive ----
  {
    Machine e;
    beginRound(e, 0);
    fresh(r, 1000, 0, 100, 0);
    at(e, 1000); e.tick(r, false);
    at(e, 1010, 1); e.tick(r, false);       // enter Escape
    fresh(r, 1050); at(e, 1050, 0); e.tick(r, false);
    fresh(r, 1090); at(e, 1090, 0); stopped(e.tick(r, false));
    assert(e.state == State::Hold);
    fresh(r, 1100); at(e, 1100, 8); d = e.tick(r, false);   // rear reflex preempts hold
    assert(d.left > 0 && d.right > 0);
    fresh(r, 1410); at(e, 1410, 8); stopped(e.tick(r, false));
    assert(e.state == State::Stopped);
  }

  // ---- Side target steering ----
  {
    Machine e;
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
    Machine e;
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
    Machine e;
    beginRound(e, 0);
    fresh(r, 1000, 0, 500, 0); at(e, 1000); e.tick(r, false);
    fresh(r, 1100, 0, 500, 0); r[1].sampledMs = 0;
    at(e, 1100); stopped(e.tick(r, false));
    assert(e.state == State::Stopped);
    assert(strcmp(reasonName(e.history.newest(0)->reason), "stale sensor data") == 0);
  }

  // ---- Long push -> unstick -> turn -> search ----
  {
    Machine e;
    beginRound(e, 0);
    fresh(r, 1000, 0, 100, 0); at(e, 1000); e.tick(r, false);
    fresh(r, 6999, 0, 100, 0); at(e, 6999); e.tick(r, false); assert(e.state == State::Push);
    fresh(r, 7000, 0, 100, 0); at(e, 7000); d = e.tick(r, false);
    assert(e.state == State::Unstick && d.left < 0 && d.right < 0);
    fresh(r, 7149); at(e, 7149); e.tick(r, false); assert(e.state == State::Unstick);
    fresh(r, 7150); at(e, 7150); e.tick(r, false); assert(e.state == State::Turn);
    fresh(r, 7400); at(e, 7400); e.tick(r, false); assert(e.state == State::Search);
  }

  // ---- Escape timeout ----
  {
    Machine e;
    beginRound(e, 0);
    fresh(r, 1000); at(e, 1000, 1); e.tick(r, false);
    fresh(r, 1299); at(e, 1299, 1); d = e.tick(r, false); assert(d.left < 0);
    fresh(r, 1300); at(e, 1300, 1); stopped(e.tick(r, false));
    assert(e.state == State::Stopped);
  }

  // ---- millis() rollover ----
  {
    Machine e;
    beginRound(e, UINT32_MAX - 500);
    fresh(r, 499, 0, 500, 0); at(e, 499); d = e.tick(r, false);
    assert(d.left > 0 && d.right > 0);
    fresh(r, 119499); at(e, 119499); stopped(e.tick(r, false));
  }

  // ---- History: every transition is recorded and readable ----
  {
    Machine e;
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

  // ---- Power scale reaches the wheels ----
  {
    Machine e;
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
  assert(stateDisplay(State::Updating) == DISPLAY_UPDATING);
  assert(stateDisplay(State::Fault) == 25);
  assert(searchDisplayPhase(1000, 0) == DISPLAY_SEARCH_TURN);
  assert(searchDisplayPhase(1000 + 320, 0) == DISPLAY_SEARCH_OBSERVE);
  assert(searchDisplayPhase(1000 + 500, 0) == DISPLAY_SEARCH_ADVANCE);

  puts("arduino state machine assertions passed");
}
