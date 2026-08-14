#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

current_hooks_path="$(git config --local --get core.hooksPath || true)"
if [ -n "$current_hooks_path" ] && [ "$current_hooks_path" != ".githooks" ]; then
  echo "refusing to replace existing core.hooksPath: $current_hooks_path" >&2
  exit 1
fi

git config --local core.hooksPath .githooks
echo "Configured project hooks at $repo_root/.githooks"
