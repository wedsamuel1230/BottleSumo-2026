# BottleSumo 2026 — Arduino IDE firmware

Arduino port of [`firmware/bottlesumo_idf`](../bottlesumo_idf) for the
**ESP32-S3 DevKitC-1 N16R8** (16 MB quad flash, 8 MB octal PSRAM, 240 MHz dual
core). Same pinout, same three-head VL53L0X schedule, same competition
strategy. Adds a configuration web UI, guided QRE self-calibration, runtime
drive tuning and OTA with rollback.

Open `bottlesumo_arduino.ino` in the Arduino IDE, or build with PlatformIO
(`pio run -e n16r8`); `platformio.ini` is included so the repository validator
still passes.

## 1. Arduino IDE board settings

`Tools` menu, exactly:

| Setting | Value |
| --- | --- |
| Board | **ESP32S3 Dev Module** |
| Flash Size | **16MB** |
| PSRAM | **OPI PSRAM** |
| Partition Scheme | **Custom** (uses `partitions.csv` in this folder) |
| USB CDC On Boot | **Enabled** |
| Upload Mode | UART0 (or USB CDC if you use the native USB port) |
| CPU Frequency | 240MHz (WiFi) |

`partitions.csv` is the Arduino `default_16MB` layout. `otadata` must stay at
`0xe000` and the first app slot at `0x10000` because the Arduino builder flashes
`boot_app0.bin` and the application image at those fixed offsets.

**First flash must be over USB.** It rewrites the bootloader and the partition
table. After that, OTA works. If you prefer a built-in scheme, `Default 8M
(3MB APP x2)` also works, but it changes the partition table and your stored
calibration is lost.

## 2. What each file does

| File | Role |
| --- | --- |
| `bottlesumo_arduino.ino` | `setup()`, the four FreeRTOS tasks, motor output, telemetry |
| `config.h` | Every pin and compile-time constant |
| `state_machine.h` | **The decision logic.** One `case` per state, one `change()` per transition, plus the history ring buffer |
| `sensors.h` | QRE sampling, calibration math, NVS persistence, VL53L0X schedule |
| `vl53l0x.h` / `vl53l0x.cpp` | VL53L0X driver ported to Arduino `Wire`; non-blocking single-shot API |
| `web.h` / `web.cpp` | SoftAP, web UI, JSON API, OTA, UDP telemetry |
| `telemetry.h` | 84-byte UDP snapshot, byte-compatible with `tools/bottlesumo_debug` |
| `shared.h` | Queues and the mutex that join the control task to the web task |
| `tests/arduino-state-machine-test.cpp` | Host assertions for the state machine |

## 3. Task split

| Task | Core | Period | Owns |
| --- | --- | --- | --- |
| `control` | 1 (prio 20) | 5 ms | QRE ADC, state machine, PWM, telemetry, buttons |
| `tof` | 0 (prio 6) | 1 ms poll | VL53L0X single-shot schedule (C–L–C–R) |
| `web` | 0 (prio 3) | 5 ms | HTTP UI, UDP telemetry, OTA window |
| `service` | 0 (prio 2) | event | Serial CLI, NVS writes |

The control task never touches I²C, Wi-Fi or NVS, and no web handler ever
touches a motor. Motor ownership is single-writer.

## 4. Wi-Fi and the web UI

On boot the robot opens a WPA2 access point:

- SSID `BottleSumo`, password `bottlesumo` by default — **change it from the UI**
  (the password is stored in NVS, never compiled into an OTA image)
- URL `http://bottlesumo/` or `http://192.168.4.1/`

The page has: live QRE bars with threshold markers and edge flags, ToF
distances, calibration controls, drive tuning, the state-machine history table,
OTA, and a motor-test panel.

### Self-calibration

1. Put the robot on the real table and press **1. 採安全面 (3s)**. Move it
   slightly during the window so the sample covers the surface and any seam
   tape. Do not include a real table edge.
2. Support the robot at normal sensor height so all four windows look over the
   real drop, then press **2. 採落差 (3s)**. Do not just lift the robot — that
   changes the background reflection.
3. Each channel needs a floor/void gap of at least **300 ADC counts**. The UI
   shows a separation ratio per channel (≥5 good, 3–5 usable, 1.5–3 risky,
   <1.5 fail) and the detected polarity.
4. The values are written to NVS. Re-calibrate after changing the table,
   lighting, sensor height or tape.

Serial equivalent: `f` then `e` (3 s each), `q` for status, `x` to stop.

### Drive tuning ("voltage")

`powerScale` is the master knob: 100 = full configured PWM, 50 = half. Every
commanded PWM is multiplied by it, so one slider changes the effective motor
voltage without recompiling. Individual levels (push, approach near/far, search
turn, search advance, escape, turn, unstick, steering correction) are separate
sliders.

Changes are accepted only while stopped, apply immediately, and persist in NVS.

## 5. State machine

`state_machine.h` is written to be read and edited:

```cpp
enum class State : uint8_t { Stopped, Starting, Search, Approach, Push,
                             Escape, Turn, Hold, Unstick, Updating, Fault };
```

- Every state has exactly one `case` in `Machine::tick()`.
- Every transition goes through `change(next, reason)`, which appends to a
  64-entry ring buffer (`History`) with the timestamp, the from/to states, the
  reason, the edge mask and all four QRE readings.
- The history is mirrored to the web task and shown in the UI, so a match can
  be replayed afterwards. `GET /api/history` returns it as JSON.
- Priority order is enforced in `tick()`: emergency/round timeout → stale
  sensor data → edge escape → per-state behaviour.
- Drive levels live in `Tuning` and are runtime-editable; the defaults in the
  struct are only a starting point.

`tests/arduino-state-machine-test.cpp` asserts the transitions, the priority
order, the hysteresis-free edge reflex, rollover safety, the history buffer and
that `powerScale` actually reaches the wheel commands.

## 6. OTA

1. Connect to the `BottleSumo` AP.
2. Press **開啟 60s 更新窗口** (or hold BUTTON2 for 2 s while stopped).
3. Choose `bottlesumo_arduino.ino.bin` and press **上傳並重啟**.

The upload is written to the inactive slot. On the next boot the firmware runs
a self-test (PSRAM size, fresh ToF data, web task alive); if it passes the image
is confirmed, otherwise the bootloader rolls back to the previous slot.

OTA is refused unless the robot is stopped. Motors are forced to zero before
the write starts.

## 7. Safety gates

- `HARDWARE_CONFIRMED = false` in `config.h` in the delivered source. Set it to
  `true` only after you have verified wheel direction, QRE mapping and stopping
  distance on the bench, then rebuild.
- `FORWARD_LEVEL` must be measured per motor.
- Bench driving is only available when BUTTON2 is held during boot; the web
  sliders are ignored otherwise.
- FG pulses are diagnostic only (`FG_CONNECTED = false`).
- Motors are disabled during calibration, during OTA, and while a fault is set.

## 8. Differences from the ESP-IDF version

- No ESP-NOW receiver bridge and no HMAC-authenticated UDP bench commands. The
  web UI replaces them for tuning and OTA; the UDP telemetry packet keeps the
  same 84-byte layout so the existing desktop dashboard still reads it.
- No optional station mode: the robot is an access point only.
- PWM is 8-bit 20 kHz via `ledcAttach`, not the IDF LEDC driver directly.
- The QRE thresholds use the same 300-count gap rule and 40-count hysteresis.

## 9. Not verified

Compilation and the host state-machine assertions are verified. The following
need bench or table time and are **not** proven by this delivery: motor
direction and PWM polarity, QRE readings on the real table, ToF initialization
on the real heads, stopping distance, RF range, a powered OTA cycle, and
competition outcomes.
