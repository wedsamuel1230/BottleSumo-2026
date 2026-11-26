// fg.h — motor FG capture (white wires on GPIO9/12).
//
// Kept out of the .ino: PlatformIO's .ino-to-C++ converter hoists a prototype
// for every function definition, and a hoisted prototype repeats the
// IRAM_ATTR section attribute with a different __COUNTER__ value, which makes
// the compiler warn that the attribute is ignored.
#pragma once
#include <stdint.h>

constexpr uint8_t FG_PINS[2] = {9, 12};  // left, right

extern volatile uint32_t fgLastUs[2];    // micros() of the last rising edge
extern volatile uint32_t fgPeriodUs[2];  // gap between the last two edges

void fgBegin();
