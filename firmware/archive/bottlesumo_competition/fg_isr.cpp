#include <Arduino.h>

extern volatile uint32_t fgEdges[2];
extern portMUX_TYPE fgMux;

void IRAM_ATTR leftFgISR() {
  portENTER_CRITICAL_ISR(&fgMux);
  fgEdges[0] = fgEdges[0] + 1;
  portEXIT_CRITICAL_ISR(&fgMux);
}

void IRAM_ATTR rightFgISR() {
  portENTER_CRITICAL_ISR(&fgMux);
  fgEdges[1] = fgEdges[1] + 1;
  portEXIT_CRITICAL_ISR(&fgMux);
}
