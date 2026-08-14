# Table-Sumo Design Evidence Ledger

Date: 2026-08-14
Purpose: provenance for `docs/table-sumo-design-v1.md`

## Source register

| ID | Source | Evidence tier | Used for |
|---|---|---|---|
| R1 | Repository rules digest: [docs/rules.md](rules.md) | Local project rule baseline supplied with checkout | Arena, bottle, intentional push, size, weight, battery, autonomy, three-second survival, and unknown configuration |
| R2 | User task, "Competition Context (official 2026 rules)" | Authoritative user-provided rule contract | 2026 facts that must override older external rules |
| S1 | ST, VL53L0X product/datasheet landing material: https://www.st.com/en/imaging-and-photonics-solutions/vl53l0x.html | Primary vendor | VL53L0X family and official API context |
| S2 | ST, VL53L0X datasheet: https://www.st.com/resource/en/datasheet/vl53l0x.pdf | Primary vendor | Device capabilities and known 0x29/XSHUT facts; the task supplied the exact multi-device facts |
| S3 | Pololu, VL53L0X carrier: https://www.pololu.com/product/2490 | Vendor carrier documentation | 25 deg FoV, 2 m nominal maximum, 20 mA typical, 40 mA peak, 400 kHz, XSHUT behavior, 0x29 default, effective-range caveats |
| S4 | Pololu, VL53L1X carrier: https://www.pololu.com/product/3415 | Vendor carrier documentation | 27 deg FoV, ROI, short-mode range/update-rate trade-off, 400 cm favorable-condition claim, carrier electrical notes |
| S5 | Espressif, ESP32-S3 datasheet v2.2: https://documentation.espressif.com/esp32-s3_datasheet_en.pdf | Primary vendor; downloaded and text-checked | Two I2C interfaces, ADC1/ADC2 channels, ADC2/Wi-Fi restriction, strapping pins, GPIO restrictions, 500 mA single-supply recommendation |
| S6 | onsemi QRE1113/QRE1113GR datasheet copy: https://docs.rs-online.com/9e66/A700000007797649.pdf | Component vendor datasheet copy; downloaded and text-checked | 940 nm emitter, phototransistor output, 1 mm example, optical/electrical limits, ambient/distance sensitivity |
| S7 | Robofest BottleSumo 2024 rules: https://www.robofest.net/images/2324/BottleSumo2024_V1.pdf | Related event rules; not controlling | Search/edge/unknown-field emphasis, front sensor side, intentional/unintentional push wording, three-second survival, no-progress condition |
| S8 | Pololu, Patrick's mini-sumo robot: https://www.pololu.com/blog/540/patricks-mini-sumo-robot-covert-ops | Competition report/anecdote | Four corner reflectance layout and multi-direction proximity sensor precedent; reported limitations |
| S9 | Pololu, Brandon's mini-sumo robot: https://www.pololu.com/blog/547/brandons-mini-sumo-robot-black-mamba | Competition report/anecdote | Roaming/spinning search and target-angle/sensor-height trade-offs |
| S10 | Pololu, Applied Robotics with the SumoBot: https://www.pololu.com/file/0J209/AppliedSumo-v1.0.pdf | Educational guide | Search-pattern, edge-avoidance, sensor timing, calibration, and strategy-change context |

## Fact versus inference ledger

### Directly supported facts

- `docs/rules.md` states the two-board offset/non-convex possibility, joint-line
  bottle placement, light-colored table, tape seam, unknown support height,
  bottle dimensions, intentional front contact, three-second survival, and
  hardware limits. [R1, R2]
- The VL53L0X carrier page states 25 deg FoV, nominal 2 m range, 20 mA typical
  active-ranging current, 40 mA peak, 400 kHz I2C, active-low XSHUT, and default
  address `0x29`. [S3]
- The VL53L1X carrier page states a programmable ROI, 27 deg typical FoV,
  short mode around 130 cm and 50 Hz maximum, and medium/long mode trade-offs.
  [S4]
- The ESP32-S3 datasheet states two I2C interfaces, ADC1 and ADC2 channels,
  ADC2/Wi-Fi incompatibility, and GPIO0/3/45/46 strapping roles. It also
  recommends at least 500 mA if a single supply powers the chip. [S5]
- The QRE1113 datasheet states a 940 nm peak emitter, phototransistor output,
  a 1 mm white-paper/mirror example, and response characteristics that are
  dependent on distance, current, and ambient temperature. [S6]
- Related BottleSumo/sumo sources emphasize edge detection, object detection,
  autonomous search, unknown field conditions, and post-push survival. [S7,
  S10]

### Engineering inferences

- Five ToF heads are a good local front-arc compromise because the 25 deg
  footprint is narrow relative to a 75 cm board and a second low/mid geometry
  reduces profile blind spots. This is a design judgment, not a vendor claim.
- A 70 mm low optical center is a practical compromise between seeing a
  roughly 100 mm horizontal bottle profile and detecting ordinary opponent
  chassis structure. It must be confirmed with the actual bottle and robot.
- A 75 mm QRE lookahead can support the illustrative 0.25-0.35 m/s speeds only
  under the stated braking assumptions. The actual attack cap must come from
  the edge-speed failure test.
- ToF-only bottle/opponent classification is not reliable enough to authorize
  a push without front alignment/contact conditions. The sensors report range,
  not rule intent.
- A seam probe can distinguish many finite tape crossings from a persistent
  void, but no reflectance-only classifier can distinguish identical seam and
  void optical responses from one instantaneous sample.
- A color sensor can improve upright-bottle confirmation if calibrated on-site,
  but it is not a replacement for low ToF or edge sensing and is weaker on a
  fallen bottle.
- Two motors are preferable to a third mechanism motor until a mechanical
  proof shows that an active mechanism improves a clean front push without
  creating side-contact or size/weight risk.

### Unresolved assumptions requiring operator confirmation

- Exact ESP32-S3 module/dev-board variant, flash/PSRAM wiring, and exposed GPIOs.
- Existing motor-driver type, enable/standby pins, PWM polarity, stall current,
  and measured braking response.
- Exact QRE1113 item: raw four-pin device, analog module, or digital comparator
  module; output polarity and onboard LED resistor are unknown.
- Exact ToF carrier/module variant, pull-up population, regulator, and whether
  its XSHUT pin is actually exposed.
- Actual chassis dimensions, drive-wheel contact line, front-face geometry,
  sensor bracket space, mass, battery, and ground clearance.
- Tape width/material and whether seam readings are separable from true void.
- Whether front contact switches or a color sensor will be installed.

## Verification boundary

No repository implementation code was added for this design package. Host-side
source extraction and repository inspection do not prove wiring, sensor output,
motor behavior, brownout immunity, edge stopping, intentionality, or match
success. Those claims remain gated on the operator-run Phase 4 milestones.
