#pragma once
#include "protocol.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <atomic>

namespace BottleSumo {
struct ReceivedDrive { DriveCommand command; uint32_t received_ms; };
extern QueueHandle_t telemetryQueue, commandQueue;
extern std::atomic<bool> otaRequested, otaLocked;
extern std::atomic<uint32_t> otaWindowUntil;
extern std::atomic<uint32_t> radioHeartbeat;
extern uint32_t bootId;
uint32_t nowMs();
bool wirelessInit(bool receiver);
void wirelessMaintain(bool stopped);
void wirelessTask(void *);
bool sendEspnow(const void *, size_t);
bool storeWirelessConfig(const NetworkSettings &);
QueueHandle_t receivedTelemetryQueue();
}
