#!/usr/bin/env python3

from __future__ import annotations

from common import (
    changed_paths,
    context_output,
    find_repo_root,
    is_bottlesumo,
    is_relevant_path,
    read_event,
    write_json,
)


def main() -> None:
    event = read_event()
    tool_name = event.get("tool_name")
    if tool_name not in {"Bash", "apply_patch"}:
        return
    tool_input = event.get("tool_input")
    command = tool_input.get("command", "") if isinstance(tool_input, dict) else ""
    if not isinstance(command, str):
        command = ""

    root = find_repo_root(event.get("cwd"))
    if not is_bottlesumo(root):
        return

    changed = changed_paths(root)
    relevant = [path for path in changed if is_relevant_path(path)]
    lowered = command.lower()
    looks_like_change = tool_name == "apply_patch" or any(
        marker in lowered
        for marker in (
            "validate-firmware.sh",
            "arduino-cli compile",
            "git add",
            "git commit",
            "git mv",
            "git apply",
        )
    )
    if not relevant or not looks_like_change:
        return

    write_json(
        context_output(
            "PostToolUse",
            "BottleSumo files relevant to delivery changed. Before finalizing, run "
            "git diff --check, validate every firmware sketch with "
            "./scripts/validate-firmware.sh, and inspect the staged file list. "
            "Compilation still does not prove physical hardware behavior.",
        )
    )


if __name__ == "__main__":
    main()
