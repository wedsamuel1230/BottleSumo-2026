#include "fg.h"

#include <Arduino.h>

volatile uint32_t fgLastUs[2] = {0, 0};
volatile uint32_t fgPeriodUs[2] = {0, 0};
static volatile uint32_t fgLastFallUs[2] = {0, 0};

// A real FG edge follows a long low phase: 35 ms at 90 rpm. The 20 kHz PWM
// carrier couples into the same wire as 50 us chatter, so a rising edge only
// counts when the pin has been low for FG_MIN_LOW_US.
//
// An ADC threshold was tried and rejected: sampling both pins at 1 kHz showed a
// 0..1.6 V carrier, and 1 kHz aliases 20 kHz into fake 3..6 ms periods. The pin
// carries the carrier, not a 14 Hz tacho pulse, so no threshold can recover it.
static constexpr uint32_t FG_MIN_LOW_US = 500;

static void IRAM_ATTR fgEdge(uint8_t wheel, bool rising) {
  const uint32_t now = micros();
  if (!rising) {
    fgLastFallUs[wheel] = now;
    return;
  }
  const uint32_t fall = fgLastFallUs[wheel];
  if (fall == 0 || now - fall < FG_MIN_LOW_US) return;
  if (fgLastUs[wheel] != 0) fgPeriodUs[wheel] = now - fgLastUs[wheel];
  fgLastUs[wheel] = now;
}

void IRAM_ATTR fgOnChange0() { fgEdge(0, digitalRead(FG_PINS[0]) != 0); }
void IRAM_ATTR fgOnChange1() { fgEdge(1, digitalRead(FG_PINS[1]) != 0); }

void fgBegin() {
  pinMode(FG_PINS[0], INPUT_PULLDOWN);
  pinMode(FG_PINS[1], INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(FG_PINS[0]), fgOnChange0, CHANGE);
  attachInterrupt(digitalPinToInterrupt(FG_PINS[1]), fgOnChange1, CHANGE);
}
