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
| QRE | 1, 2, 4, 5 | Front-left, front-right, rear-left, rear-right; ADC1 |
| Motor1 FG / PWM / DIR | 9 / 10 / 11 | Left wheel |
| Motor2 FG / PWM / DIR | 12 / 13 / 14 | Right wheel |
| I2C SDA / SCL | 17 / 16 | Shared 400 kHz bus |
| ToF XSHUT | 6 / 7 / 15 | Left / center / right; module pull-up release |
| Original ToF GPIO | 18 | Input, not shared as three-head identification |
| LED / start / stop | 40 / 41 / 42 | External button pull-downs; HIGH pressed |
| USB | 19 / 20 | Native USB Serial/JTAG console and receiver bridge |

Addresses 0x30/0x31/0x32 are assigned each boot. Disconnect fixed XSHUT-to-3V3
connections. Do not use PSRAM-reserved GPIO35/36/37 for additional peripherals.
Motor PWM is 20 kHz/8-bit; forward polarity must be measured for each motor.
FG remains disabled and diagnostic-only until voltage/pull-up/shaft reference
are confirmed. See [motor reference](motor-24gp-2430-reference.md).

Use [ESP-IDF operating guide](../competition/esp-idf-wireless.md). The older
[N32R16 reference](esp32-s3-devkitc-1-n32r16-pinout.md) records historical schematic
and legacy Arduino mismatches; those sketches are not the active board profile.
