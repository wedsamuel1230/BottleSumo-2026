// rules.h — pure IR edge rules: failure mask and the 2025 escape reactions.
// No Arduino dependency, so tests/bottlesumo-2026-rules-test.cpp can check it.
#pragma once
#include <stdint.h>
#include "config.h"

namespace bs {

inline uint8_t popcount8(uint8_t v) {
  uint8_t n = 0;
  while (v) { n = uint8_t(n + (v & 1u)); v >>= 1; }
  return n;
}

// Three or more channels off the table is a lost round, not an escape.
inline bool edgeFails(uint8_t bits) { return popcount8(bits) >= 3; }

struct Reaction { int8_t left; int8_t right; };

// 2025 escape table, with the left/right commands corrected: the 2025 code
// sent both sides the same command, so a right-hand edge turned the robot
// toward the edge. Bit order: 0=FL, 1=FR, 2=RL, 3=RR.
inline Reaction reactionFor(uint8_t bits) {
  const bool fl = bits & 0x1;
  const bool fr = bits & 0x2;
  const bool rl = bits & 0x4;
  const bool rr = bits & 0x8;
  if (fl && fr) return {-ESCAPE_SPEED, -ESCAPE_SPEED}; // front pair: back off
  if (rl && rr) return {ESCAPE_SPEED, ESCAPE_SPEED};   // rear pair: drive on
  if ((rl && !fl && !fr) || (rr && !fl && !fr)) return {ESCAPE_SPEED, ESCAPE_SPEED};
  if ((fl || rl) && !(fr || rr)) return {ESCAPE_SPEED, -ESCAPE_SPEED};
  if ((fr || rr) && !(fl || rl)) return {-ESCAPE_SPEED, ESCAPE_SPEED};
  return {ESCAPE_SPEED, ESCAPE_SPEED};
}

} // namespace bs
