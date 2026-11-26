// robot.h — the hardware layer: motors, four IR channels, one ToF head.
// No game logic here.
#pragma once
#include <Arduino.h>

namespace bs {

void robotBegin();                      // safe motor levels first, then I/O
void robotStop();                       // both wheels to their idle level
void motorCommand(int8_t left, int8_t right, uint32_t now);
uint32_t robotForwardMs(); // 100 %-equivalent forward time since reset
uint32_t robotForwardCm(); // estimate from FORWARD_MM_PER_S
void robotResetForward();
void irRead(uint16_t out[4]);

bool tofBegin();                        // false leaves tofError() set
bool tofService(uint32_t now, uint16_t &mm); // true when a new sample landed
bool tofHealthy();
const char *tofError();

} // namespace bs
