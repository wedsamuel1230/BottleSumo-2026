# BottleSumo 2026

[![Arduino CLI build](https://github.com/wedsamuel1230/BottleSumo-2026/actions/workflows/arduino-cli-build.yml/badge.svg)](https://github.com/wedsamuel1230/BottleSumo-2026/actions/workflows/arduino-cli-build.yml)

Hardware, PCB, 3D model, and firmware files for the BottleSumo robot.

## Continuous build

Every pushed commit and pull request runs the Arduino CLI build workflow. The
workflow installs Arduino CLI 1.4.1 and Arduino-ESP32 3.3.8, then compiles the
uploaded movement sketch for the planned ESP32-S3-DEVKITC-1-N32R16 board:

```text
esp32:esp32:esp32s3-octal:FlashSize=32M
```

The badge above shows the latest GitHub Actions result. A passing compile only
proves that the sketch builds for the selected toolchain and memory interface;
it does not prove motor direction, sensor readings, FG pulse levels, or other
physical behavior.

The current hardware note assigns the two motor FG inputs to `GPIO9` and
`GPIO12`; the supplied motor reference states 9 FG square-wave pulses per
motor revolution. Electrical level and motor-to-pin assignment still require
bench verification.

## Hardware references

- [ESP32-S3-DEVKITC-1-N32R16 pinout and project wiring](docs/esp32-s3-devkitc-1-n32r16-pinout.md)
- [MY24GP-2430 motor and FG reference](docs/motor-24gp-2430-reference.md)
- [Competition rules digest](docs/rules.md)

## Firmware

The current Arduino sketch is in
[`firmware/movement_test/movement_test.ino`](firmware/movement_test/movement_test.ino).
The pin audit in the board reference must be resolved before applying this
movement test to the assembled v2 PCB.
