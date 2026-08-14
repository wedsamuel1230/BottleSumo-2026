#!/usr/bin/env python3
"""Shared, dependency-free helpers for the BottleSumo Codex hooks."""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any, Iterable


def read_event() -> dict[str, Any]:
    try:
        value = json.load(sys.stdin)
    except (json.JSONDecodeError, OSError):
        return {}
    return value if isinstance(value, dict) else {}


def write_json(value: dict[str, Any]) -> None:
    sys.stdout.write(json.dumps(value, separators=(",", ":")) + "\n")


def context_output(event_name: str, context: str) -> dict[str, Any]:
    return {
        "hookSpecificOutput": {
            "hookEventName": event_name,
            "additionalContext": context,
        }
    }


def pre_tool_denial(reason: str) -> dict[str, Any]:
    return {
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "deny",
            "permissionDecisionReason": reason,
        }
    }


def stop_block(reason: str) -> dict[str, Any]:
    return {"decision": "block", "reason": reason}


def find_repo_root(cwd: str | None) -> Path | None:
    candidate = Path(cwd or os.getcwd())
    try:
        result = subprocess.run(
            ["git", "-C", str(candidate), "rev-parse", "--show-toplevel"],
            check=False,
            capture_output=True,
            text=True,
            timeout=5,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if result.returncode != 0:
        return None
    root = result.stdout.strip()
    return Path(root) if root else None


def is_bottlesumo(root: Path | None) -> bool:
    if root is None:
        return False
    return (
        (root / "README.md").is_file()
        and (root / ".github" / "workflows" / "arduino-cli-build.yml").is_file()
        and (root / "scripts" / "validate-firmware.sh").is_file()
    )


def git_output(root: Path, args: Iterable[str]) -> str:
    try:
        result = subprocess.run(
            ["git", "-C", str(root), *args],
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return result.stdout if result.returncode == 0 else ""


def current_branch(root: Path) -> str:
    return git_output(root, ["branch", "--show-current"]).strip() or "(detached HEAD)"


def changed_paths(root: Path) -> list[str]:
    paths: set[str] = set()
    tracked = git_output(root, ["diff", "--name-only", "--no-renames", "HEAD", "--"])
    paths.update(line.strip() for line in tracked.splitlines() if line.strip())
    untracked = git_output(root, ["ls-files", "--others", "--exclude-standard"])
    paths.update(line.strip() for line in untracked.splitlines() if line.strip())
    return sorted(paths)


def is_firmware_path(path: str) -> bool:
    return path.endswith(".ino") or path.startswith("firmware/")


def is_relevant_path(path: str) -> bool:
    return (
        is_firmware_path(path)
        or path.startswith(".codex/")
        or path.startswith(".githooks/")
        or path.startswith(".github/workflows/")
        or path.startswith("ci/")
        or path.startswith("docs/")
        or path in {"AGENTS.md", "README.md", ".gitignore"}
        or path.startswith("scripts/")
    )


def short_paths(paths: Iterable[str], limit: int = 8) -> str:
    selected = list(paths)
    if len(selected) <= limit:
        return ", ".join(selected)
    return ", ".join(selected[:limit]) + f", +{len(selected) - limit} more"
