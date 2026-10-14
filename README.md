# BottleSumo 2026

[![PlatformIO firmware build](https://github.com/wedsamuel1230/BottleSumo-2026/actions/workflows/platformio-build.yml/badge.svg)](https://github.com/wedsamuel1230/BottleSumo-2026/actions/workflows/platformio-build.yml)

Hardware, PCB, 3D model, and firmware files for the BottleSumo robot.

## Active firmware: ESP-IDF / PlatformIO

The active target is **ESP32-S3 N16R8** (16 MB quad flash, 8 MB octal PSRAM).
Native ESP-IDF 6.0.1 runs separate FreeRTOS control, ranging, wireless and service
tasks. PlatformIO environments `robot` and `espnow_receiver` provide UDP and
encrypted ESP-NOW telemetry, physically selected bench driving, and stopped-only
authenticated OTA with two slots and rollback. See the
[timing, setup and wireless guide](docs/competition/esp-idf-wireless.md).

```bash
uv sync --frozen
./scripts/validate-firmware.sh
uv run --frozen tools/bottlesumo_debug/debugger.py configure
uv run --frozen tools/bottlesumo_debug/debugger.py provision --serial PORT
uv run --frozen tools/bottlesumo_debug/debugger.py gui
```

Private wireless configuration stays in an ignored local JSON file and is USB-provisioned into NVS; OTA images do not embed credentials. AP+STA and code-selectable UDP/ESP-NOW/both modes are supported.
Default firmware disables motors and unconfigured radio services. Hardware
commissioning, RF performance, powered OTA and competition outcomes remain
unverified. PlatformIO manages the native ESP-IDF firmware and every existing
Arduino sketch. The sketches stay in their own folders and share pinned settings
through [`platformio-arduino-common.ini`](platformio-arduino-common.ini).

## Continuous build

Every pushed commit and pull request runs the PlatformIO firmware workflow.
The all-program validator uses the locked `uv` environment with Python 3.12 and
PlatformIO Core 6.2.0 to compile the three Arduino projects and both active
ESP-IDF environments:

```text
robot
espnow_receiver
bottlesumo_competition (Arduino-ESP32 3.3.8)
bottlesumo_v2_test (Arduino-ESP32 3.3.8)
movement_test (Arduino-ESP32 3.3.8)
```

The active native environments use the pinned PlatformIO Espressif32 7.0.1
platform and ESP-IDF 6.0.1. The Arduino projects use the pinned pioarduino
release that provides Arduino-ESP32 3.3.8, and pin Pololu VL53L0X by commit.
The former Arduino CLI FQBN remains only in historical hardware records.

The badge above shows the latest GitHub Actions result. A passing compile only
proves that the sketch builds for the selected toolchain and memory interface;
it does not prove motor direction, sensor readings, FG pulse levels, or other
physical behavior.

The current hardware note assigns the two motor FG inputs to `GPIO9` and
`GPIO12`; the supplied motor reference states 9 FG square-wave pulses per
motor revolution. Electrical level and motor-to-pin assignment still require
bench verification.

[`ci/arduino-libraries.txt`](ci/arduino-libraries.txt) remains a historical
candidate-library inventory. Active firmware dependencies are declared in each
PlatformIO project, so the validator resolves only the library a sketch uses.

## Documentation

Use the [documentation hub](docs/README.md) to navigate the competition,
hardware, CI, and image references.

## Firmware

- [Active ESP-IDF robot](firmware/bottlesumo_idf/main/robot.cpp) and
  [ESP-NOW USB receiver](firmware/bottlesumo_idf/main/receiver.cpp) use the
  N16R8 profile in [platformio.ini](platformio.ini).
- [Autonomous competition program](firmware/bottlesumo_competition/bottlesumo_competition.ino)
  is the Arduino reference, using the v2 test pinout, four corner QRE inputs and three VL53L0X heads.
  Read the [strategy, wiring and commissioning guide](docs/competition/competition-firmware.md).
  Motors are disabled by default until hardware confirmation and calibration.
- [Electrical/function test](firmware/bottlesumo_v2_test/bottlesumo_v2_test.ino)
  retains the supplied single-head test mapping. Its motor duty is 255 (full
  8-bit PWM), despite the printed 25% label; do not treat it as a low-duty test.
- [Legacy movement test](firmware/movement_test/movement_test.ino) has the pin
  mismatch documented in the board reference and is not suitable for the v2 PCB.
