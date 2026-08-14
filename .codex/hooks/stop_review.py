#!/usr/bin/env python3

from __future__ import annotations

import json
import subprocess

from common import (
    changed_paths,
    find_repo_root,
    is_bottlesumo,
    is_firmware_path,
    is_relevant_path,
    read_event,
    short_paths,
    stop_block,
    write_json,
)


def diff_check(root) -> str:
    try:
        result = subprocess.run(
            ["git", "-C", str(root), "diff", "--check", "HEAD", "--"],
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return result.stdout.strip() if result.returncode != 0 else ""


def json_config_is_valid(root) -> bool:
    config = root / ".codex" / "hooks.json"
    if not config.exists():
        return True
    try:
        with config.open(encoding="utf-8") as handle:
            json.load(handle)
    except (OSError, json.JSONDecodeError):
        return False
    return True


def main() -> None:
    event = read_event()
    root = find_repo_root(event.get("cwd"))
    if not is_bottlesumo(root):
        return

    paths = changed_paths(root)
    relevant = [path for path in paths if is_relevant_path(path)]
    if not relevant:
        return

    whitespace_error = diff_check(root)
    if whitespace_error:
        write_json(
            stop_block(
                "BottleSumo completion is blocked by git diff --check:\n"
                + whitespace_error
            )
        )
        return
    if ".codex/hooks.json" in paths and not json_config_is_valid(root):
        write_json(stop_block("BottleSumo .codex/hooks.json is not valid JSON."))
        return

    if event.get("stop_hook_active") is True:
        return

    firmware_changed = any(is_firmware_path(path) for path in paths)
    checks = ["run git diff --check", "review the staged file list and staged diff"]
    if firmware_changed:
        checks.insert(
            0,
            "run ./scripts/install-arduino-libraries.sh && "
            "./scripts/validate-firmware.sh for every sketch under firmware/",
        )
    if any(path.startswith("docs/") for path in paths):
        checks.append("verify local Markdown links and image paths")
    if any(path.startswith((".codex/", ".githooks/", ".github/", "ci/")) for path in paths):
        checks.append("review hook/workflow/library configuration syntax")

    write_json(
        stop_block(
            "Before stopping, complete the BottleSumo verification pass:\n- "
            + "\n- ".join(checks)
            + "\nCompilation is host evidence only; report any unverified hardware behavior. "
            f"Relevant changes: {short_paths(relevant)}."
        )
    )


if __name__ == "__main__":
    main()
