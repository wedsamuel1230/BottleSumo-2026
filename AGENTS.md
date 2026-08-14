# BottleSumo-2026 Project Instructions

These instructions apply to this repository. Keep the global Codex contract in
the workspace-level `AGENTS.md`; keep this file limited to project-specific
constraints and references.

## Required References

- Read `.agents/skills/bottlesumo-arduino-ci/SKILL.md` for CI or Arduino CLI work.
- Treat `.github/workflows/arduino-cli-build.yml` as the executable CI contract.
- Treat `ci/arduino-libraries.txt` as the versioned candidate sensor-library manifest.
- Use `docs/arduino-libraries.md` for the sensor-library choice and registry-name map.
- Use `docs/esp32-s3-devkitc-1-n32r16-pinout.md` for the board and GPIO map.
- Use `docs/motor-24gp-2430-reference.md` for the motor and FG reference.

## Build Contract

- Target board: `ESP32-S3-DEVKITC-1-N32R16`.
- FQBN: `esp32:esp32:esp32s3-octal:FlashSize=32M`.
- Arduino CLI: `1.4.1`.
- Arduino-ESP32 core: `3.3.8`.
- Firmware programs: every Arduino sketch (`*.ino`) under `firmware/`.
- CI triggers: `push`, `pull_request`, and `workflow_dispatch`.

Every firmware-program change, including a new sketch, refactor, library include,
or build-setting change, must pass the repository validator:

```bash
./scripts/validate-firmware.sh
```

Compilation proves only that the sketch builds for the selected toolchain. It
does not prove motor direction, FG electrical levels, PWM behavior, sensor
readings, or powered robot safety.

CI runs `scripts/install-arduino-libraries.sh` first. It installs every pinned
entry in `ci/arduino-libraries.txt` and lets each package install its declared
dependencies. Arduino CLI does not auto-download missing libraries merely
because a future sketch includes a header.

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
- Create a feature branch named `codex/<short-topic>` for Codex changes.
- Push the feature branch only; never push these changes directly to `main`.
- Do not force-push or merge. Human review and merge into `main` are required.
- Preserve unrelated working-tree changes and stage explicit paths only.
- Keep build output, caches, secrets, and machine-local files out of commits.
- Install the versioned hooks with `./scripts/install-git-hooks.sh`.
- The pre-commit hook validates staged firmware, manifest, validator, and CI changes.
- The pre-push hook blocks direct pushes to `main`; hooks are guardrails, not a
  substitute for GitHub branch protection.

Loop Engine state and ledgers are machine-local runtime artifacts and are ignored
by `.gitignore`; do not add them to project commits.

## Verification

Before committing, run `./scripts/validate-firmware.sh`, `git diff --check`, and
workflow contract checks. Resolve local Markdown image and link references.
Inspect the staged file list and staged diff, then report any unverified hardware
behavior explicitly.
