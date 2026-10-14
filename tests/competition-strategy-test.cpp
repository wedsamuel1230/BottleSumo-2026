#include <assert.h>
#include <stdio.h>
#include "../firmware/bottlesumo_competition/strategy.h"
using namespace BottleSumo;

void fresh(Range (&r)[3], uint32_t now, uint16_t left = 0,
           uint16_t center = 0, uint16_t right = 0) {
  r[0] = {left, now}; r[1] = {center, now}; r[2] = {right, now};
}
void stopped(Drive d) { assert(d.left == 0 && d.right == 0); }

int main() {
  Calibration cal;
  assert(!cal.valid());
  for (int i = 0; i < 4; ++i) {
    cal.floorMin[i] = 600; cal.floorMax[i] = 800;
    cal.edgeMin[i] = 3000; cal.edgeMax[i] = 3300;
  }
  assert(cal.valid()); assert(!cal.edge(0, 700, false)); assert(cal.edge(0, 3100, false));
  cal.floorMin[0] = 3000; cal.floorMax[0] = 3300;
  cal.edgeMin[0] = 600; cal.edgeMax[0] = 800;
  assert(cal.valid()); assert(cal.edge(0, 700, false)); assert(!cal.edge(0, 3100, false));
  cal.floorMin[1] = 2900; assert(!cal.valid());
  Range r[3]; Controller c;
  fresh(r, 1000, 0, 100, 0);
  stopped(c.tick(1000, 0, r));
  c.start(1000);
  stopped(c.tick(1999, 0, r));
  fresh(r, 2000, 0, 500, 0);
  auto d = c.tick(2000, 0, r);
  assert(d.left > 0 && d.right > 0 && c.state == State::Approach);
  fresh(r, 2100, 0, 100, 0);
  d = c.tick(2100, 0, r);
  assert(d.left > 0 && d.right > 0 && c.state == State::Push);
  fresh(r, 2200);
  stopped(c.tick(2200, 0, r));
  assert(c.state == State::Hold);
  fresh(r, 5699, 0, 100, 0);
  stopped(c.tick(5699, 0, r));
  fresh(r, 5700, 0, 100, 0);
  d = c.tick(5700, 0, r); assert(d.left > 0 && d.right > 0);
  stopped(c.tick(6000, 0, r)); assert(c.state == State::Stopped);

  // Every combination of physical corners must preempt an imminent push.
  for (uint8_t mask = 1; mask < 16; ++mask) {
    Controller edge; edge.start(0); fresh(r, 1000, 0, 100, 0);
    edge.tick(1000, 0, r);
    d = edge.tick(1005, mask, r);
    const bool front = mask & 3, rear = mask & 12;
    if (front && rear) { stopped(d); assert(edge.state == State::Stopped); }
    else if (front) { assert(d.left < 0 && d.right < 0); }
    else { assert(d.left > 0 && d.right > 0); }
  }
  c = Controller{}; c.start(0); fresh(r, 1000, 0, 100, 0);
  c.tick(1000, 0, r); c.tick(1010, 1, r);
  fresh(r, 1050); c.tick(1050, 0, r);
  fresh(r, 1090); stopped(c.tick(1090, 0, r));
  assert(c.state == State::Hold);
  fresh(r, 1100); d = c.tick(1100, 8, r); // Rear reflex also preempts hold.
  assert(d.left > 0 && d.right > 0);
  fresh(r, 1410); stopped(c.tick(1410, 8, r));
  assert(c.state == State::Stopped);

  c = Controller{}; c.start(0); fresh(r, 1000, 400, 0, 0);
  d = c.tick(1000, 0, r); assert(d.left < 0 && d.right > 0);
  fresh(r, 1100, 0, 0, 400);
  d = c.tick(1100, 0, r); assert(d.left > 0 && d.right < 0);
  fresh(r, 1200); d = c.tick(1200, 0, r);
  assert(d.left != 0 || d.right != 0);
  fresh(r, 120000); stopped(c.tick(120000, 0, r));
  assert(c.state == State::Stopped);
  c.start(120001); fresh(r, 121001);
  stopped(c.tick(121001, 0, r, true));

  c = Controller{}; c.start(0); fresh(r, 1000, 0, 100, 0); c.tick(1000, 0, r);
  fresh(r, 6999, 0, 100, 0); c.tick(6999, 0, r); assert(c.state == State::Push);
  fresh(r, 7000, 0, 100, 0); d = c.tick(7000, 0, r);
  assert(c.state == State::Unstick && d.left < 0 && d.right < 0);
  fresh(r, 7149); c.tick(7149, 0, r); assert(c.state == State::Unstick);
  fresh(r, 7150); c.tick(7150, 0, r); assert(c.state == State::Turn);
  fresh(r, 7400); c.tick(7400, 0, r); assert(c.state == State::Search);
  c = Controller{}; c.start(0); fresh(r, 1000); c.tick(1000, 1, r);
  fresh(r, 1299); d = c.tick(1299, 1, r); assert(d.left < 0);
  fresh(r, 1300); stopped(c.tick(1300, 1, r)); assert(c.state == State::Stopped);

  // Unsigned elapsed time must survive millis() rollover.
  c.start(UINT32_MAX - 500); fresh(r, 499, 0, 500, 0);
  d = c.tick(499, 0, r); assert(d.left > 0 && d.right > 0);
  fresh(r, 119499); stopped(c.tick(119499, 0, r));
  c = Controller{}; c.start(0);
  fresh(r, 1000); d = c.tick(1000, 0, r); assert(d.left < 0 && d.right > 0);
  fresh(r, 1320); stopped(c.tick(1320, 0, r));
  fresh(r, 1499); stopped(c.tick(1499, 0, r));
  fresh(r, 1500); d = c.tick(1500, 0, r); assert(d.left == 100 && d.right == 100);
  fresh(r, 1800); stopped(c.tick(1800, 0, r));
  fresh(r, 2000); d = c.tick(2000, 0, r); assert(d.left > 0 && d.right < 0);
  puts("competition strategy assertions passed");
}
