// shared.h — the plumbing between the control task and the web task.
//
//   control task  --writes-->  telemetryQueue   (latest snapshot, overwrite)
//   web task      --reads-->   telemetryQueue
//   web task      --writes-->  requestQueue     (one command at a time)
//   control task  --reads-->   requestQueue
//
// Configuration that both sides touch (calibration, tuning, flags, history)
// lives in `live` and is guarded by `configMutex`. Telemetry uses a single
// entry queue instead of a mutex, so the 5 ms control loop never blocks.
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "config.h"
#include "state_machine.h"
#include "sensors.h"
#include "telemetry.h"

namespace BS {

enum class Command : uint8_t {
  None = 0,
  Start, Stop, ClearFault,
  CalibrateFloor, CalibrateVoid, CalibrateCancel,
  ApplyTuning,
  BenchDrive,
  Reset,
  ArmOtaWindow,
};

struct Request {
  Command command = Command::None;
  int16_t a = 0;   // bench: left PWM
  int16_t b = 0;   // bench: right PWM
};

// Protected by configMutex.
struct Live {
  QreCalibration calibration{};
  bool calibrated = false;
  bool floorCaptured = false;
  char calibrationMode = 0;     // 0 = none, 'f' = floor, 'e' = void
  uint32_t calibrationStart = 0;
  Tuning tuning{};
  bool benchMode = false;
  bool benchActive = false;
  bool faulted = false;
  bool otaWindowArmed = false;
  uint32_t otaWindowUntil = 0;
  bool updating = false;
  bool bootVerified = false;
  uint32_t bootId = 0;
  uint8_t otaProgress = 0;
  uint8_t wifiClients = 0;
  uint32_t webTicks = 0;
  History history{};
};

extern Live live;
extern QueueHandle_t telemetryQueue;
extern QueueHandle_t requestQueue;
extern SemaphoreHandle_t configMutex;

inline void lockConfig() { if (configMutex) xSemaphoreTake(configMutex, portMAX_DELAY); }
inline void unlockConfig() { if (configMutex) xSemaphoreGive(configMutex); }

inline void sendRequest(Command command, int16_t a = 0, int16_t b = 0) {
  if (!requestQueue) return;
  Request r{command, a, b};
  xQueueSend(requestQueue, &r, 0);
}

} // namespace BS
