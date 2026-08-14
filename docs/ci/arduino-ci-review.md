# Arduino CI Review Record

Date: 2026-08-15

## Scope

This review covers the project-specific Arduino CI skill, the all-firmware
validator, the pinned candidate sensor-library install step, the local Git
hooks, and the project instructions used for future BottleSumo firmware work.

The Codex lifecycle hooks are documented separately in
[`codex-hooks.md`](codex-hooks.md) and are included in the verification record
below because they enforce the same branch and firmware-review boundaries.

## Gate results

| Gate | Result | Evidence |
| --- | --- | --- |
| Firmware compilation | Pass | `./scripts/validate-firmware.sh` compiled 1 sketch for `esp32:esp32:esp32s3-octal:FlashSize=32M` with Arduino CLI 1.4.1 and Arduino-ESP32 3.3.8. |
| Library manifest | Pass | `ci/arduino-libraries.txt` contains the seven pinned candidate packages; the workflow installs it before validation. |
| Local hook safety | Pass | `.githooks/pre-commit` invokes the validator for firmware/build inputs; `.githooks/pre-push` rejects `refs/heads/main`. |
| Loop state hygiene | Pass | `.gitignore` ignores `loop-state.json` and `experiment-ledger.jsonl` at any repository depth. |
| Documentation and structure | Pass | `git diff --check`, workflow contract checks, and local Markdown path checks completed before commit. |
| Codex lifecycle hooks | Pass | `python3 scripts/test-codex-hooks.py`, Python compilation, JSON parsing, representative allow/deny events, and local `git diff --check` passed. |

## Residual risk

Compilation does not prove GPIO electrical levels, motor direction, FG pulse
polarity, sensor readings, wiring, or powered-robot safety. Human review and
merge into `main` remain required.
