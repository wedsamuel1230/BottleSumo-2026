---
name: bottlesumo-arduino-ci
description: Build and flash BottleSumo firmware with the Arduino IDE, which is the user's only local build/flash path; PlatformIO is retained only as the repository CI compile gate. Use when changing firmware, the Arduino board profile, or the CI workflow.
---

# BottleSumo Firmware Builds

## Build and flash: Arduino IDE

The user is fully on the Arduino IDE. Give IDE steps first; do not lead with
PlatformIO for local work.

- Board: `BottleSumo 2026 (ESP32-S3 N16R8)`.
- FQBN: `esp32:esp32:bottlesumo_n16r8`.
- Installed core: **esp32:esp32 3.3.12** (verify with `arduino-cli core list`).
- Profile: 16 MB QIO flash, OPI PSRAM, `default_16MB` (two 6.25 MiB OTA slots),
  USB CDC on boot, 240 MHz, 921600 upload.
- Install or repair the profile with `./scripts/install-arduino-board.sh`, then
  restart the IDE. A Boards Manager core update deletes `boards.local.txt`;
  re-run the script after every core update.
- Libraries: `ArduinoOTA`, `WebServer` and `WiFi` ship with the core; the ToF
  sketches also need Pololu `VL53L0X`.
- First flash of an OTA build goes over USB (`/dev/cu.usbmodem*`). Later builds
  use `Tools > Port > "bottlesumo-move at <ip> (network)"`, password from the
  gitignored `wifi_local.h` (`./scripts/make-wifi-header.sh`).
- CLI equivalent of the IDE: `arduino-cli compile -b
  esp32:esp32:bottlesumo_n16r8 <sketch>` and `arduino-cli upload -p <port>
  -b esp32:esp32:bottlesumo_n16r8`. OTA uploads add
  `--input-dir <dir> --upload-field password=<ota password>`.
- Partition map (`default_16MB`): `nvs` 0x9000, `otadata` 0xe000, app0 0x10000,
  app1 0x650000, `spiffs` 0xc90000, `coredump` 0xFF0000. Never move `otadata`
  or app0.

## CI gate: PlatformIO only

PlatformIO is **not** the local build or flash path. It remains the repository
CI and pre-commit compile gate, pinned to Arduino-ESP32 3.3.8.

- Every firmware change must pass `./scripts/validate-firmware.sh` before
  commit. It builds every `firmware/**/*.ino` sketch and both native ESP-IDF
  environments (`robot`, `espnow_receiver`).
- The IDE core (3.3.12) and the CI core (3.3.8) differ. A pass in the IDE is
  not a CI pass; build in the IDE, then run the validator once.
- The native ESP-IDF environments have no Arduino IDE path. The validator is
  their only compile check.
- Keep `platformio-arduino-common.ini` and the leaf `platformio.ini` files for
  CI; do not use them to produce the flashed image.

## Workflow

1. Read the repository `AGENTS.md`, this skill, the CI workflow, and the
   relevant hardware and competition documents.
2. Edit the sketch.
3. Compile and flash with the Arduino IDE board above; USB first, then OTA.
4. Before commit, run `./scripts/validate-firmware.sh`. It invokes PlatformIO
   Core 6.2.0 through the locked `uv` Python 3.12 environment; set
   `PLATFORMIO_BIN` only for an intentional local Core.
5. Run `git diff --check`, verify the workflow and project configuration, and
   check local Markdown links after documentation changes. Preserve the
   existing feature branch; do not push or merge unless separately requested.
6. Report compile evidence and every hardware behavior that remains unverified.

## Hardware boundary

Compilation does not prove motor direction, GPIO electrical levels, FG
polarity, PWM behavior, sensor readings, wiring, or powered-robot safety. Motor
FG inputs are documented on GPIO9 and GPIO12 with 9 square-wave pulses per
motor revolution. Resolve the firmware-to-schematic pin mismatch before a
powered movement test. The `movement_test` sketch commands repeated motion and
is not a safe diagnostic with wheels on the ground.

## Project references

- Arduino IDE board profile: `scripts/install-arduino-board.sh`,
  `firmware/bottlesumo-n16r8/README.md`
- Bench checklist and OTA commands: `firmware/bottlesumo_2026/README.md`
- Build workflow: `.github/workflows/platformio-build.yml`
- All-program validator: `scripts/validate-firmware.sh`
- Native environments: `platformio.ini`
- Shared Arduino CI settings: `platformio-arduino-common.ini`,
  `boards/esp32-s3-devkitc1-n16r8.json`
- Documentation hub: `docs/README.md`
- Active GPIO map: `docs/hardware/esp32-s3-devkitc-1-n16r8-pinout.md`
- Motor and FG reference: `docs/hardware/motor-24gp-2430-reference.md`
- Competition rules: `docs/competition/table-sumo-rules-2026.md`
