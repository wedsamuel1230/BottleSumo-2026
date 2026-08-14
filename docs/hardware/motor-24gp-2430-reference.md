# MY24GP-2430 motor reference

This note records the useful values visible in the supplied Taobao listing for
the selected motor. It is a purchase-listing reference, not a substitute for a
manufacturer datasheet or a bench measurement.

## Selected variant

- Motor family: `MY24GP-2430` planetary geared motor
- Selected reduction ratio in the supplied image: **64:1**
- Nominal supply table recorded: **12 V**
- Motor-before-reduction speed shown: **6000 RPM**
- Gearbox output shaft: 6 mm diameter, according to the supplied dimension image
- Motor body diameter: approximately 24 mm, according to the supplied image

## 12 V, 64:1 listing values

| Parameter | Listing value |
| --- | ---: |
| No-load output speed | 94 RPM |
| Rated output speed | 75 RPM |
| Rated torque | 1.5 kgf.cm |
| Rated current | 0.35 A |
| Rated input power | 2.8 W |
| Limit torque | 3.6 kgf.cm |
| Limit current | 1.4 A |
| Limit input power | 4.5 W |

The 94 RPM no-load value is consistent with approximately `6000 / 64 = 93.75`
RPM before accounting for gearbox losses and listing rounding.

## Motor control and FG

The supplied motor wiring image states:

- PWM speed-control input: 15-25 kHz
- FG output: for external speed measurement
- FG pulse count: 9 square-wave pulses per motor revolution, according to the
  supplied image; confirm whether the seller means the internal or geared
  output shaft
- Direction and PWM are separate control lines in the listing

The image does not establish the ESP32-safe FG voltage, pull-up arrangement,
wire colors, or direction polarity. Verify those on the purchased motor before
connecting it to the controller. See the [ESP32 project pinout reference](esp32-s3-devkitc-1-n32r16-pinout.md)
for the assigned `GPIO9` and `GPIO12` inputs.

For speed measurement:

```text
output_shaft_rpm = fg_frequency_hz * 60 / 9
```

Expected FG frequencies from the 12 V, 64:1 listing are approximately if the
9-pulse count applies at the geared output shaft:

| Operating point | Output speed | Expected FG frequency |
| --- | ---: | ---: |
| Rated | 75 RPM | 11.25 Hz |
| No-load | 94 RPM | 14.10 Hz |

## Mechanical notes

The supplied dimension drawings show a 24 mm body, a 6 mm output shaft, and a
mounting bracket with a 31 mm upright dimension and 33.5 mm base length. Confirm
the selected gearbox ratio and bracket orientation before finalizing the 3D
model mounting holes.

## Source images

The source images supplied for this note are kept beside it:

- [Motor wiring and FG description](../images/motor/motor-24gp-2430-wire-and-fg.png)
- [FG pulse-count note](../images/motor/motor-24gp-2430-fg-pulse-description.png)
- [Motor dimensions](../images/motor/motor-24gp-2430-dimensions.png)
- [64:1 performance table](../images/motor/motor-24gp-2430-64-to-1-specification.png)
