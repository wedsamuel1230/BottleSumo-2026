// web.h — Wi-Fi AP, configuration web UI, OTA upload and UDP telemetry.
#pragma once
#include <Arduino.h>
#include "shared.h"

namespace BS {

// Starts the AP, the HTTP server and the UDP telemetry socket. Call once from
// setup(), before the tasks are created.
void webBegin();

// Runs the web server, UDP telemetry and OTA-window bookkeeping. Call from a
// FreeRTOS task; never from the control task.
void webTask(void *arg);

}
