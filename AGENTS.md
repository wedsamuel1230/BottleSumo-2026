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
- Use `docs/competition/esp-idf-wireless.md` for FreeRTOS, OTA and bench-control boundaries.
- Use `docs/hardware/motor-24gp-2430-reference.md` for the motor and FG reference.
- Use `docs/competition/table-sumo-rules-2026.md` for the attached 2026 rules transcription.

## Build Contract

- Active board: `ESP32-S3-DEVKITC-1-N16R8` (16 MB quad flash, 8 MB octal PSRAM).
- Active framework: native ESP-IDF `6.0.1`, PlatformIO Core `6.2.0`, `platformio/espressif32@7.0.1`.
- PlatformIO environments: `robot` and `espnow_receiver`; configuration in `platformio.ini`.
- Shared board profile: `boards/esp32-s3-devkitc1-n16r8.json` (16 MB quad flash, 8 MB octal PSRAM).
- Python tools are locked in `pyproject.toml` and `uv.lock` and run with Python 3.12, including PlatformIO Core `6.2.0`.
- Existing Arduino sketches are separate PlatformIO projects targeting the active N16R8 16 MB flash profile. They use the pinned pioarduino platform release for Arduino-ESP32 `3.3.8`; do not inherit the root IDF platform's Arduino `2.0.17` package.
- The former Arduino CLI `1.4.1` / FQBN `esp32:esp32:esp32s3-octal:FlashSize=32M` build is historical and is not the active board profile.
- Shared Arduino project settings are in `platformio-arduino-common.ini`; each sketch folder has its own `platformio.ini`.
- CI triggers: `push`, `pull_request`, and `workflow_dispatch`.

Every firmware-program change, including native ESP-IDF source, a new sketch, refactor, library include,
or build-setting change, must pass the repository validator:

```bash
./scripts/validate-firmware.sh
```

Compilation proves only that the sketch builds for the selected toolchain. It
does not prove motor direction, FG electrical levels, PWM behavior, sensor
readings, or powered robot safety.

`scripts/validate-firmware.sh` discovers every `firmware/**/*.ino` PlatformIO
project, builds it, and then builds both native ESP-IDF environments. It invokes
PlatformIO Core 6.2.0 through `uv` with Python 3.12 by default. Each sketch's
`lib_deps` is resolved by PlatformIO; `ci/arduino-libraries.txt` is not part of
the active build contract. `PLATFORMIO_BIN` can explicitly select a local Core
executable instead of `uv`.

Keep `wireless.local.json` ignored; provision credentials into NVS rather than embedding them in firmware images. OTA writes only while outputs are stopped and
physically armed. Remote driving is bench-only, selected with BUTTON2 at boot;
an autonomous round must reject steering. A missing drive lease stops outputs.

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
