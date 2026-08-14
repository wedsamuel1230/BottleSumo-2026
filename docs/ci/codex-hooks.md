# Codex project hooks

This repository includes project-local [Codex lifecycle hooks](https://learn.chatgpt.com/docs/hooks.md)
under `.codex/`. They provide workflow context and safety checks for BottleSumo
sessions. They are separate from the Git hooks in `.githooks/` and from the
GitHub Actions workflow.

## Hook behavior

| Event | BottleSumo behavior |
| --- | --- |
| `SessionStart` | Loads the ESP32-S3 board/FQBN, all-firmware validation rule, branch policy, and current working-tree summary. |
| `UserPromptSubmit` | Adds context when a request mentions firmware, Arduino, hardware, sensors, motors, CI, pins, or hooks. |
| `PreToolUse` for `Bash` | Blocks direct pushes to `main`, `--all`/`--mirror` pushes, review pushes outside `codex/*` or `human/*`, `gh pr merge`, and destructive Git commands that could discard unrelated firmware work. |
| `PostToolUse` for `Bash`/`apply_patch` | Reminds Codex to run the firmware validator, `git diff --check`, and a staged-diff review after relevant changes. |
| `Stop` | Runs a whitespace check and hook-config JSON check, then requests one final verification pass when firmware, documentation, CI, or policy files changed. |

The hooks do not claim that compilation proves motor direction, FG electrical
levels, sensor readings, wiring, or powered-robot safety. They also do not write
Loop Engine state or modify the working tree.

## Files

- `.codex/hooks.json` - the project-local hook configuration.
- `.codex/hooks/common.py` - shared JSON, Git, and path helpers.
- `.codex/hooks/*.py` - event handlers.
- `scripts/test-codex-hooks.py` - isolated behavior checks for the handlers.
- `.githooks/pre-commit` and `.githooks/pre-push` - repository Git guardrails.

## Enable and trust

Project-local hooks are loaded only when the project `.codex/` layer is trusted.
From a Codex session in this repository:

1. Open `/hooks`.
2. Review the exact `.codex/hooks.json` command definitions.
3. Trust the project hooks that should run.

Codex command hooks are reviewed by their current definition hash. Re-review
them after a hook configuration or handler changes. The hooks feature is on by
default; a user or administrator can disable it with `[features].hooks = false`.

Install the separate Git hooks independently when working with Git:

```bash
./scripts/install-git-hooks.sh
```

The Codex hooks are assistant-session guardrails. They do not replace GitHub
branch protection, required Actions checks, or human review and merge into
`main`.

## Local checks

Run the hook tests and configuration checks from the repository root:

```bash
python3 -m json.tool .codex/hooks.json >/dev/null
python3 scripts/test-codex-hooks.py
git diff --check
```

For any firmware change, also install the pinned candidate libraries and build
every sketch:

```bash
./scripts/install-arduino-libraries.sh
./scripts/validate-firmware.sh
```

The GitHub Actions workflow performs the library installation and compilation
on every push, pull request, and manual dispatch.
