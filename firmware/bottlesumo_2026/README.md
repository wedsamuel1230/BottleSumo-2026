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
| `Route` | runtime per-division segment table; the first leg is always a 100/100 straight charge | table end, edge |
| `Search` | advance 3 500 ms ↔ spin 2 500 ms | ToF ≤ 350 mm, edge |
| `Attack` | forward while ToF ≤ 400 mm | target lost/far, edge |
| `Escape` | operator edge table: all clear 70 % forward, front pair 70 % back, rear pair 70 % forward, singles/side pairs turn, diagonals arc gently; 3+ sensors stop | edges clear 40 ms, or the 2-minute round limit |
| `Hold` | motors off, 3 500 ms after a close push | timer |
| `RoundEnd` | stop; 3+ IR channels off the table, 2-minute limit, or stop command | 3 s, then `Idle` |

Priority inside one tick: OTA inhibit > 3+ stop > escape > round timeout >
the current state. Escape keeps reacting for as long as any edge bit is set;
it returns to `Search` (forward) after the bits stay clear for 40 ms. The
2-minute limit still ends the round.

`Route` is a per-division segment table with up to 8 segments. Edit it on the
bench page: one `left,right,ms` line per segment. **Save** writes NVS and the
next start uses it; **Default A/B** restores the array from `config.h`. The
route persists in NVS (`bottlesumo/route`, magic `BSR1`), so `ROUTE_A` /
`ROUTE_B` are defaults, not the live table. Edits are accepted only in `Idle`,
`Debug` or `RoundEnd`. The **first leg always runs 100/100 straight**; its
duration comes from the table, and later legs use their own left/right.

The page's **前進** row accumulates the 100 %-equivalent time the robot is
commanded forward (both wheels > 0) and estimates centimetres with
`FORWARD_MM_PER_S` (default 250 mm/s). It resets on each start and on entering
Debug; `enterRoundEnd()` also prints the value to Serial. This is a commanded
estimate, not encoder odometry; measure the robot with a tape and set
`FORWARD_MM_PER_S` once. `Search` alternates advance and spin, reversing the
spin direction after each cycle.

## 3. Pins

| Signal | GPIO |
| --- | --- |
| IR FL / FR / RL / RR | 4 / 1 / 5 / 2 |
| Left PWM / DIR | 13 / 14 |
| Right PWM / DIR | 10 / 11 |
| I2C SDA / SCL | 17 / 16 |
| ToF XSHUT (center, address 0x31) | 7 |
| Button A (SW2) / Button B (SW3) | 42 / 41 |
| LEDs | 48, 47 |

`config.h` currently swaps the two motor signal pairs against the Eagle
harness: the schematic's left motor is 10/11 and the right is 13/14, while the
firmware maps left to 13/14 and right to 10/11. Treat that as a bench mapping,
not a schematic change; re-check the harness and wheel direction before a
powered drive. `PWM_ACTIVE_LOW = {true, true}` and
`FORWARD_LEVEL = {true, false}` also come from the movement bench.

## 4. Build

Local build and flash: Arduino IDE, board `BottleSumo 2026 (ESP32-S3 N16R8)`,
FQBN `esp32:esp32:bottlesumo_n16r8`, 16 MB QIO flash, OPI PSRAM, USB CDC on
boot. The installed IDE core is esp32:esp32 **3.3.12**.

CI and pre-commit compile gate (PlatformIO, Arduino-ESP32 3.3.8):

```bash
uv run --frozen --python 3.12 platformio run -d firmware/bottlesumo_2026 -e n16r8
```

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
enter a 0–4095 value, pick the trigger side, press **Set**; **Auto** returns the
channel to the captured midpoint. Manual values and both envelopes persist in
NVS (`bottlesumo/cal`, magic `BS62`).

The trigger side is **not** a style choice: `低於閾值` means bit 1 when the ADC
is *below* the value, `高於閾值` means bit 1 when it is *above*. The page shows
the manual value with its side and the captured `auto` midpoint, so a bad
override is visible. A threshold that sits inside the floor's own noise band
(e.g. 80 with a floor of 85) will latch the bit: once confirmed, the 40-count
hysteresis needs the ADC to cross back by 40 counts.

Note the bit order: `bit3..0 = RR RL FR FL`, and the operator's labels are
`A0=RL, A1=FL, A2=FR, A3=RR`.

## 7. Bench page and API

Connect to the `BottleSumo` AP (password `bottlesumo`) or the home LAN. The page
shows state, division, event, four IR bars with threshold, the edge mask,
forward time/distance, ToF distance, Wi-Fi link, per-task timing and the
Route A/B editor.

| Endpoint | Method | Effect |
| --- | --- | --- |
| `/api/status` | GET | JSON snapshot |
| `/api/start?a=1\|2` | POST | start Senior A / B |
| `/api/stop` | POST | stop the round |
| `/api/debug?on=1\|0` | POST | enter/leave remote Debug |
| `/api/debug/drive?l=&r=` | POST | −100..100, 500 ms lease |
| `/api/calibrate?mode=floor\|void` | POST | 3 s capture |
| `/api/threshold?ch=&value=&high=` | POST | manual override; `high` is required: 0 = below is edge, 1 = above is edge |
| `/api/threshold/clear?ch=` | POST | back to automatic |
| `/api/route` | GET | current A/B route tables as JSON |
| `/api/route?div=&count=&l0=&r0=&ms0=…` | POST | replace one table; duty −100..100, time 10..10 000 ms |
| `/api/route/reset?div=A\|B` | POST | restore the compile-time defaults |
| `/api/update` | POST | browser OTA (.bin) |

Debug ignores the edge rules on purpose. Every Debug direction button commands
100 %. Put the robot on a stand before you press a drive button; the motors
stop 500 ms after the last command.

## 8. Bench checklist (not yet run)

Run these in order. Each row needs eyes on the robot; the page alone does not
prove direction, angle or escape direction.

| Step | Setup | Action | Pass criterion |
| --- | --- | --- | --- |
| 1 | Robot on the real table | `POST /api/calibrate?mode=floor` | progress reaches 100, `calibrated` still 0 |
| 2 | Robot supported, four windows over the drop | `POST /api/calibrate?mode=void` | `calibrated` becomes 1, four thresholds between the clusters |
| 3 | Wheels off the ground | Debug, hold 前 / 後 | both wheels turn the commanded way at full speed, stop on release |
| 4 | Wheels off the ground | hold 左轉 / 右轉 | robot yaws left / right, not translate |
| 5 | Wheels off the ground | leave a drive command unsent | motors stop within 500 ms |
| 6 | Table, clear route | Start A | first leg is 100/100 straight, then the configured legs, then `search`; `前進` accumulates |
| 7 | Table, bottle at 30–40 cm | Start A, let it search | `attack` at ≤350 mm, push forward |
| 8 | Bottle removed during attack | Start A | `hold` for 3.5 s, then `search` |
| 9 | One sensor over the edge | Debug off, start | `escape` reacts within 20–25 ms; keeps reacting while the bit holds; 40 ms clear, then `search` forward |
| 10 | Three sensors over the edge | start | state `round-end`, motors stop, no escape |
| 11 | Full 120 s round | start | no deadlock; record `ctrlUs`, `netUs`, `ctrlMiss`, `netMiss` and `前進` |
| 12 | Any round end | wait 3 s | state returns to `idle` without a command |

Compilation proves the code builds for Arduino-ESP32 3.3.8 (CI) and 3.3.12
(IDE). It proves nothing about motor direction, IR polarity, ToF range or
powered safety.
