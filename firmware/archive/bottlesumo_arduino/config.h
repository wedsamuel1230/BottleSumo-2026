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
// QRE corners measured on the assembled robot, front = the ToF side:
//   front-left GPIO4, front-right GPIO1, rear-left GPIO5, rear-right GPIO2.
// The array order is what state_machine.h relies on: bits 0/1 = front pair,
// bits 2/3 = rear pair, bits 0/2 = left side.
constexpr uint8_t PIN_QRE_FL = 4;
constexpr uint8_t PIN_QRE_FR = 1;
constexpr uint8_t PIN_QRE_RL = 5;
constexpr uint8_t PIN_QRE_RR = 2;
constexpr uint8_t PIN_QRE[4]   = {PIN_QRE_FL, PIN_QRE_FR, PIN_QRE_RL, PIN_QRE_RR};
constexpr uint8_t PIN_PWM[2]   = {10, 13};   // left, right
constexpr uint8_t PIN_DIR[2]   = {11, 14};
// Reported from the bench: duty 0, a pin held low, ran the wheel at full
// speed. That points to an active-low PWM input, so the LEDC output is
// inverted: command 0 stops, 255 is full. Verify with 0 = stop and 255 = full
// before a powered drive; if 255 stops, the flag is wrong. Set an entry to
// false for a driver that switches on HIGH.
constexpr bool PWM_ACTIVE_LOW[2] = {true, true};
constexpr uint8_t PIN_FG[2]    = {9, 12};
constexpr uint8_t PIN_XSHUT[3] = {6, 7, 15}; // left, center, right
constexpr uint8_t PIN_SDA      = 17;
constexpr uint8_t PIN_SCL      = 16;
constexpr uint8_t PIN_LED[2]   = {48, 47};   // status LEDs, active high
constexpr uint8_t PIN_SW2      = 42;   // start map A (single board); HIGH pressed
constexpr uint8_t PIN_SW3      = 41;   // start map B (offset double); HIGH pressed

constexpr uint8_t TOF_ADDRESS[3] = {0x30, 0x31, 0x32}; // reassigned every boot

// ---- ToF heads actually fitted --------------------------------------------
// The competition build fits three heads. A bench build with one module fits
// it in a single slot and lists only that head: unused XSHUT lines are then
// never driven, the address stays per head, and the start gate and OTA
// self-test wait only for the fitted heads. A single center head (GPIO7) sees
// a target but has no bearing, so the state machine keeps its scan pattern
// instead of turning toward a side head.
constexpr uint8_t TOF_HEAD_COUNT = 1;
constexpr uint8_t TOF_HEADS[TOF_HEAD_COUNT] = {1}; // 0 = left, 1 = center, 2 = right

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
constexpr int      EDGE_HYSTERESIS = 40;      // ADC counts of hysteresis
constexpr uint32_t OTA_WINDOW_MS = 60000;     // armed by holding BUTTON2
constexpr uint32_t BOOT_VERIFY_MS = 5000;     // self-test before OTA confirm

// ---- Commissioning gates ---------------------------------------------------
// Set HARDWARE_CONFIRMED = true only after the bench checks in README.md.
// Motors stay disabled while this is false.
constexpr bool HARDWARE_CONFIRMED = false;
// Measured default from the movement bench: the gearboxes are mounted
// mirrored, so a straight command needs opposite levels. Equal levels spin the
// robot in place. Flip an entry only after watching that wheel run the other way.
constexpr bool FORWARD_LEVEL[2] = {true, false};
// FG pulses are diagnostic only until their voltage and pull-up are confirmed.
constexpr bool FG_CONNECTED = false;

// ---- FreeRTOS task layout (dual core) --------------------------------------
// Core 1 runs the two hard real-time tasks and nothing else, so a Wi-Fi burst
// or an HTTP response can never delay a control tick or a ranging slot.
// Core 0 runs Wi-Fi/lwIP plus the soft tasks. Priorities are fixed per core:
// they are only comparable within a core, which is why the split matters more
// than the absolute numbers.
constexpr int CORE_REALTIME = 1;
constexpr int CORE_NETWORK  = 0;
constexpr unsigned PRIO_CONTROL = 20;   // core 1, highest
constexpr unsigned PRIO_TOF     = 10;   // core 1, below control
constexpr unsigned PRIO_WEB     = 3;    // core 0
constexpr unsigned PRIO_SERVICE = 2;    // core 0
constexpr uint32_t STACK_CONTROL = 8192;
constexpr uint32_t STACK_TOF     = 4096;
constexpr uint32_t STACK_WEB     = 16384;
constexpr uint32_t STACK_SERVICE = 4096;

// ---- Persistence keys ------------------------------------------------------
// Bumped when the stored layouts change: an old record is then rejected and
// the robot demands a fresh calibration instead of using stale geometry.
constexpr uint32_t CAL_MAGIC   = 0x42534D35; // 'BSM5'  (QRE reorder: old records rejected)
constexpr uint32_t TUNING_MAGIC = 0x42535434; // 'BST4'  (commit path + turn-drag)
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
constexpr uint8_t DISPLAY_OPENING         = 26;
constexpr uint8_t DISPLAY_CARRY           = 27;

} // namespace BS
