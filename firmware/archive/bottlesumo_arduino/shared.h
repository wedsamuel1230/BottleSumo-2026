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
  int16_t a = 0;   // bench: left PWM; start: map type
  int16_t b = 0;   // bench: right PWM
  Reason why = Reason::None;
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
  uint8_t escapeCount = 0;        // mirrored from the machine for the UI
  uint8_t mapType = 0;            // MapType of the current or last round
  char tofError[24] = {};         // failing stage from TofState::begin()
  uint8_t tofFailHead = 0xFF;
  uint16_t stackFree[4] = {};     // control, tof, web, service (bytes left)
  uint16_t taskHz[4] = {};        // measured ticks per second, 0 = not yet
  History history{};
};

// Fixed per-wheel duty offsets, four duty points, linearly interpolated. The
// FG tacho is unusable on this build, so this table is the only trim.
struct TrimTable {
  static constexpr uint8_t N = 4;
  uint8_t duty[N] = {64, 128, 192, 255};
  int8_t trim[2][N] = {};

  int8_t at(uint8_t wheel, int commandDuty) const {
    if (wheel > 1) return 0;
    if (commandDuty <= duty[0]) return trim[wheel][0];
    for (uint8_t i = 1; i < N; ++i) {
      if (commandDuty <= duty[i]) {
        const int span = int(duty[i]) - int(duty[i - 1]);
        const int offset = commandDuty - int(duty[i - 1]);
        const int low = trim[wheel][i - 1];
        const int high = trim[wheel][i];
        return int8_t(low + (high - low) * offset / span);
      }
    }
    return trim[wheel][N - 1];
  }

  uint8_t pointFor(int commandDuty) const {
    uint8_t best = 0;
    int bestDistance = 1000;
    for (uint8_t i = 0; i < N; ++i) {
      const int d = commandDuty > int(duty[i]) ? commandDuty - int(duty[i])
                                               : int(duty[i]) - commandDuty;
      if (d < bestDistance) { bestDistance = d; best = i; }
    }
    return best;
  }
};

extern TrimTable trimTable;
extern volatile bool tofRestartRequest;

// I2C probe runs inside the ToF task, which owns the bus. The web handler only
// posts a request and reads the stored result, so a scan can never overlap a
// VL53L0X transaction.
struct I2cProbeResult {
  volatile bool request = false;
  volatile bool ready = false;
  uint8_t code[112] = {};   // endTransmission() code for addresses 8..119
  int16_t model29 = -1;     // register 0xC0 at 0x29, -1 = read failed
};
extern I2cProbeResult i2cProbe;
void saveTrimTable();
void loadTrimTable();

extern Live live;
extern QueueHandle_t telemetryQueue;
extern QueueHandle_t requestQueue;
extern SemaphoreHandle_t configMutex;

inline void lockConfig() { if (configMutex) xSemaphoreTake(configMutex, portMAX_DELAY); }
inline void unlockConfig() { if (configMutex) xSemaphoreGive(configMutex); }

// Per-task health, published into `live` once a second so the web UI can show
// whether a task is running at its design rate and how much stack is left. A
// starved or dying task is otherwise invisible from the outside.
struct TaskHealth {
  uint32_t windowStart = 0;
  uint32_t ticks = 0;

  void sample(uint8_t slot, uint32_t now) {
    ++ticks;
    if (now - windowStart < 1000) return;
    const uint32_t elapsed = now - windowStart;
    const uint16_t hz = uint16_t(elapsed ? (ticks * 1000u) / elapsed : 0);
    const uint16_t stack = uint16_t(uxTaskGetStackHighWaterMark(nullptr));
    lockConfig();
    live.taskHz[slot] = hz;
    live.stackFree[slot] = stack;
    unlockConfig();
    ticks = 0;
    windowStart = now;
  }
};

inline void sendRequest(Command command, int16_t a = 0, int16_t b = 0,
                        Reason why = Reason::None) {
  if (!requestQueue) return;
  Request r{command, a, b, why};
  xQueueSend(requestQueue, &r, 0);
}

} // namespace BS
