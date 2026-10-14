---
name: bottlesumo-arduino-ci
description: Build and maintain every BottleSumo firmware program with PlatformIO, including the native ESP-IDF robot and receiver plus the existing Arduino sketches. Use when changing firmware, PlatformIO configuration, or the firmware CI workflow.
---

# BottleSumo Firmware Builds

## Build contract

PlatformIO manages every firmware program in this repository:

- Root `platformio.ini`: native ESP-IDF 6.0.1 environments `robot` and `espnow_receiver` for the active ESP32-S3-DEVKITC-1-N16R8.
- Each `firmware/<sketch>/platformio.ini`: one existing Arduino sketch, built for the N16R8 flash profile.
- `platformio-arduino-common.ini`: shared, pinned Arduino-ESP32 3.3.8 PlatformIO platform and board settings. Arduino 3.x currently uses the pinned pioarduino platform release because the root ESP-IDF platform's Arduino package is 2.0.17.
- `boards/esp32-s3-devkitc1-n16r8.json`: shared board definition for the selected 16 MB quad-flash, 8 MB octal-PSRAM module.
- The two ToF sketches pin Pololu VL53L0X by Git commit in their PlatformIO project configs.
- `scripts/validate-firmware.sh`: required all-program gate; discovers every `*.ino`, then builds both native ESP-IDF environments.
- Python used by PlatformIO and the bench tools is managed through `uv.lock`, with Python 3.12, PlatformIO Core 6.2.0, and the required `intelhex`/`pyserial` modules pinned.
- `.github/workflows/platformio-build.yml`: executable CI contract; triggers on push, pull request, and manual dispatch.

The old `ci/arduino-libraries.txt` is retained as a candidate-library inventory for reference. It is not installed by the PlatformIO build. Add actual firmware dependencies to the relevant project's `lib_deps` instead.

## Workflow

1. Read the repository `AGENTS.md`, this skill, the CI workflow, and the relevant hardware and competition documents.
2. Preserve unrelated working-tree changes. The all-program validator is the required compile gate:

   ```bash
   uv sync --frozen
   ./scripts/validate-firmware.sh
   ```

   It uses the locked `uv` environment by default. Set `PLATFORMIO_BIN` only when intentionally using an existing PlatformIO installation.
3. Build one Arduino reference sketch directly when needed:

   ```bash
   uv run --frozen platformio run --project-dir firmware/bottlesumo_competition -e n16r8
   ```

   Use `firmware/bottlesumo_v2_test` or `firmware/movement_test` to select the other sketches. Build the active native environments from the repository root with `-e robot -e espnow_receiver`.
4. Keep `platformio-arduino-common.ini` and leaf project settings synchronized. Do not copy sketches into a central build directory; each source remains in its own PlatformIO project.
5. Run `git diff --check`, verify the workflow and project configurations, and resolve local Markdown links after documentation changes. Preserve the existing feature branch and do not push or merge unless separately requested.
6. Report compile evidence and any hardware behavior that remains unverified.

## Hardware boundary

Compilation does not prove motor direction, GPIO electrical levels, FG polarity, PWM behavior, sensor readings, wiring, or powered-robot safety. Motor FG inputs are documented on GPIO9 and GPIO12 with 9 square-wave pulses per motor revolution. Resolve the firmware-to-schematic pin mismatch before a powered movement test. The `movement_test` sketch commands repeated motion and is not a safe diagnostic with wheels on the ground.

## Project references

- Build workflow: `.github/workflows/platformio-build.yml`
- All-program validator: `scripts/validate-firmware.sh`
- Active native environments: `platformio.ini`
- Shared Arduino settings and board profile: `platformio-arduino-common.ini`, `boards/esp32-s3-devkitc1-n16r8.json`
- Documentation hub: `docs/README.md`
- Active N16R8 GPIO map: `docs/hardware/esp32-s3-devkitc-1-n16r8-pinout.md`
- Motor and FG reference: `docs/hardware/motor-24gp-2430-reference.md`
- Competition rules: `docs/competition/table-sumo-rules-2026.md`
