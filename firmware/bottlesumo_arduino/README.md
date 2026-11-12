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
| `sensors.h` | QRE sampling, calibration persistence, VL53L0X schedule |
| `qre.h` | Threshold logic and edge debounce; no hardware dependency, so it is host-testable |
| `vl53l0x.h` / `vl53l0x.cpp` | VL53L0X driver ported to Arduino `Wire`; non-blocking single-shot API |
| `web.h` / `web.cpp` | SoftAP, web UI, JSON API, OTA, UDP telemetry |
| `telemetry.h` | 84-byte UDP snapshot, byte-compatible with `tools/bottlesumo_debug` |
| `shared.h` | Queues and the mutex that join the control task to the web task |
| `tests/arduino-state-machine-test.cpp` | Host assertions for the state machine |

## 3. Task split

| Task | Core | Priority | Period | Owns |
| --- | --- | --- | --- | --- |
| `control` | 1 | 20 | 5 ms | QRE ADC, state machine, PWM, telemetry, buttons |
| `tof` | 1 | 10 | 1 ms poll | VL53L0X single-shot schedule (C–L–C–R) |
| `web` | 0 | 3 | 5 ms | HTTP UI, UDP telemetry, OTA window |
| `service` | 0 | 2 | event | Serial CLI, NVS writes |

Core 1 carries only the two hard real-time tasks, so a Wi-Fi burst or an HTTP
response on core 0 can never delay a control tick or a ranging slot. Priorities
are only comparable within a core, which is why the split matters more than the
absolute numbers.

The control task never touches I²C, Wi-Fi or NVS, and no web handler ever
touches a motor. Motor ownership is single-writer: only `controlTask` calls
`setWheel()`. Each task publishes its measured rate and remaining stack into
`live`, and `GET /api/status` returns both, so a starved task is visible.

## 4. Wi-Fi and the web UI

On boot the robot opens a WPA2 access point:

- SSID `BottleSumo`, password `bottlesumo` by default — **change it from the UI**
  (the password is stored in NVS, never compiled into an OTA image)
- URL `http://bottlesumo/` or `http://192.168.4.1/`

The page has: live QRE bars with threshold markers and edge flags, ToF
distances, calibration controls, the per-channel manual threshold editor, the
drive tuning sliders with a live summary of the opening, carry and edge
settings, the state-machine history table, task rates and remaining stack,
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

### Manual threshold override

The automatic midpoint is a convenience, not a requirement. Every channel has a
checkbox, a number field and a polarity selector under **QRE 手動閾值**, next to
its live raw ADC value. Tick a channel and it stops using the calibrated
midpoint entirely: the threshold and polarity you typed are used instead, they
persist in NVS, and the channel no longer has to pass the 300-count gap test.
This is the answer to a surface where the auto calibration is wrong and there is
no time to fix it — the same failure that cost last year's round.

`POST /api/manual` takes `{"mask":0..15,"high":0..15,"t0".."t3":1..4094}`.
`mask` bit i enables manual mode on channel i, `high` bit i sets its polarity.
Send `mask:0` to return every channel to automatic.

### Drive tuning ("voltage")

`powerScale` is the master knob: 100 = full configured PWM, 50 = half. Every
commanded PWM is multiplied by it, so one slider changes the effective motor
voltage without recompiling. Individual levels (push, approach near/far, search
turn, search advance, escape, turn, unstick, steering correction) are separate
sliders.

Three groups are also runtime-tunable, which is the point: last year's numbers
were correct for one table and one battery, and there was no way to adjust them
trackside.

| Group | Fields |
| --- | --- |
| Opening | `openingEnabled`, `openingPwm`, `openingForwardMs`, `openingTurnMs`, `openingTurnDir`, `openingForward2Ms`, `openingAbortMm` |
| Carry | `carryEnabled`, `carryAfterMs`, `carryPwm`, `carryBias`, `carrySweepMs`, `carryMaxMs`, `carryFarMm` |
| Edge | `edgeDebounceMs`, `escapeMaxMs`, `maxEscapesPerRound` |

Changes are accepted only while stopped, apply immediately, and persist in NVS.

## 5. State machine

`state_machine.h` is written to be read and edited:

```cpp
enum class State : uint8_t { Stopped, Starting, Search, Approach, Push,
                             Escape, Turn, Hold, Unstick,
                             Opening = 26, Carry = 27, Updating = 30, Fault = 31 };
```

`Opening` and `Carry` sit outside 0..24 because those values are the display
codes the desktop telemetry tool already understands.

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

### Opening run

The starting position and orientation are **announced at inspection**, not fixed
by the rules (`docs/competition/table-sumo-rules-2026.md` §5). So the opening is
an open-loop script you time for one event, not a constant: after the 1 s start
delay the robot drives straight for `openingForwardMs`, pivots `openingTurnMs`
in `openingTurnDir`, then drives straight for `openingForward2Ms`. Set those
four numbers on the practice table before the first round; they are what makes
the script valid, not the source code.

Three guards keep the open loop honest. A centre ToF reading inside
`openingAbortMm` (300 mm) ends the script early and hands over to normal
approach, so it cannot drive past the bottle — but only while driving straight;
aborting mid-pivot leaves the robot at a partial heading with almost no steering
authority. A confirmed edge during the opening goes to `Escape` like any other
state, and the opening is not resumed afterwards.

The pivot runs at `openingPwm`, so `openingTurnMs` means one thing: the time to
swing the documented angle at the duty you will actually use.

### Carry

Contact alone puts the bottle in front of the robot; the opponent can still
line up a clean approach from the side. After `carryAfterMs` of contact, `Carry`
runs both wheels forward at `carryPwm` with a differential of `carryBias` that
flips every `carrySweepMs`, so the robot arcs and drags the bottle sideways. The
left and right ToF readings pull the arc back toward the bottle when it drifts
off centre; the correction is `|carryBias| + 20`, so it can always reverse the
sweep instead of arcing away for half of every cycle.

Keep `carryBias` well under half of `carryPwm`. At 28 on 120 the inside wheel
still turns at roughly 77 % of the outer one, which drags; at 45 it is 2.2:1 and
the robot pivots on the spot instead.

It leaves after `carryMaxMs`, or after three samples beyond `carryFarMm`
(260 mm — the bottle is not being held any more), and returns to the normal
approach.

The 6 s push budget is measured from the first contact of an engagement, and
survives the `Push` → `Carry` → `Push` cycle.

### Edge robustness

Four layers, in the order they act:

1. Auto calibration finds the midpoint between the floor and void clusters.
2. `EdgeFilter` requires the raw bit to hold for `edgeDebounceMs` before the
   state machine sees it, so one noisy sample cannot trigger an escape.
3. Per-channel manual thresholds override the midpoint entirely, with their own
   polarity, set from the web UI.
4. `escapeCount` is reset every round; past `maxEscapesPerRound` the machine
   enters `Fault` instead of reversing again. A broken threshold then shows up
   as a blinking LED and a fault on the page rather than a robot that reverses
   for the rest of the round.

Escape itself is bounded by `escapeMaxMs` (700 ms). Reaching the cap enters
`Hold` with both motors at zero — never `Stopped`, because ending the round is
what a recoverable edge looks like from the outside as a loss. Front and rear
sensors together are treated as an ordinary edge for the same reason: during the
opening pivot that pattern is a normal corner straddle.

### Losing the bottle

Acquisition needs two ToF samples (~136 ms) but a single miss is only ~66 ms, so
a strict filter believes "lost" far more easily than "found". The state machine
holds the last confirmed centre reading for `CENTRE_HOLD_MS` (200 ms) before it
declares the bottle gone. A bottle pressed against the platen reads below the
sensor's 35 mm floor and is clamped up to 35 mm, which counts as contact rather
than as no target.

`tests/arduino-state-machine-test.cpp` holds 120 assertions covering the
transitions, the priority order, the opening script and its early abort, the
carry arc and its sweep flip, the push budget across carry cycles, the escape
budget, the debounce filter, manual thresholds and polarity, rollover safety,
the history buffer, and that `powerScale` reaches the wheel commands.

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
