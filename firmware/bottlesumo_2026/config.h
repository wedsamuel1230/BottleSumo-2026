// config.h — the only file that knows the hardware.
//
// Change a pin, a speed or a route here; no other file needs an edit.
#pragma once
#include <stdint.h>

namespace bs {

constexpr char VERSION[] = "2026.1";

// ---------------------------------------------------------------------------
// Pins (ESP32-S3 DevKitC-1 N16R8, v2 board, confirmed)
// ---------------------------------------------------------------------------
constexpr uint8_t PIN_PWM[2] = {10, 13};  // left, right
constexpr uint8_t PIN_DIR[2] = {11, 14};
// Bench finding: a low PWM input with duty 0 ran the wheel at full speed, so
// the driver input is active low. Command 0 = stop, 100 = full.
constexpr bool PWM_ACTIVE_LOW[2] = {true, true};
// Gearboxes are mounted mirrored: a straight command needs opposite DIR
// levels. Flip an entry only after watching that wheel run the other way.
constexpr bool FORWARD_LEVEL[2] = {true, false};

// QRE1113 edge sensors, ADC1. Index order is fixed by the state machine:
// 0 = front-left (GP4), 1 = front-right (GP1),
// 2 = rear-left (GP5),  3 = rear-right (GP2).
constexpr uint8_t PIN_IR[4] = {4, 1, 5, 2};

constexpr uint8_t PIN_SDA = 17;
constexpr uint8_t PIN_SCL = 16;
constexpr uint8_t PIN_TOF_XSHUT = 7;   // single center head
constexpr uint8_t TOF_ADDRESS = 0x31;

constexpr uint8_t PIN_BUTTON_A = 42;   // SW2, HIGH pressed
constexpr uint8_t PIN_BUTTON_B = 41;   // SW3, HIGH pressed
constexpr uint8_t PIN_LED[2] = {48, 47};
constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;

// ---------------------------------------------------------------------------
// Loop and limits
// ---------------------------------------------------------------------------
constexpr uint32_t CONTROL_MS = 5;        // IR sample and control tick
constexpr uint32_t START_DELAY_MS = 1000; // hands off after start
constexpr uint32_t ROUND_MS = 120000;     // 2-minute round limit
constexpr uint32_t TOF_STALE_MS = 250;
constexpr uint32_t TOF_TIMEOUT_MS = 80;   // one measurement budget is 33 ms
constexpr uint32_t PWM_HZ = 20000;
constexpr uint8_t PWM_BITS = 8;
constexpr uint32_t REVERSAL_OFF_MS = 25;

// ---------------------------------------------------------------------------
// IR calibration
// ---------------------------------------------------------------------------
constexpr uint32_t CALIBRATION_MS = 3000;
constexpr uint16_t CAL_MIN_GAP = 300;     // floor/void separation, ADC counts
constexpr uint16_t EDGE_HYSTERESIS = 40;
constexpr uint8_t EDGE_CONFIRM_MS = 15;

// ---------------------------------------------------------------------------
// Driving (percent of full duty, -100..100)
// ---------------------------------------------------------------------------
struct Segment { int8_t left; int8_t right; uint16_t ms; };

// 2025 confirmed route: forward 1500 ms, spin right 300 ms, forward 3500 ms.
// Reuse the numbers; adjust B after measuring the Senior B board.
constexpr Segment ROUTE_A[] = {
    {100, 100, 1500},
    {100, -100, 300},
    {100, 100, 3500},
};
constexpr Segment ROUTE_B[] = {
    {100, 100, 1500},
    {100, -100, 300},
    {100, 100, 3500},
};

constexpr int8_t SEARCH_SPIN = 50;
constexpr int8_t SEARCH_ADVANCE = 60;
constexpr int8_t ATTACK_SPEED = 100;
constexpr int8_t ESCAPE_SPEED = 60;
constexpr uint16_t SEARCH_SPIN_MS = 2500;    // 2025 SPIN_SEARCH_TIME
constexpr uint16_t SEARCH_ADVANCE_MS = 3500; // 2025 FOWARD_SERACH_TIME
constexpr uint16_t ESCAPE_MAX_MS = 1500;     // 2025 EMERGENCY_DURATION_MS
constexpr uint16_t EDGE_STOP_MS = 200;       // 3+ sensors: stop first
constexpr uint16_t HOLD_MS = 3500;           // stay up after a push

// ToF target window, millimetres.
constexpr uint16_t TOF_TRIGGER_MM = 350; // attack at or below this
constexpr uint16_t TOF_RELEASE_MM = 400; // keep pushing up to this
constexpr uint16_t TOF_MIN_MM = 35;
constexpr uint16_t TOF_MAX_MM = 900;
constexpr uint16_t TOF_CLOSE_MM = 150;   // lost here means the bottle moved
constexpr uint8_t TOF_CONFIRM = 2;       // samples before a target is real

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------
constexpr uint32_t CAL_MAGIC = 0x42533632; // 'BS62'
constexpr char NVS_NAMESPACE[] = "bottlesumo";

// Calibration is a gate: an uncalibrated robot refuses a match start on
// purpose. Flip this only for a bench session.
constexpr bool REQUIRE_CALIBRATION = true;

} // namespace bs
