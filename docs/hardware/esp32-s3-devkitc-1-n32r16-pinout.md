# ESP32-S3-DEVKITC-1-N32R16 project reference

This document is the working pin and hardware reference for the BottleSumo
controller.

## Board identity

- **Planned physical board:** `ESP32-S3-DEVKITC-1-N32R16`
- **Arduino CLI profile:** `esp32:esp32:esp32s3-octal:FlashSize=32M`
- **Arduino-ESP32 core tested:** `3.3.8`
- **Memory meaning:** `N32R16` denotes 32 MB flash and 16 MB octal PSRAM.
- **Attached PCB schematic label:** `ESP32-S3-DEVKITC-1-N8R8`

The schematic and PCB preview currently carry the older `N8R8` library label.
The production board choice is `N32R16`; the memory variant does not change the
GPIO numbering used by the project. The CAD label should be updated in a future
design revision, but the existing design files are intentionally not rewritten
by the CI setup change.

Source design files:

- [v2 schematic source](../../PCB/v2/Bottlesumo-v2.fsch)
- [v2 PCB source](../../PCB/v2/Bottlesumo-v2.fbrd)
- [v2 schematic preview](../../PCB/v2/SCH.png)
- [v2 PCB preview](../../PCB/v2/PCB.png)

## Project signal map

The table below combines the v2 schematic net names with the motor FG detail
provided for this build.

| GPIO | Project signal | Direction or use | Source / note |
| ---: | --- | --- | --- |
| 1 | `IR1_OUT` | Input | QRE1113 sensor 1 output in schematic |
| 2 | `IR2_OUT` | Input | QRE1113 sensor 2 output in schematic |
| 4 | `IR3_OUT` | Input | QRE1113 sensor 3 output in schematic |
| 5 | `IR4_OUT` | Input | QRE1113 sensor 4 output in schematic |
| 6 | `XSHUT1` | Output | VL53L0X shutdown control, schematic label |
| 7 | `XSHUT2` | Output | VL53L0X shutdown control, schematic label |
| 9 | `MOTOR1_FG` (working assignment) | Input / pulse counter | User note: FG input; 9 square-wave pulses per motor revolution; verify motor loom assignment |
| 10 | `PWM1` | PWM output | Motor 1 speed-control net in v2 schematic |
| 11 | `DIR1` | Digital output | Motor 1 direction net in v2 schematic |
| 12 | `MOTOR2_FG` (working assignment) | Input / pulse counter | User note: FG input; 9 square-wave pulses per motor revolution; verify motor loom assignment |
| 13 | `PWM2` | PWM output | Motor 2 speed-control net in v2 schematic |
| 14 | `DIR2` | Digital output | Motor 2 direction net in v2 schematic |
| 15 | `XSHUT3` | Output | VL53L0X shutdown control, schematic label |
| 16 | `SCL` | I2C clock | v2 schematic; pass explicitly to `Wire.begin()` |
| 17 | `SDA` | I2C data | v2 schematic; pass explicitly to `Wire.begin()` |
| 19 | `USB_D-` | USB data | ESP32-S3 USB function |
| 20 | `USB_D+` | USB data | ESP32-S3 USB function |
| 21 | `LED1` | OUTPUT | LED indicator |
| 43 | `U0TXD` | UART transmit | Board UART signal |
| 44 | `U0RXD` | UART receive | Board UART signal |
| 47 | `LED2` | OUTPUT | LED indicator |
| 48 | `LED3` | OUTPUT | LED indicator |

Power and ground connections are shown in the schematic and must be checked
against the assembled board before power is applied. The motor supply is a
separate 12 V rail in the design; do not connect it to an ESP32 GPIO or 3V3
pin.

## Current firmware pin audit

The existing movement sketch is preserved for CI compilation, but its constants
do not match the v2 schematic signal map:

| Sketch constant | Current GPIO | v2 schematic / planned GPIO | Concern |
| --- | ---: | ---: | --- |
| `LPWM` | 1 | 10 | GPIO1 is `IR1_OUT` in the schematic |
| `LDIR` | 2 | 11 | GPIO2 is `IR2_OUT` in the schematic |
| `RPWM` | 15 | 13 | GPIO15 is `XSHUT3` in the schematic |
| `RDIR` | 14 | 14 | Matches `DIR2` |
| `LED1` | 17 | 21 | GPIO17 is `SDA` in the schematic |
| FG inputs | Not implemented | 9 and 12 | Add pulse capture only after electrical-level validation |

Do not use the current movement sketch for a powered motor test until this
audit is resolved in firmware and reviewed against the physical PCB. CI is a
compile gate, not a hardware approval gate.

## I2C note

The schematic assigns `SCL=GPIO16` and `SDA=GPIO17`. Arduino's generic ESP32-S3
defaults are different, so sensor firmware should set the bus explicitly, for
example:

```cpp
Wire.begin(17, 16);  // SDA, SCL from the BottleSumo v2 schematic
```

The three VL53L0X devices share the I2C bus and use separate XSHUT controls for
address assignment. The exact physical sensor order must follow the labels on
the assembled PCB.

## FG pulse reference

The supplied motor image states that one motor revolution produces **9
square-wave FG pulses**. It does not establish whether that count is referenced
to the internal motor shaft or the geared output shaft, so treat the shaft
location as an open hardware question. For a measured FG frequency, use the
confirmed pulse count and shaft reference:

```text
output_shaft_rpm = fg_frequency_hz * 60 / 9
```

The selected 64:1, 12 V listing gives approximately 75 RPM rated speed and
94 RPM no-load speed. Those values correspond to approximately 11.25 Hz and
14.10 Hz at the FG output if the 9-pulse specification applies at the geared
output shaft.

Before connecting either FG line to GPIO9 or GPIO12:

1. Confirm the motor wire identity and output electrical level from the actual
   motor documentation or a current-limited bench measurement.
2. Confirm whether an external pull-up is required and pull up only to an
   ESP32-safe voltage.
3. Never apply the motor's 12 V rail to an ESP32 GPIO.
4. Validate pulse polarity, frequency, and duty cycle with the motor unloaded
   before enabling closed-loop control.

The motor listing also specifies a 15-25 kHz PWM speed-control input. The
current `analogWrite()` sketch is not evidence that this frequency is configured
and should be replaced or configured deliberately during the motor-control
firmware update.

## Verification boundaries

Verified by repository/toolchain inspection:

- The current sketch compiles with the Arduino-ESP32 3.3.8 core for the 32 MB
  flash and OPI PSRAM configuration.
- The v2 schematic contains the project GPIO nets listed above.

Still requiring hardware evidence:

- The exact purchased board marking and memory population.
- Motor wire colors and FG voltage level.
- Motor direction polarity and PWM input behavior.
- Sensor startup, XSHUT ordering, and I2C readings.
