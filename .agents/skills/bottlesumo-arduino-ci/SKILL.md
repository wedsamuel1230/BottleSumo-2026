---
name: bottlesumo-arduino-ci
description: Validate every BottleSumo firmware-program change and build, inspect, or update the GitHub Actions Arduino CLI workflow for the ESP32-S3-DEVKITC-1-N32R16, including explicit sensor-library installation, toolchain pinning, README status reporting, hardware-reference updates, hooks, and safe feature-branch delivery. Use when changing any `firmware/**/*.ino`, `ci/arduino-libraries.txt`, `.github/workflows/arduino-cli-build.yml`, or related validation/documentation files.
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
- Firmware programs: every Arduino sketch (`*.ino`) under `firmware/`
- Sensor manifest: `ci/arduino-libraries.txt`
- Triggers: `push`, `pull_request`, and `workflow_dispatch`

Use the project pinout reference for GPIO assignments, the motor reference for
FG/PWM facts, and the sensor library matrix for package choices. Do not infer
physical behavior from a successful compile.

## Workflow

1. Read `AGENTS.md`, this skill, the workflow, and the relevant files in `docs/`.
2. Preserve unrelated working-tree changes. Update from `origin/main`, then create
   a review branch named `codex/<short-topic>`.
3. Change the smallest required surface. Pin new GitHub Actions and tool versions;
   do not add a floating core or action reference.
4. Install candidate libraries when reproducing CI or changing the manifest:

   ```bash
   ./scripts/install-arduino-libraries.sh
   ```

   Arduino CLI can resolve an installed library after seeing an `#include`, but
   it does not auto-download an arbitrary missing library from the registry.
   Keep future sensor choices in `ci/arduino-libraries.txt` instead of hiding
   installation commands in the workflow.

5. Run the local firmware gate when Arduino CLI and the ESP32 core are available:

   ```bash
   arduino-cli version
   arduino-cli core list
   ./scripts/validate-firmware.sh
   ```

   The validator discovers and compiles every `*.ino` under `firmware/`, so a
   new firmware program cannot silently avoid the build gate.

6. Install the versioned hooks for local guardrails:

   ```bash
   ./scripts/install-git-hooks.sh
   ```

   The pre-commit hook runs the validator for staged firmware, manifest, CI, or
   validator changes. The pre-push hook blocks direct pushes to `main`. Hooks
   complement, but do not replace, GitHub branch protection and Actions.

7. Run structural checks:

   ```bash
   git diff --check
   rg -n 'push:|pull_request:|workflow_dispatch:|esp32:esp32@3.3.8|esp32:esp32:esp32s3-octal:FlashSize=32M' \
     .github/workflows/arduino-cli-build.yml
   ```

   Resolve every local Markdown image/link after documentation changes. Confirm
   renamed images in `docs/rules.md` exist and old generic names are absent.

8. Review `git diff --stat`, `git diff --name-only`, and the staged diff. Keep build
   output, caches, secrets, and unrelated user files out of the commit.
9. Push the review branch only. Never push this project change directly to `main`,
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
- Library manifest: `ci/arduino-libraries.txt`
- Firmware validator: `scripts/validate-firmware.sh`
- Hook installer: `scripts/install-git-hooks.sh`
- Sensor library matrix: `docs/arduino-libraries.md`
- Project rules: `AGENTS.md`
- Board and signal map: `docs/esp32-s3-devkitc-1-n32r16-pinout.md`
- Motor and FG reference: `docs/motor-24gp-2430-reference.md`
- Competition rules: `docs/rules.md`
- User-facing CI result: `README.md`
