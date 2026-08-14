---
name: bottlesumo-arduino-ci
description: Build, inspect, or update the BottleSumo GitHub Actions Arduino CLI workflow for the ESP32-S3-DEVKITC-1-N32R16, including local compile verification, toolchain pinning, README status reporting, hardware-reference updates, and safe feature-branch delivery. Use when changing `.github/workflows/arduino-cli-build.yml`, validating `firmware/movement_test`, or preparing CI/documentation changes for human merge.
---

# BottleSumo Arduino CI

## Contract

Treat `.github/workflows/arduino-cli-build.yml` as the executable source of truth.
Keep the following contract synchronized with the workflow and the linked project
references:

- Board: `ESP32-S3-DEVKITC-1-N32R16`
- FQBN: `esp32:esp32:esp32s3-octal:FlashSize=32M`
- Arduino CLI: `1.4.1`
- Arduino-ESP32 core: `3.3.8`
- Sketch: `firmware/movement_test`
- Triggers: `push`, `pull_request`, and `workflow_dispatch`

Use the project pinout reference for GPIO assignments and the motor reference for
FG/PWM facts. Do not infer physical behavior from a successful compile.

## Workflow

1. Read `AGENTS.md`, this skill, the workflow, and the relevant files in `docs/`.
2. Preserve unrelated working-tree changes. Update from `origin/main`, then create
   a review branch named `codex/<short-topic>`.
3. Change the smallest required surface. Pin new GitHub Actions and tool versions;
   do not add a floating core or action reference.
4. Run the local compile gate when Arduino CLI and the ESP32 core are available:

   ```bash
   arduino-cli version
   arduino-cli core list
   arduino-cli compile --clean --warnings all \
     --fqbn esp32:esp32:esp32s3-octal:FlashSize=32M \
     firmware/movement_test
   ```

5. Run structural checks:

   ```bash
   git diff --check
   rg -n 'push:|pull_request:|workflow_dispatch:|esp32:esp32@3.3.8|esp32:esp32:esp32s3-octal:FlashSize=32M' \
     .github/workflows/arduino-cli-build.yml
   ```

   Resolve every local Markdown image/link after documentation changes. Confirm
   renamed images in `docs/rules.md` exist and old generic names are absent.

6. Review `git diff --stat`, `git diff --name-only`, and the staged diff. Keep build
   output, caches, secrets, and unrelated user files out of the commit.
7. Push the review branch only. Never push this project change directly to `main`,
   force-push, or merge it. A human must review and merge the branch into `main`.

## Hardware Boundary

The project currently assigns motor FG inputs to GPIO9 and GPIO12 and records 9
square-wave FG pulses per motor revolution. Treat motor wire identity, FG voltage,
pull-up requirements, shaft reference, direction polarity, PWM behavior, and
sensor readings as unverified until measured on the actual hardware.

The current movement sketch has a documented pin mismatch with the v2 schematic.
Do not approve a powered motor test or claim hardware readiness solely because CI
compiles. Update `docs/esp32-s3-devkitc-1-n32r16-pinout.md` when the audited
firmware mapping changes.

## Project References

- Workflow: `.github/workflows/arduino-cli-build.yml`
- Project rules: `AGENTS.md`
- Board and signal map: `docs/esp32-s3-devkitc-1-n32r16-pinout.md`
- Motor and FG reference: `docs/motor-24gp-2430-reference.md`
- Competition rules: `docs/rules.md`
- User-facing CI result: `README.md`
