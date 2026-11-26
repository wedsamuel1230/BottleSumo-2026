# BottleSumo-2026 Project Instructions

These instructions apply to this repository. Keep the global Codex contract in
the workspace-level `AGENTS.md`; keep this file limited to project-specific
constraints and references.

## Required References

- Read `.agents/skills/bottlesumo-arduino-ci/SKILL.md` for firmware or PlatformIO CI work.
- Treat `.github/workflows/platformio-build.yml` as the executable CI contract.
- Treat `ci/arduino-libraries.txt` as a historical candidate sensor-library inventory; active PlatformIO dependencies are pinned in the project configs.
- Start at `docs/README.md` for the documentation map.
- Use `docs/ci/arduino-libraries.md` for the historical candidate sensor-library matrix.
- Use `docs/ci/platformio-firmware.md` for the all-program PlatformIO layout and build commands.
- Use `docs/hardware/esp32-s3-devkitc-1-n16r8-pinout.md` for the active board and GPIO map;
  the N32R16 document is historical.
- Use `arduino/bottlesumo-n16r8/README.md` and `scripts/install-arduino-board.sh` for the
  Arduino IDE board profile: pinned flash/PSRAM/USB settings and the `default_16MB`
  partition table with its two OTA slots.
- Use `docs/competition/esp-idf-wireless.md` for FreeRTOS, OTA and bench-control boundaries.
- Use `docs/hardware/motor-24gp-2430-reference.md` for the motor and FG reference.
- Use `docs/competition/table-sumo-rules-2026.md` for the attached 2026 rules transcription.

## Build Contract

- Active board: `ESP32-S3-DEVKITC-1-N16R8` (16 MB quad flash, 8 MB octal PSRAM).
- Active framework: native ESP-IDF `6.0.1`, PlatformIO Core `6.2.0`, `platformio/espressif32@7.0.1`.
- PlatformIO environments: `robot` and `espnow_receiver`; configuration in `platformio.ini`.
- Shared board profile: `boards/esp32-s3-devkitc1-n16r8.json` (16 MB quad flash, 8 MB octal PSRAM).
- Python tools are locked in `pyproject.toml` and `uv.lock` and run with Python 3.12, including PlatformIO Core `6.2.0`.
- Existing Arduino sketches are separate PlatformIO projects targeting the active N16R8 16 MB flash profile. They use the pioarduino platform release pinned in `platformio-arduino-common.ini` for Arduino-ESP32 **3.3.8**; do not inherit the root IDF platform's Arduino `2.0.17` package.
- CI triggers: `push`, `pull_request`, and `workflow_dispatch`; `.github/workflows/platformio-build.yml` is the executable CI contract.

### Arduino IDE (local build and flash)

The IDE is the bench path for the two Arduino sketches. It is an addition to
this contract, not a replacement.

- Board `BottleSumo 2026 (ESP32-S3 N16R8)`, FQBN `esp32:esp32:bottlesumo_n16r8`,
  installed with `./scripts/install-arduino-board.sh`; restart the IDE after.
  The profile pins 16 MB QIO flash, OPI PSRAM, `default_16MB` (two 6.25 MiB OTA
  slots), USB CDC on boot, 240 MHz and 921600 upload.
- Toolchain versions differ: the IDE has **esp32:esp32 3.3.12** installed
  (verified with `arduino-cli core list`), while PlatformIO builds the same
  sketches with **Arduino-ESP32 3.3.8**. A sketch must build under both; a pass
  in one is not a pass in the other. A Boards Manager core update deletes
  `boards.local.txt`; re-run the install script.
- Libraries: `ArduinoOTA`, `WebServer` and `WiFi` ship with the core; the ToF
  sketches also need Pololu `VL53L0X`.
- First flash of an OTA build goes over USB; later builds use
  `Tools > Port > "bottlesumo-move at <ip> (network)"`, password from the
  gitignored `wifi_local.h` (`./scripts/make-wifi-header.sh`).
- Partition map (`default_16MB`): `nvs` 0x9000, `otadata` 0xe000, app0 0x10000,
  app1 0x650000, `spiffs` 0xc90000, `coredump` 0xFF0000. Never move `otadata`
  or app0; the IDE flashes `boot_app0.bin` and the image at those offsets.

Every firmware-program change, including native ESP-IDF source, a new sketch, refactor, library include,
or build-setting change, must pass the repository validator:

```bash
./scripts/validate-firmware.sh
```

Compilation proves only that the sketch builds for the selected toolchain. It
does not prove motor direction, FG electrical levels, PWM behavior, sensor
readings, or powered robot safety.

`scripts/validate-firmware.sh` discovers every `firmware/**/*.ino` PlatformIO
project, builds it, and then builds both native ESP-IDF environments.
`firmware/archive/**` is retired reference code and is skipped. It invokes
PlatformIO Core 6.2.0 through `uv` with Python 3.12 by default. Each sketch's
`lib_deps` is resolved by PlatformIO; `ci/arduino-libraries.txt` is not part of
the active build contract. `PLATFORMIO_BIN` can explicitly select a local Core
executable instead of `uv`.

Keep `wireless.local.json` ignored; provision credentials into NVS rather than
embedding them in firmware images. OTA writes only while outputs are stopped and
physically armed. Remote driving is bench-only, selected with BUTTON2 at boot;
an autonomous round must reject steering. A missing drive lease stops outputs.

## Bench Access

- Never move the host machine's Wi-Fi to the robot's own access point while the
  host depends on Wi-Fi for its own link. That AP exists for a phone; joining
  it from the build host drops the host's network link and any remote session
  with it. Control and flash over the home LAN.
- Exception, wired fallback only: a temporary switch to the robot AP is allowed
  for one OTA cycle when a wired interface has an address and answers an
  internet request bound to that interface. Do not require it to own the
  default route: macOS keeps the service order, and the wired path still
  carries traffic while Wi-Fi is on the robot AP. Verify both, do the arm and
  upload, then return to the home SSID immediately and verify the robot over
  the LAN. Log every step; never start the switch on an unverified wired link.
- The Arduino firmware runs AP+STA (merged from `movement_test`): it joins the
  home network with the gitignored `wifi_local.h` and keeps `BottleSumo` up.
  Both links share one radio, so the AP follows the station channel; the
  startup `channel` is only a pre-join default and `/api/status` reports the
  live value.
- Reach the robot at `bottlesumo.local` (mDNS) or at the address in
  `/api/status`.
- The first flash of an OTA build goes over USB (`arduino-cli upload -p
  /dev/cu.usbmodem*`). Every later build uses Arduino OTA:
  `arduino-cli upload -p bottlesumo.local -b esp32:esp32:bottlesumo_n16r8
  --input-dir <dir> --upload-field password=<ota password>`.
- Build locally with `arduino-cli compile -b esp32:esp32:bottlesumo_n16r8`;
  PlatformIO stays the CI and pre-commit-validator path.

## Hardware Boundaries

- Motor FG inputs are currently documented as GPIO9 and GPIO12.
- The supplied motor reference records 9 square-wave FG pulses per motor
  revolution.
- Confirm motor wiring, FG voltage, pull-up requirements, shaft reference, and
  direction polarity on the actual hardware before connection or closed-loop
  control.
- Resolve the documented firmware-to-schematic pin mismatch before a powered
  movement test.

## Git Safety

- Start from an up-to-date `origin/main`.
- Create a review branch named `codex/<short-topic>` for agent changes or
  `human/<short-topic>` for human-authored changes.
- Push a `codex/*` or `human/*` review branch only; never push these changes
  directly to `main`.
- Do not force-push or merge. Human review and merge into `main` are required.
- Preserve unrelated working-tree changes and stage explicit paths only.
- Keep build output, caches, secrets, and machine-local files out of commits.
- Install the versioned hooks with `./scripts/install-git-hooks.sh`.
- The pre-commit hook validates staged firmware, manifest, validator, and CI changes.
- The pre-push hook blocks direct pushes to `main` and guides contributors to
  `codex/*` or `human/*` review branches; hooks are guardrails, not a substitute
  for GitHub branch protection.

Loop Engine state and ledgers are machine-local runtime artifacts and are ignored
by `.gitignore`; do not add them to project commits.

## Verification

Before committing, run `./scripts/validate-firmware.sh`, `git diff --check`, and
workflow contract checks. Resolve local Markdown image and link references.
Inspect the staged file list and staged diff, then report any unverified hardware
behavior explicitly.
