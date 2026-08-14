#!/usr/bin/env python3
"""Exercise the BottleSumo Codex hooks without changing the checkout."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[1]
HOOK_ROOT = REPO_ROOT / ".codex" / "hooks"


def run(command: list[str], cwd: Path) -> None:
    subprocess.run(command, cwd=cwd, check=True, capture_output=True, text=True)


def invoke(script: str, event: dict[str, Any], cwd: Path) -> dict[str, Any] | None:
    result = subprocess.run(
        [sys.executable, str(HOOK_ROOT / script)],
        cwd=REPO_ROOT,
        input=json.dumps(event) + "\n",
        check=True,
        capture_output=True,
        text=True,
    )
    output = result.stdout.strip()
    if not output:
        return None
    value = json.loads(output)
    if not isinstance(value, dict):
        raise AssertionError(f"{script} returned a non-object JSON value")
    return value


def assert_denied(event: dict[str, Any], cwd: Path, phrase: str) -> None:
    output = invoke("pre_tool_use.py", event, cwd)
    if not output or output.get("hookSpecificOutput", {}).get("permissionDecision") != "deny":
        raise AssertionError(f"expected denial containing {phrase!r}: {output}")
    reason = output["hookSpecificOutput"].get("permissionDecisionReason", "")
    if phrase not in reason:
        raise AssertionError(f"denial did not contain {phrase!r}: {reason}")


def make_fixture(root: Path) -> None:
    (root / ".github" / "workflows").mkdir(parents=True)
    (root / "scripts").mkdir()
    (root / "firmware").mkdir()
    (root / "README.md").write_text("# BottleSumo fixture\n", encoding="utf-8")
    (root / ".github" / "workflows" / "arduino-cli-build.yml").write_text(
        "name: Arduino CLI build\n", encoding="utf-8"
    )
    (root / "scripts" / "validate-firmware.sh").write_text(
        "#!/usr/bin/env bash\n", encoding="utf-8"
    )
    run(["git", "init", "-q", "-b", "main"], root)
    run(["git", "config", "user.email", "hooks-test@example.invalid"], root)
    run(["git", "config", "user.name", "BottleSumo hook test"], root)
    run(["git", "add", "."], root)
    run(["git", "commit", "-qm", "fixture"], root)
    (root / "firmware" / "test.ino").write_text("void setup() {}\nvoid loop() {}\n", encoding="utf-8")


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="bottlesumo-hooks-") as directory:
        fixture = Path(directory)
        make_fixture(fixture)
        base = {"cwd": str(fixture), "hook_event_name": "PreToolUse", "tool_name": "Bash"}

        assert_denied(
            {**base, "tool_input": {"command": "git push origin main"}},
            fixture,
            "Direct pushes",
        )
        assert_denied(
            {**base, "tool_input": {"command": "git push origin feature/test"}},
            fixture,
            "codex/*",
        )
        assert_denied(
            {**base, "tool_input": {"command": "git reset --hard HEAD"}},
            fixture,
            "destructive Git",
        )
        allowed = invoke(
            "pre_tool_use.py",
            {**base, "tool_input": {"command": "git push origin codex/test"}},
            fixture,
        )
        if allowed is not None:
            raise AssertionError(f"review-branch push should be allowed: {allowed}")

        session = invoke(
            "session_start.py",
            {"cwd": str(fixture), "hook_event_name": "SessionStart", "source": "startup"},
            fixture,
        )
        if not session or "ESP32-S3-DEVKITC-1-N32R16" not in json.dumps(session):
            raise AssertionError(f"session context missing board contract: {session}")

        prompt = invoke(
            "prompt_context.py",
            {
                "cwd": str(fixture),
                "hook_event_name": "UserPromptSubmit",
                "prompt": "Please change the ESP32 motor firmware",
            },
            fixture,
        )
        if not prompt or "validate-firmware.sh" not in json.dumps(prompt):
            raise AssertionError(f"prompt context missing firmware gate: {prompt}")

        post = invoke(
            "post_tool_use.py",
            {
                "cwd": str(fixture),
                "hook_event_name": "PostToolUse",
                "tool_name": "apply_patch",
                "tool_input": {"command": "Update File: firmware/test.ino"},
            },
            fixture,
        )
        if not post or "git diff --check" not in json.dumps(post):
            raise AssertionError(f"post-tool context missing verification reminder: {post}")

        stop_event = {
            "cwd": str(fixture),
            "hook_event_name": "Stop",
            "stop_hook_active": False,
        }
        stop = invoke("stop_review.py", stop_event, fixture)
        if not stop or stop.get("decision") != "block":
            raise AssertionError(f"firmware stop gate should request a review: {stop}")
        resumed = invoke("stop_review.py", {**stop_event, "stop_hook_active": True}, fixture)
        if resumed is not None:
            raise AssertionError(f"active stop continuation should be allowed: {resumed}")

    print("Codex hook checks passed.")


if __name__ == "__main__":
    main()
