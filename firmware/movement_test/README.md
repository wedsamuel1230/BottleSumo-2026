# movement_test — Arduino IDE movement bench with Wi-Fi OTA

Diagnostic sketch for the drive train: commands each direction from the serial
monitor and uploads over Wi-Fi afterwards. Motors stay idle until a command
arrives.

Open `movement_test.ino` in the Arduino IDE. Put the robot on a stand with both
wheels off the table for every motor test.

| File | Role |
| --- | --- |
| `movement_test.ino` | pins, motor output, OTA, serial commands, sweep |
| `web_page.h` | the control page served from the robot's own access point |
| `wifi_local.h.example` | copy to `wifi_local.h` (gitignored) for house Wi-Fi + OTA password |
| `platformio.ini` | keeps this sketch in the repository PlatformIO build gate |

## 1. Arduino IDE board

Select **Tools > Board > BottleSumo 2026 (ESP32-S3 N16R8)**. That profile pins
flash size, OPI PSRAM, the `default_16MB` partition table (two 6.25 MiB OTA
slots), USB CDC on boot and 921600 baud, so there is no per-project Tools
setting left to get wrong. Install it once with:

```bash
./scripts/install-arduino-board.sh
```

See [`arduino/bottlesumo-n16r8/README.md`](../../arduino/bottlesumo-n16r8/README.md)
for the partition map, removal and verification.

The repository board descriptor uses the same `default_16MB` layout, so the
PlatformIO build and the IDE build agree.

**The first flash must be over USB.** It writes the bootloader, `otadata` and
the partition table. Only later builds can use the network port.

## 2. Wi-Fi: bench access point plus home network (AP+STA)

The radio runs both interfaces at once:

- **AP** — the robot publishes `BottleSumo-Bench` (WPA2, default password
  `bottlesumo`, 2.4 GHz channel 6, up to 4 clients) and serves the control page
  at `http://192.168.4.1/`. This works with or without `wifi_local.h`.
- **STA** — when `wifi_local.h` holds the house credentials, the robot also
  joins that network. OTA rides on this link, so a phone on the robot's own AP
  never needs the house Wi-Fi.

The AP stays up while STA connects; a station connect can move the AP onto the
router's channel, so a client may have to reconnect. `AP_SSID` and
`AP_PASSWORD` can be overridden in `wifi_local.h`.

The same page answers on the house network: `http://<sta-ip>/` and
`http://bottlesumo-move.local/` (mDNS). Both are the same `WebServer` on port
80, so a laptop on the router drives the car without leaving its own network.

### Control page

| Element | Behaviour |
| --- | --- |
| ▲ ▼ ◀ ▶ | hold to drive, release to stop |
| STOP | stop and cancel the drive |
| duty slider | 32..255, applied to the next command |
| `掃描 w` | run the four-combination pin/polarity sweep |
| `極性 p` | flip PWM polarity |
| `換腳 c` | swap the PWM pair and the DIR pair |
| `平衡 k` | toggle FG speed balance (equal rpm on both wheels) |
| `L−` / `L+` | manual left duty offset −1 / +1 (no FG needed) |
| `R−` / `R+` | manual right duty offset −1 / +1 |
| `左反 1` / `右反 2` | flip one wheel's DIR level |
| `兩輪反 d` | flip both DIR levels |
| `示範 a` | timed forward/left/right/backward demo |
| status line | duty, rpm and Hz per wheel, balance trim, pair, polarity, DIR levels, sweep step, clients, OTA state, STA IP |

Every bench button sends the same character the serial monitor takes
(`GET /key?k=<char>`), so the two paths cannot drift. Drive commands answer
`409` while an OTA upload or the sweep owns the outputs.

Every drive command is a **lease**: the page repeats it every 100 ms and the
wheels stop if 400 ms pass without one. A closed tab, a lost client or a phone
lock screen therefore stops the car. An OTA upload answers `409` to drive
commands and stops the outputs.

The page is served from flash and needs no internet connection. It is plain
HTTP on the robot's own network; there is no login beyond the AP password, so
the AP password is the access control.

### OTA from the house network

1. Generate the credentials header from this Mac's own Wi-Fi settings:

   ```bash
   ./scripts/make-wifi-header.sh --ssid ChanNetwork
   ```

   It reads the network password from the macOS Keychain (the same item the
   Wi-Fi menu uses), writes `firmware/movement_test/wifi_local.h` with mode 600,
   keeps an existing `OTA_PASSWORD`, and prints the SSID and the OTA password
   only. The home password never reaches the terminal or git.
   `wifi_local.h` is gitignored.
2. Pass `--sketch bottlesumo_arduino` for the other sketch folder. Without a
   Keychain item the header keeps an empty password; fill it in by hand.
3. Flash once over USB, open the serial monitor at 115200. On connect the
   sketch prints `Wi-Fi: <ssid>  IP <ip>  RSSI <dbm>` and
   `OTA:  <hostname>.local port 3232`.
4. Later uploads: `Tools > Port > "bottlesumo-move at <ip> (network)"`, then
   Upload. The IDE asks for `OTA_PASSWORD` the first time.

Without `wifi_local.h` the sketch still builds and runs; only OTA is unavailable
and the serial monitor says so.

The ESP32-S3 radio is 2.4 GHz only. A 5 GHz-only SSID never associates; use the
2.4 GHz SSID or a band-steering SSID that allows 802.11b/g/n.

An OTA upload stops the motors and locks out drive commands while it runs.

## 3. Commands

| Key | Action |
| --- | --- |
| `f` / `b` | both wheels forward / backward |
| `l` / `r` | spin left / spin right |
| `s` or `x` | stop, PWM inputs parked at their resting level |
| `+` / `-` | run duty ±16 (32..255) |
| `k` | toggle FG speed balance (equal rpm on both wheels) |
| `<` / `>` | manual left duty offset −1 / +1 |
| `,` / `.` | manual right duty offset −1 / +1 |
| `p` | flip PWM polarity active-low ↔ active-high |
| `c` | swap which GPIO pair carries PWM |
| `d` | flip both DIR levels |
| `1` / `2` | flip the left / right DIR level only |
| `w` | sweep all four pair/polarity combinations, 1.5 s each |
| `a` | timed demo: forward, left, right, backward, 1 s each |
| `i` | status: duty, rpm, trim, polarity, drive, IP, RSSI |
| `?` | command list |

## 4. Pin map

The Eagle v2 netlist puts `PWM1`/`PWM2` on **GPIO10/GPIO13** and `DIR1`/`DIR2`
on **GPIO11/GPIO14** (motor connectors X1/X2, pin 4 = PWM, pin 5 = DIR). The
final bench harness is straight, with the FG wire moved to the input pins:

| Signal | GPIO | Wire |
| --- | --- | --- |
| FG left / right (inputs, never driven) | 9 / 12 | white |
| PWM left / right | 10 / 13 | yellow |
| DIR left / right | 11 / 14 | orange |
| LED2 / LED3 | 48 / 47 | |

The sketch starts on `pwmOnPairA = false`: PWM on GPIO10/13, DIR on GPIO11/14.
GPIO9/12 are left as inputs and are never driven by this sketch.

**Check the map before the first 12 V run.** A swapped harness can put a GPIO
output on the motor's FG line, so both ends drive one node. It can also put the
FG input pin (pull-down) on the motor's PWM input, which the motor reads as
full speed as soon as power is applied. Wheels off the ground for every test.

Stopping (`s`, `x`, any other bench key, or an expired drive lease) ends a
running sweep or demo and restores the pair and polarity that were active when
the sweep started, so the next command never drives with a mapping nobody
chose.

`c` swaps the two pairs and `w` sweeps both pairs against both polarities, so
the bench resolves which one is right instead of the sketch asserting it. The
choice lives in one variable (`pwmOnPairA`), not in four `#define`s.

**The previous default assumed a crossed harness**, putting the 20 kHz carrier
on GPIO11/14 and a static level on GPIO10/13. With the original harness that
fed the carrier to the DIR wire and left the PWM wire at a fixed level, so
every key pivoted. The mapping is now straight and the default matches it;
`c` still swaps it at the bench if a connector is re-crimped.

The two gearboxes are mounted mirrored, so a straight command needs **opposite**
DIR levels; the older sketch drove forward as `LDIR=1, RDIR=0`. The defaults are
`dirForwardHigh = {true, false}`. Equal levels on both pins spin the robot in
place, which is what "it spins instead of driving" looks like. The motor note
says the DIR line is pulled up forwards and tied to the black wire to reverse,
so `dirForwardHigh = {true, false}` also means one wheel is electrically in
reverse to travel forwards.

### FG speed readout and balance

The white FG wires land on GPIO9/12, both inputs. An interrupt records the gap
between rising edges; the sketch shows `Hz` and `rpm` per wheel on the status
line, in the `i` output, and in `announce()`.

```text
rpm = 60 × Hz / 9        (9 pulses per motor revolution, from the listing)
```

`k` toggles the balance loop. While a drive command is active and both wheels
report rpm, it adds ±1 duty per 100 ms to the slower and faster wheel until the
gap is within 2 rpm, bounded to ±40 counts. With no FG pulses both rpm read 0
and the loop stays idle, so an unpopulated FG path cannot change the drive.

Two guards keep noise out of the loop:

- A period outside 2 ms .. 1 s is rejected. The PWM carrier is 50 us, so a pin
  that picks it up reads 0 rpm, not 20000 Hz.
- A rising edge counts only after the pin has been low for 500 us. A real FG
  low phase is 35 ms at 90 rpm; the carrier couples in as 25 us chatter and is
  dropped.
- Three samples must agree within 25 % before a speed is accepted. A floating
  wire produces plausible-looking periods now and then; the gate drops them,
  and any missing reading clears the trim instead of freezing an old offset.

The board's FG path is R4/R5 in series and R6/R7 to ground. The parts are
marked **103** (10 kΩ) on the board; the schematic records no value. That halves the signal: 5 V becomes 2.5 V, which is marginal against
the ESP32-S3 VIH of 2.475 V, and 3.3 V becomes 1.65 V, which is not a valid
high at all. Fit 15–18 kΩ for R6/R7 on a 5 V output, or remove them for a 3.3 V
push-pull output; an open-collector output needs a pull-up to 3V3 instead. The
motor's FG output type is still unconfirmed: measure it before changing parts.

**ADC attempt, 2026-11-21:** sampling both FG pins as analog at 1 kHz (ADC2 on
GPIO12 works with Wi-Fi) showed 0..1.6 V while driving and 0 V (left) / noise
(right) at rest. That is the 20 kHz carrier through the divider, and 1 kHz
aliases it into fake 3..6 ms periods, so a threshold detector cannot recover a
14 Hz pulse. The internal pull-up cannot beat the 10k pull-down either (0.6 V).

**Bench result, 2026-11-21 (after the 500 us filter):** driving at 20 kHz PWM
produced no accepted FG edge on either pin for 5 s. A 200 Hz test build made
GPIO9 report 5/10/15 ms periods (multiples of the new carrier), so that pin sees
the PWM and not a tacho. The white wire idles at 2.58 V. An ESP32 pin at
3.3 V would read 1.65 V through the same divider, so the source is the motor's
~5.16 V output: the wire is FG. While driving, GPIO9 then showed a steady 50 us
period (the 20 kHz carrier) and GPIO12 random 0.4 ms .. 285 ms periods, because
a 2.58 V high sits only 105 mV above VIH and the adjacent PWM wire couples into
it. Raise the divider ratio and re-measure; `perL`/`perR` should read ~71 ms at
90 rpm.

### Fixed trim table

Without a usable FG signal the balance loop never acts, so use the table: four
duty points (64 / 128 / 192 / 255) with a signed offset per wheel, linearly
interpolated for the commanded duty.

| Key | Action |
| --- | --- |
| `<` / `>` | left wheel offset −1 / +1 |
| `,` / `.` | right wheel offset −1 / +1 |

The edit lands on the table point nearest the duty slider. Every change is
written to NVS (namespace `movetrim`) and survives a reboot. The control page
lists the table in the balance card and marks the point being edited.

Tune it on a stand: pick a duty, drive `f`, and add offset to the slower wheel
until both wheels look even. The table removes a fixed mismatch only; it cannot
react to load or battery changes.

## 5. If a wheel still does not turn

The older sketch could not move forward because it wrote duty `0` for
"forward" and then parked the pins with `digitalWrite`. With the pair inverted
that also put a constant level on the PWM input and the 20 kHz carrier on the
DIR input, so the driver never received a speed command at all. The current
sketch commands a real duty, applies the polarity in duty space, and forces
PWM to idle before any DIR change.

Work through these in order, with the wheels free:

1. `i` — prints the pair, polarity, duty and DIR levels in use.
2. `w` — four combinations of pair and polarity, 1.5 s of forward each. Note
   which line moves a wheel. Then apply it with `c` and `p`.
3. A wheel runs at full speed before any command? The PWM input is parked at
   the wrong resting level: `p`.
4. A wheel turns the wrong way: `d`.
5. One wheel pulls harder: check the two rpm values, then let the balance loop
   (`k`) equalise them, or compare with balance off to see the raw gap.

Every setting is printed, so a bench log names the setting that is wrong
instead of leaving it to guesswork. A driver that only moves on a real PWM
signal (not a DC level) will also show up here: `w` drives 20 kHz on the PWM
pair throughout.
