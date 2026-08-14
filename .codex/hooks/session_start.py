#!/usr/bin/env python3

from __future__ import annotations

from common import (
    changed_paths,
    context_output,
    current_branch,
    find_repo_root,
    is_bottlesumo,
    read_event,
    short_paths,
    write_json,
)


def main() -> None:
    event = read_event()
    root = find_repo_root(event.get("cwd"))
    if not is_bottlesumo(root):
        return

    paths = changed_paths(root)
    lines = [
        "BottleSumo project context: target ESP32-S3-DEVKITC-1-N32R16 "
        "with FQBN esp32:esp32:esp32s3-octal:FlashSize=32M.",
        "Every Arduino sketch under firmware/ is covered by "
        "./scripts/validate-firmware.sh; CI installs the pinned sensor manifest first.",
        "Compilation is host evidence only; motor direction, FG electrical levels, "
        "sensor readings, wiring, and powered-robot safety still require human testing.",
        "Use codex/* or human/* review branches. Do not push or merge directly to main.",
    ]
    if paths:
        lines.append(f"Current working-tree changes: {short_paths(paths)}.")
    else:
        lines.append("Current working tree has no tracked or untracked project changes.")

    source = event.get("source", "startup")
    lines.insert(0, f"Codex session source: {source}.")
    write_json(context_output("SessionStart", "\n".join(lines)))


if __name__ == "__main__":
    main()
