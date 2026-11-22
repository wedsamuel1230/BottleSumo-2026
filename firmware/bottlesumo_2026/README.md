# bottlesumo_2026 — Arduino + FreeRTOS rewrite

Clean rebuild of the competition firmware: the 2025 Pico behaviour, ported to
the ESP32-S3 N16R8 with ArduinoOTA, a bench web page and per-task timing
evidence. The previous 4 000-line firmware stays in `firmware/bottlesumo_arduino`
until this one passes a bench round.

## 1. Tasks and timing budget

`loop()` is empty. Three tasks run on fixed periods with `vTaskDelayUntil()`.

| Task | Core | Prio | Period | WCET budget | Work |
| --- | ---: | ---: | ---: | ---: | --- |
| `control` | 1 | 20 | 5 ms | 1.5 ms | 4×ADC, edge filter, state machine, 2×LEDC |
| `tof` | 1 | 15 | 5 ms poll | 3 ms | one 33 ms VL53L0X measurement at a time |
| `net` | 0 | 3 | 5 ms | 5 ms | HTTP, ArduinoOTA, Wi-Fi retry |
| `loop` | 0 | 1 | — | — | `delay(1000)` only |

Edge reaction worst case: 5 ms sampling + 15 ms debounce + ≤1.5 ms work =
**21.5 ms**, inside the 20–25 ms target. `/api/status` reports each task's
`maxUs` and deadline misses (`*Miss`). The numbers above are budgets until the
bench run records measured values.

OTA flash writes stall the instruction cache for tens of milliseconds. That is
why `controlSetInhibited(true)` stops the motors before the first write and
keeps them stopped for the whole upload.

Measured on 2026-11-22 after the first OTA boot, idle, 11 s uptime:
control 1 092 µs, ToF 1 486 µs, net 1 883 µs, zero deadline misses. Re-measure
during a full round before trusting the headroom.

## 2. State machine

`enum class State` in `control.h`; one function per state in `control.cpp`.

| State | Behaviour | Leaves when |
| --- | --- | --- |
| `Init` | boot only | `controlBegin()` moves to `Idle` |
| `Idle` | motors off; waits for a button or the web | start command |
| `Debug` | web remote drive, 500 ms dead-man | `/api/debug?on=0`, button, stop |
| `StartDelay` | motors off, 1 000 ms hands-off | timer |
| `Route` | 2025 route table per division | table end, edge |
| `Search` | advance 3 500 ms ↔ spin 2 500 ms | ToF ≤ 350 mm, edge |
| `Attack` | forward while ToF ≤ 400 mm | target lost/far, edge |
| `Escape` | 2025 reaction table, ≤1 500 ms | edges clear 60 ms, timeout |
| `Hold` | motors off, 3 500 ms after a close push | timer |
| `Failed` | **3+ IR channels off the table** | stop command |
| `RoundEnd` | stop, 2-minute limit or stop command | next start |

Priority inside one tick: OTA inhibit > 3+ edge failure > round timeout >
escape > the current state. The 3+ rule is deliberate: that case is a lost
round, not an escape.

`Route` uses the 2025 numbers: forward 1 500 ms → spin right 300 ms → forward
3 500 ms. `ROUTE_A` and `ROUTE_B` are separate arrays with the same numbers
until the Senior B board is measured. `Search` alternates advance and spin,
reversing the spin direction after each cycle.

## 3. Pins

| Signal | GPIO |
| --- | --- |
| IR FL / FR / RL / RR | 4 / 1 / 5 / 2 |
| Left PWM / DIR | 10 / 11 |
| Right PWM / DIR | 13 / 14 |
| I2C SDA / SCL | 17 / 16 |
| ToF XSHUT (center, address 0x31) | 7 |
| Button A (SW2) / Button B (SW3) | 42 / 41 |
| LEDs | 48, 47 |

`PWM_ACTIVE_LOW = {true, true}` and `FORWARD_LEVEL = {true, false}` come from
the movement bench. Re-check both with the wheels off the ground before a
powered drive.

## 4. Build

```bash
uv run --frozen --python 3.12 platformio run -d firmware/bottlesumo_2026 -e n16r8
```

Arduino IDE: board `BottleSumo 2026 (ESP32-S3 N16R8)`, FQBN
`esp32:esp32:bottlesumo_n16r8`, 16 MB QIO flash, OPI PSRAM, USB CDC on boot.

Host rules test (no hardware):

```bash
c++ -std=c++17 -Wall -Wextra -Werror -Ifirmware/bottlesumo_2026 \
    tests/bottlesumo-2026-rules-test.cpp -o /tmp/bs2026-rules && /tmp/bs2026-rules
```

## 5. First flash, then OTA

First flash over USB:

```bash
arduino-cli compile -b esp32:esp32:bottlesumo_n16r8 --output-dir /tmp/bs2026 \
  firmware/bottlesumo_2026
arduino-cli upload -p /dev/cu.usbmodem* -b esp32:esp32:bottlesumo_n16r8 \
  --input-dir /tmp/bs2026
```

Wi-Fi credentials: copy `wifi_local.h.example` to `wifi_local.h`, or run
`./scripts/make-wifi-header.sh --sketch bottlesumo_2026`. The header is
gitignored; without it the robot runs an AP only.

Every later flash over the LAN (ArduinoOTA, port 3232, password from
`wifi_local.h`):

```bash
arduino-cli upload -p bottlesumo.local -b esp32:esp32:bottlesumo_n16r8 \
  --input-dir /tmp/bs2026 --upload-field password=<ota password>
```

Browser upload: open `http://bottlesumo/`, choose the `.bin`, only in `Idle`.
A browser upload refuses when the robot is not in `Idle`. There is no rollback
self-test, so keep the USB cable and `esptool` available.

Verified on 2026-11-22: `arduino-cli` pushed `2026.1` over ArduinoOTA to
`192.168.128.137`; `/api/status` then reported `"version":"2026.1"`, `idle`,
ToF fresh at 655 mm and Wi-Fi RSSI −47 dBm.

## 6. IR calibration and thresholds

The old failure mode was a hardcoded voltage threshold that no longer matched
the robot. This firmware learns the two clusters instead:

1. Put the robot on the real table, all four sensors over the surface.
2. **Cal floor** on the page (3 s, motors stopped).
3. Support the robot at the same height with all four windows over the drop.
4. **Cal void** (3 s).

Polarity is detected per channel; the threshold is the midpoint and every
channel needs a 300-count gap. A weak calibration does **not** block a start —
it shows `未校準（只警告）`, as requested. A channel can be overridden by hand:
enter a 0–4095 value, pick `high` or `low`, press **Set**; **Auto** returns the
channel to the captured midpoint. Manual values and both envelopes persist in
NVS (`bottlesumo/cal`, magic `BS62`).

## 7. Bench page and API

Connect to the `BottleSumo` AP (password `bottlesumo`) or the home LAN. The page
shows state, division, event, four IR bars with threshold, the edge mask, ToF
distance, Wi-Fi link and per-task timing.

| Endpoint | Method | Effect |
| --- | --- | --- |
| `/api/status` | GET | JSON snapshot |
| `/api/start?a=1\|2` | POST | start Senior A / B |
| `/api/stop` | POST | stop the round |
| `/api/debug?on=1\|0` | POST | enter/leave remote Debug |
| `/api/debug/drive?l=&r=` | POST | −100..100, 500 ms lease |
| `/api/calibrate?mode=floor\|void` | POST | 3 s capture |
| `/api/threshold?ch=&value=&high=` | POST | manual override |
| `/api/threshold/clear?ch=` | POST | back to automatic |
| `/api/update` | POST | browser OTA (.bin) |

Debug ignores the edge rules on purpose. Put the robot on a stand before you
press a drive button; the motors stop 500 ms after the last command.

## 8. Bench checklist (not yet run)

Run these in order. Each row needs eyes on the robot; the page alone does not
prove direction, angle or escape direction.

| Step | Setup | Action | Pass criterion |
| --- | --- | --- | --- |
| 1 | Robot on the real table | `POST /api/calibrate?mode=floor` | progress reaches 100, `calibrated` still 0 |
| 2 | Robot supported, four windows over the drop | `POST /api/calibrate?mode=void` | `calibrated` becomes 1, four thresholds between the clusters |
| 3 | Wheels off the ground | Debug, hold 前 / 後 | both wheels turn the commanded way, stop on release |
| 4 | Wheels off the ground | hold 左轉 / 右轉 | robot yaws left / right, not translate |
| 5 | Wheels off the ground | leave a drive command unsent | motors stop within 500 ms |
| 6 | Table, clear route | Start A | forward 1 500 ms, spin right 300 ms, forward 3 500 ms, then `search` |
| 7 | Table, bottle at 30–40 cm | Start A, let it search | `attack` at ≤350 mm, push forward |
| 8 | Bottle removed during attack | Start A | `hold` for 3.5 s, then `search` |
| 9 | One sensor over the edge | Debug off, start | `escape` starts within 20–25 ms of the confirmed bit |
| 10 | Three sensors over the edge | start | state `failed`, motors stop, no escape |
| 11 | Full 120 s round | start | no `Unstick`/deadlock, record `ctrlUs`, `netUs`, misses |

Compilation proves the code builds for Arduino-ESP32 3.3.8. It proves nothing
about motor direction, IR polarity, ToF range or powered safety.
