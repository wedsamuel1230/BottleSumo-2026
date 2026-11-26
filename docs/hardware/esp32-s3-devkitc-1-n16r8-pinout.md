# ESP32-S3-DEVKITC-1-N16R8 — active controller

The user selected **N16R8** for the native ESP-IDF/PlatformIO migration:
16 MB quad flash, 8 MB octal PSRAM, dual-core ESP32-S3 at 240 MHz.
This is WROOM-1-N16R8, not the WROOM-2 `N16R8V` octal-flash variant.
The checked-in PlatformIO board descriptor is
[`boards/esp32-s3-devkitc1-n16r8.json`](../../boards/esp32-s3-devkitc1-n16r8.json).
The memory modes follow the
[Espressif module datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf).
The purchased module marking still needs physical inspection.

| Signal | GPIO | Confirmed position/use |
| --- | --- | --- |
| QRE (IR edge) | 4, 1, 5, 2 | Top-left, top-right, bottom-left, bottom-right (ToF face up); ADC1 |
| Motor1 FG / PWM / DIR | 9 / 10 / 11 | Left wheel |
| Motor2 FG / PWM / DIR | 12 / 13 / 14 | Right wheel |
| I2C SDA / SCL | 17 / 16 | Shared 400 kHz bus |
| ToF XSHUT | 6 / 7 / 15 | Left / center / right; module pull-up release |
| LED / SW3 / SW2 | 48 + 47 / 41 / 42 | LED2 + LED3 driven together; external pull-downs; HIGH pressed. SW3 starts Senior B, SW2 starts Senior A; either stops a round |
| USB | 19 / 20 | Native USB Serial/JTAG console and receiver bridge |

### QRE (infrared edge) corner map

Confirmed by the user on 2026-11-22 against the assembled robot. Corners are
named on a top view with the ToF/push face at the top of the view, so "top" is
the front of the robot. `GPn` in the wiring notes means GPIO n:

| Top view | GPIO | `PIN_QRE` index | Robot frame |
| --- | ---: | ---: | --- |
| Top-left | 4 | 0 | front-left |
| Top-right | 1 | 1 | front-right |
| Bottom-left | 5 | 2 | rear-left |
| Bottom-right | 2 | 3 | rear-right |

So `PIN_QRE = {4, 1, 5, 2}`, the order `state_machine.h` relies on: bits 0/1 are
the front pair, bits 2/3 the rear pair, bits 0/2 the left side. `CAL_MAGIC` was
bumped to `BSM5` so an old per-index calibration cannot be applied to the
reordered sensors. Any firmware change to this array also needs the corner map
updated here and in the `config.h` comment.

LED1 on GPIO21 is not used by the firmware. The v2 board has no GPIO18
connection; the three ToF heads use XSHUT only.
Addresses 0x30/0x31/0x32 are assigned each boot. Disconnect fixed XSHUT-to-3V3
connections. A bench build may fit a single center head on XSHUT **GPIO7**
(address 0x31): the Arduino firmware drives and polls only the heads listed in
`TOF_HEADS` (`config.h`), and the start gate then ignores the absent heads.
Do not use PSRAM-reserved GPIO35/36/37 for additional peripherals.
Motor PWM is 20 kHz/8-bit; forward polarity must be measured for each motor.
FG remains disabled and diagnostic-only until voltage/pull-up/shaft reference
are confirmed. See [motor reference](motor-24gp-2430-reference.md).

Use [ESP-IDF operating guide](../competition/esp-idf-wireless.md). The older
[N32R16 reference](esp32-s3-devkitc-1-n32r16-pinout.md) records the historical
schematic; the Arduino sketches now use the v2 signal map but are not the active
board profile.
