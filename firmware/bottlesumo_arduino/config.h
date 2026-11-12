// config.h — board profile, pinout and every compile-time constant.
//
// Edit this file first when the hardware changes. Nothing here is written at
// runtime; live-tunable values (drive levels, calibration, Wi-Fi) are in
// state_machine.h / sensors.h and persist in NVS.
#pragma once
#include <stdint.h>

namespace BS {

// ---------------------------------------------------------------------------
// Board: ESP32-S3 DevKitC-1 N16R8 — 16 MB quad flash, 8 MB octal PSRAM,
// dual core 240 MHz. This is WROOM-1-N16R8, not the N16R8V octal-flash part.
//
// Arduino IDE → Tools:
//   Board              : ESP32S3 Dev Module
//   Flash Size         : 16MB
//   PSRAM              : OPI PSRAM
//   Partition Scheme   : Custom        (uses partitions.csv in this folder)
//   USB CDC On Boot    : Enabled       (Serial over the native USB port)
//   Upload Mode        : UART0         (or USB CDC if you use the native port)
//   Erase All Flash Before Upload : enabled on the first flash only
// ---------------------------------------------------------------------------
constexpr uint32_t CPU_MHZ = 240;
constexpr uint32_t PSRAM_EXPECTED_MB = 8;

// ---- Pins (N16R8, confirmed) ----------------------------------------------
// QRE order is front-left, front-right, rear-left, rear-right.
// ADC1 channels are 0, 1, 3, 4 on GPIO 1, 2, 4, 5.
constexpr uint8_t PIN_QRE[4]   = {1, 2, 4, 5};
constexpr uint8_t PIN_PWM[2]   = {10, 13};   // left, right
constexpr uint8_t PIN_DIR[2]   = {11, 14};
constexpr uint8_t PIN_FG[2]    = {9, 12};
constexpr uint8_t PIN_XSHUT[3] = {6, 7, 15}; // left, center, right
constexpr uint8_t PIN_SDA      = 17;
constexpr uint8_t PIN_SCL      = 16;
constexpr uint8_t PIN_TOF_GPIO = 18;         // input only; not used for ranging
constexpr uint8_t PIN_LED      = 40;
constexpr uint8_t PIN_BTN_START = 41;        // HIGH when pressed (external pull-down)
constexpr uint8_t PIN_BTN_STOP  = 42;        // HIGH when pressed

constexpr uint8_t TOF_ADDRESS[3] = {0x30, 0x31, 0x32}; // reassigned every boot

// ---- PWM / control ---------------------------------------------------------
constexpr uint32_t PWM_HZ = 20000;
constexpr uint8_t  PWM_RES_BITS = 8;          // duty 0..255
constexpr uint32_t CONTROL_PERIOD_MS = 5;     // control task period
constexpr uint32_t REVERSAL_OFF_MS = 25;      // PWM=0 dwell before DIR changes
constexpr uint32_t RAMP_STEP = 12;            // max duty increase per control tick

// ---- Match timing (2026 Rev.1 rules) --------------------------------------
constexpr uint32_t START_DELAY_MS = 1000;
constexpr uint32_t ROUND_MS = 120000;
constexpr uint32_t RANGE_STALE_MS = 250;
constexpr uint32_t SURVIVE_MS = 3500;         // stay on the table after a push
constexpr uint32_t CALIBRATION_MS = 3000;     // floor / void capture window
constexpr uint32_t OTA_WINDOW_MS = 60000;     // armed by holding BUTTON2
constexpr uint32_t BOOT_VERIFY_MS = 5000;     // self-test before OTA confirm

// ---- Commissioning gates ---------------------------------------------------
// Set HARDWARE_CONFIRMED = true only after the bench checks in README.md.
// Motors stay disabled while this is false.
constexpr bool HARDWARE_CONFIRMED = false;
// Measure each wheel independently and change these if a wheel runs backwards.
constexpr bool FORWARD_LEVEL[2] = {true, true};
// FG pulses are diagnostic only until their voltage and pull-up are confirmed.
constexpr bool FG_CONNECTED = false;

// ---- Persistence keys ------------------------------------------------------
constexpr uint32_t CAL_MAGIC   = 0x42534D33; // 'BSM3'
constexpr uint32_t TUNING_MAGIC = 0x42535431; // 'BST1'
constexpr uint32_t WIFI_MAGIC  = 0x42535749;  // 'BSWI'

// ---- Display state codes (telemetry / web UI) ------------------------------
// 0..8 mirror State; the rest are composite display codes kept compatible with
// tools/bottlesumo_debug.
constexpr uint8_t DISPLAY_TARGET_SEEN     = 9;
constexpr uint8_t DISPLAY_SEARCH_TURN     = 10;
constexpr uint8_t DISPLAY_SEARCH_OBSERVE  = 11;
constexpr uint8_t DISPLAY_SEARCH_ADVANCE  = 12;
constexpr uint8_t DISPLAY_BENCH_IDLE      = 20;
constexpr uint8_t DISPLAY_BENCH_DRIVE     = 21;
constexpr uint8_t DISPLAY_CAL_FLOOR       = 22;
constexpr uint8_t DISPLAY_CAL_VOID        = 23;
constexpr uint8_t DISPLAY_UPDATING        = 24;

} // namespace BS
