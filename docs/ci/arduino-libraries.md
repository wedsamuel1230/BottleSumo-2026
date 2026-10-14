# Arduino sensor library matrix

This is a historical candidate matrix for sensor selection. The active
PlatformIO CI no longer installs every candidate. The competition and v2 test
projects declare the Pololu VL53L0X dependency in their own `platformio.ini`
files; the movement sketch uses only `Arduino.h`.

`ci/arduino-libraries.txt` remains for legacy Arduino CLI use. For active builds,
declare dependencies with PlatformIO `lib_deps` and pin registry versions or a
specific VCS commit.

| Sensor | Arduino CLI package | Author | ROI / multizone | Project use |
| --- | --- | --- | --- | --- |
| VL53L5CX | `SparkFun VL53L5CX Arduino Library@1.0.3` | SparkFun | Full 4x4/8x8 | Primary multizone candidate |
| VL53L0X | `Adafruit_VL53L0X@1.2.5` | Adafruit | Single-zone | Primary full ST API wrapper candidate |
| VL53L0X | `VL53L0X@1.3.1` | Pololu | Single-zone | Lightweight yes/no proximity candidate |
| MPU-6050 | `Adafruit MPU6050@2.2.9` | Adafruit | Not applicable | Primary unified IMU candidate |
| VL53L4CD | `STM32duino VL53L4CD@1.0.5` | STMicroelectronics | Single-zone | Single-zone ToF candidate |
| VL53L1X | `Adafruit VL53L1X@3.1.2` | Adafruit | ROI-capable API | Primary ROI candidate |
| VL53L1X | `VL53L1X@1.3.1` | Pololu | No ROI | Smallest/simple API candidate |

The package name is the Arduino Library Manager name, which can differ from a
human product name. `arduino-cli lib install` also installs dependencies declared
by each package. Do not assume that installing alternatives selects one for the
firmware; choose the intended include/API deliberately and document that choice
in the firmware and board references.
