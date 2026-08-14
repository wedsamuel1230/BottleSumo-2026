#!/usr/bin/env python3

from __future__ import annotations

import re
import shlex
from pathlib import Path

from common import (
    current_branch,
    find_repo_root,
    is_bottlesumo,
    pre_tool_denial,
    read_event,
    write_json,
)


def command_segments(command: str) -> list[str]:
    return [part for part in re.split(r"&&|\|\||[;&|]", command) if part.strip()]


def git_push_arguments(tokens: list[str]) -> list[str] | None:
    for index, token in enumerate(tokens):
        if token != "git":
            continue
        cursor = index + 1
        while cursor < len(tokens) and tokens[cursor] in {"-C", "--git-dir", "--work-tree"}:
            cursor += 2
        if cursor < len(tokens) and tokens[cursor] == "push":
            return tokens[cursor + 1 :]
    return None


def remote_targets(args: list[str], branch: str) -> tuple[list[str], bool]:
    positional: list[str] = []
    all_refs = False
    after_options = False
    for token in args:
        if not after_options and token == "--":
            after_options = True
            continue
        if not after_options and token.startswith("-"):
            if token in {"--all", "--mirror"} or token.startswith("--all="):
                all_refs = True
            continue
        positional.append(token)

    if len(positional) < 2:
        return [branch], all_refs

    targets: list[str] = []
    for refspec in positional[1:]:
        refspec = refspec.lstrip("+")
        target = refspec.rsplit(":", 1)[-1]
        target = target.removeprefix("refs/heads/")
        if target == "HEAD":
            target = branch
        if target:
            targets.append(target)
    return targets or [branch], all_refs


def is_main_target(target: str) -> bool:
    return target == "main" or target.endswith("/main")


def destructive_git_command(command: str) -> bool:
    patterns = (
        r"\bgit\s+reset\s+[^;&|]*--hard\b",
        r"\bgit\s+clean\s+[^;&|]*-[^;&|\s]*f",
        r"\bgit\s+checkout\s+--\s+\.",
        r"\bgit\s+restore\s+\.",
    )
    return any(re.search(pattern, command) for pattern in patterns)


def main() -> None:
    event = read_event()
    if event.get("tool_name") != "Bash":
        return
    tool_input = event.get("tool_input")
    command = tool_input.get("command") if isinstance(tool_input, dict) else None
    if not isinstance(command, str):
        return

    root = find_repo_root(event.get("cwd"))
    if not is_bottlesumo(root):
        return
    branch = current_branch(Path(root))

    if destructive_git_command(command):
        write_json(
            pre_tool_denial(
                "This BottleSumo hook blocks destructive Git commands so unrelated "
                "firmware work is preserved. Inspect the exact paths and use a "
                "recoverable, explicit operation instead."
            )
        )
        return

    for segment in command_segments(command):
        try:
            tokens = shlex.split(segment)
        except ValueError:
            continue
        if re.search(r"\bgh\s+pr\s+merge\b", segment):
            write_json(
                pre_tool_denial(
                    "Human review is required to merge BottleSumo changes into main; "
                    "push the codex/* or human/* review branch instead."
                )
            )
            return

        args = git_push_arguments(tokens)
        if args is None:
            continue
        targets, all_refs = remote_targets(args, branch)
        if all_refs:
            write_json(
                pre_tool_denial(
                    "This hook blocks --all/--mirror pushes because they can include "
                    "BottleSumo main. Push one explicit codex/* or human/* review branch."
                )
            )
            return
        if any(is_main_target(target) for target in targets):
            write_json(
                pre_tool_denial(
                    "Direct pushes to BottleSumo main are blocked. Push a codex/* or "
                    "human/* review branch for human merge."
                )
            )
            return
        invalid = [target for target in targets if not target.startswith(("codex/", "human/"))]
        if invalid:
            write_json(
                pre_tool_denial(
                    "BottleSumo review pushes must target codex/* or human/*; "
                    f"received {', '.join(invalid)}."
                )
            )
            return


if __name__ == "__main__":
    main()
