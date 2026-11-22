#include <Arduino.h>

extern volatile uint32_t motor1Edges;
extern volatile uint32_t motor2Edges;

void IRAM_ATTR motor1EdgeISR() {
  motor1Edges = motor1Edges + 1;
}

void IRAM_ATTR motor2EdgeISR() {
  motor2Edges = motor2Edges + 1;
}
