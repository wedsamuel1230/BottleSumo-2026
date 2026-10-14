# PlatformIO firmware management

PlatformIO is the single firmware build system for the active ESP-IDF robot,
ESP-NOW receiver, and the three existing Arduino programs. The sources stay in
their current folders; no sketch copies are maintained.

## Project layout

| Program | PlatformIO project | Environment | Framework |
| --- | --- | --- | --- |
| Autonomous competition sketch | `firmware/bottlesumo_competition` | `n16r8` | Arduino-ESP32 3.3.8 |
| Sensor/electrical test sketch | `firmware/bottlesumo_v2_test` | `n16r8` | Arduino-ESP32 3.3.8 |
| Legacy movement sketch | `firmware/movement_test` | `n16r8` | Arduino-ESP32 3.3.8 |
| Robot firmware | repository root | `robot` | ESP-IDF 6.0.1 |
| ESP-NOW receiver | repository root | `espnow_receiver` | ESP-IDF 6.0.1 |

The root [`platformio.ini`](../../platformio.ini) uses PlatformIO Espressif32
7.0.1 for native ESP-IDF. All projects use the checked-in
[`esp32-s3-devkitc1-n16r8.json`](../../boards/esp32-s3-devkitc1-n16r8.json)
board profile for the 16 MB quad-flash and 8 MB octal-PSRAM module. Each sketch directory has its own `platformio.ini`
that imports the shared [`platformio-arduino-common.ini`](../../platformio-arduino-common.ini).
That shared file pins the pioarduino PlatformIO release for Arduino-ESP32 3.3.8,
sets the ESP32-S3 16 MB flash build profile, and records the Pololu VL53L0X
library at a fixed Git commit. The test and competition projects declare that
library; the movement project has no external library dependency.

## Build commands

Install the locked Python tools once, then use the repository gate to compile
every sketch and both native environments. It uses `uv` with Python 3.12 and
PlatformIO Core 6.2.0:

```bash
uv sync --frozen
./scripts/validate-firmware.sh
```

To build only one program:

```bash
uv run --frozen platformio run --project-dir firmware/bottlesumo_competition -e n16r8
uv run --frozen platformio run --project-dir firmware/bottlesumo_v2_test -e n16r8
uv run --frozen platformio run --project-dir firmware/movement_test -e n16r8
uv run --frozen platformio run -e robot -e espnow_receiver
```

The validator fails if it discovers an Arduino sketch without a sibling
`platformio.ini`. CI runs the same validator from
`.github/workflows/platformio-build.yml` on pushes, pull requests, and manual
dispatches. PlatformIO build artifacts stay under ignored `.pio/` directories.

A successful build proves compilation only. It does not verify board flash/PSRAM
markings, motor or sensor wiring, GPIO electrical levels, movement safety, or
competition performance. The `movement_test` sketch commands repeated motion;
compile it freely, but do not use it on powered wheels resting on the ground.
