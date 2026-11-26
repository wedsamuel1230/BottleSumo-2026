# BottleSumo board profile for the Arduino IDE

One board entry replaces the whole `Tools` menu. Selecting it pins the flash
size, PSRAM type, partition table, USB mode, CPU clock and upload speed that
the N16R8 module and the OTA builds need, so a build cannot be produced with
the wrong flash layout.

| Setting | Pinned value |
| --- | --- |
| Board | BottleSumo 2026 (ESP32-S3 N16R8) |
| FQBN | `esp32:esp32:bottlesumo_n16r8` |
| Flash size | 16 MB, QIO 80 MHz, DIO image mode |
| PSRAM | OPI, `-DBOARD_HAS_PSRAM` |
| Partition scheme | `default_16MB` (two OTA slots) |
| USB | Hardware CDC and JTAG, CDC on boot, 921600 baud |
| CPU | 240 MHz, loop and events on core 1 |

## Install

```bash
./scripts/install-arduino-board.sh
```

The script merges the block from
[`boards.local.txt`](boards.local.txt) into
`<Arduino15>/packages/esp32/hardware/esp32/<version>/boards.local.txt` and keeps
anything already there. Restart the Arduino IDE afterwards, then choose
`Tools > Board > BottleSumo 2026 (ESP32-S3 N16R8)`; the other `Tools` entries
disappear because every value is fixed.

Remove it again with:

```bash
./scripts/install-arduino-board.sh --remove
```

`ARDUINO_DIRECTORIES_DATA` overrides the Arduino15 data directory when it is not
in the default macOS, Linux or Windows location.

**A core update reinstalls the ESP32 core and drops `boards.local.txt`.** Re-run
the script after updating the core in Boards Manager. Verified against ESP32
core 3.3.12.

### The board does not appear in the IDE

1. Quit and reopen the Arduino IDE: the board list is read at start-up.
2. Check the install from a terminal:
   `arduino-cli board listall | grep BottleSumo`.
   A hit means the profile is installed and the IDE only needs the restart.
3. A miss means the IDE reads a different data directory. Run
   `arduino-cli config get directories.data`, then
   `ARDUINO_DIRECTORIES_DATA=<that path> ./scripts/install-arduino-board.sh`.
4. After a Boards Manager update, re-run the script: the update removes
   `boards.local.txt`.

## Partition table

`default_16MB.csv` ships with the core and is byte-identical to
[`firmware/bottlesumo_arduino/partitions.csv`](../../firmware/bottlesumo_arduino/partitions.csv)
and to the PlatformIO descriptor
[`boards/esp32-s3-devkitc1-n16r8.json`](../../boards/esp32-s3-devkitc1-n16r8.json):

| Name | Offset | Size | Use |
| --- | --- | ---: | --- |
| `nvs` | `0x9000` | 20 KiB | Preferences, calibration |
| `otadata` | `0xe000` | 8 KiB | which app slot boots |
| `app0` | `0x10000` | 6.25 MiB | OTA slot A |
| `app1` | `0x650000` | 6.25 MiB | OTA slot B |
| `spiffs` | `0xc90000` | 3.43 MiB | unused by the current firmware |
| `coredump` | `0xFF0000` | 64 KiB | crash dump |

`upload.maximum_size` is set to `6553600`, the app slot size, so the IDE rejects
an image that could not be swapped in.

`otadata` must stay at `0xe000` and the first app slot at `0x10000`: the Arduino
builder flashes `boot_app0.bin` and the application image at those fixed
offsets.

## First flash, then OTA

The partition table and `otadata` only reach the module over USB. Flash the
first OTA-capable build with the board cabled, then every later build can go
over Wi-Fi from a sketch that includes `ArduinoOTA`:

1. `Tools > Port > "bottlesumo-move at 192.168.x.x (network)"`.
2. Upload. The IDE asks for the OTA password once.

A sketch that does not include `ArduinoOTA` has no network port to select; flash
it over USB.

## Check the install

```bash
arduino-cli board listall | grep BottleSumo
arduino-cli board details -b esp32:esp32:bottlesumo_n16r8
arduino-cli compile --fqbn esp32:esp32:bottlesumo_n16r8 firmware/movement_test
```

The compile report shows the app against the 6553600-byte slot; a partition
mistake shows up as a maximum size that is not 6553600.
