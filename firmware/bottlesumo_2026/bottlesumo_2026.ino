// bottlesumo_2026.ino — setup only. All work happens in fixed-period tasks:
//   control : core 1, 5 ms, ADC + edge + state + PWM
//   tof     : core 1, 5 ms, one VL53L0X measurement at a time
//   net     : core 0, 5 ms, HTTP + ArduinoOTA
// loop() stays empty on purpose.
#include <Arduino.h>
#include "config.h"
#include "control.h"
#include "net.h"
#include "robot.h"
#include "timing.h"

namespace bs {
TaskTiming g_controlTiming;
TaskTiming g_tofTiming;
TaskTiming g_netTiming;
} // namespace bs

using namespace bs;

namespace {

void tofTask(void *arg) {
  (void)arg;
  TickType_t wake = xTaskGetTickCount();
  uint32_t retryAt = 0;
  for (;;) {
    const uint32_t start = micros();
    if (!tofHealthy()) {
      if (millis() - retryAt >= 2000) {
        retryAt = millis();
        if (tofBegin()) Serial.println("ToF recovered");
      }
    } else {
      uint16_t mm = 0;
      if (tofService(millis(), mm)) controlTofSample(mm, millis());
    }
    timingRecord(g_tofTiming, micros() - start, 3000);
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(5));
  }
}

} // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\nBottleSumo 2026 %s\n", VERSION);

  robotBegin();
  const bool tof = tofBegin();
  Serial.printf("ToF %s%s%s\n", tof ? "ok" : "fail",
                tof ? "" : " - ", tof ? "" : tofError());

  controlBegin();
  netBegin();

  xTaskCreatePinnedToCore(controlTask, "control", 8192, nullptr, 20, nullptr, 1);
  xTaskCreatePinnedToCore(tofTask, "tof", 4096, nullptr, 15, nullptr, 1);
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 3, nullptr, 0);
}

void loop() { delay(1000); }
