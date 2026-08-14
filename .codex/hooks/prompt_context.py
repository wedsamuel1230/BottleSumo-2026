#!/usr/bin/env python3

from __future__ import annotations

from common import context_output, find_repo_root, is_bottlesumo, read_event, write_json


PROJECT_TERMS = (
    "arduino",
    "firmware",
    "esp32",
    "gpio",
    "motor",
    "sensor",
    "tof",
    "qre",
    "pwm",
    "fg",
    "workflow",
    "github action",
    "hook",
    "pin",
    ".ino",
)


def main() -> None:
    event = read_event()
    root = find_repo_root(event.get("cwd"))
    prompt = event.get("prompt", "")
    if not is_bottlesumo(root) or not isinstance(prompt, str):
        return
    lowered = prompt.lower()
    if not any(term in lowered for term in PROJECT_TERMS):
        return

    lines = [
        "BottleSumo request reminder:",
        "- Validate every firmware-program change, including new .ino files, with "
        "./scripts/validate-firmware.sh.",
        "- Keep library choices in ci/arduino-libraries.txt and install them before "
        "local reproduction or CI compilation.",
        "- Treat GPIO9/GPIO12 motor FG and the documented 9 pulses per revolution as "
        "hardware references pending bench measurement.",
        "- Do not report a successful compile as proof of physical robot behavior.",
    ]
    if "push" in lowered or "commit" in lowered or "branch" in lowered:
        lines.append("- Deliver agent work on codex/* or human/* for human review and merge.")
    write_json(context_output("UserPromptSubmit", "\n".join(lines)))


if __name__ == "__main__":
    main()
