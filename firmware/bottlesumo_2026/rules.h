// rules.h — pure IR edge rules: the 3+ stop and the operator's edge reaction
// table. No Arduino dependency, so tests/bottlesumo-2026-rules-test.cpp can
// check it.
//
// Bit order is fixed by PIN_IR and the web page:
//   bit0 = FL (GP4, top-left)     bit1 = FR (GP1, top-right)
//   bit2 = RL (GP5, bottom-left)  bit3 = RR (GP2, bottom-right)
//
// The operator's A labels map to those bits as:
//   A0 = bottom-left  = RL = bit2
//   A1 = top-left     = FL = bit0
//   A2 = top-right    = FR = bit1
//   A3 = bottom-right = RR = bit3
//
// The table below is the operator's, rewritten in PIN_IR bit order. A set bit
// means that sensor sees the edge (ADC below its threshold).
#pragma once
#include <stdint.h>
#include "config.h"

namespace bs {

inline uint8_t popcount8(uint8_t v) {
  uint8_t n = 0;
  while (v) { n = uint8_t(n + (v & 1u)); v >>= 1; }
  return n;
}

// Three or more channels off the table: stop, do not try to escape.
inline bool edgeFails(uint8_t bits) { return popcount8(bits) >= 3; }

struct Reaction { int8_t left; int8_t right; };

// PIN_IR bits -> motor command, percent. Positive is forward on that wheel.
//   0b0000 all clear                     forward 70
//   0b1111/0b1110/0b1101/0b1011/0b0111   stop (edgeFails, {0,0} here)
//   0b0001 FL                 turn right 30
//   0b0010 FR                 turn left 30
//   0b0100 RL (A0)            turn right 30
//   0b1000 RR (A3)            turn left 30
//   0b0011 FL+FR (top pair)   backward 70
//   0b1100 RL+RR (bottom pair) forward 70
//   0b0101 FL+RL (left side)  turn right 50
//   0b1010 FR+RR (right side) turn left 50
//   0b0110 RL+FR (A0+A2)      slight right 40
//   0b1001 FL+RR (A1+A3)      slight left 40
inline Reaction reactionFor(uint8_t bits) {
  switch (bits & 0x0F) {
    case 0x0: return {EDGE_FORWARD, EDGE_FORWARD};
    case 0x1: return {EDGE_TURN_30, -EDGE_TURN_30};   // FL
    case 0x2: return {-EDGE_TURN_30, EDGE_TURN_30};   // FR
    case 0x4: return {EDGE_TURN_30, -EDGE_TURN_30};   // RL
    case 0x8: return {-EDGE_TURN_30, EDGE_TURN_30};   // RR
    case 0x3: return {-EDGE_BACKWARD, -EDGE_BACKWARD}; // front pair
    case 0xC: return {EDGE_FORWARD, EDGE_FORWARD};    // rear pair
    case 0x5: return {EDGE_TURN_50, -EDGE_TURN_50};   // left side
    case 0xA: return {-EDGE_TURN_50, EDGE_TURN_50};   // right side
    case 0x6: return {EDGE_SLIGHT_FAST, EDGE_SLIGHT_SLOW}; // RL + FR
    case 0x9: return {EDGE_SLIGHT_SLOW, EDGE_SLIGHT_FAST}; // FL + RR
    default:  return {0, 0}; // 3+ sensors: stop
  }
}

} // namespace bs
