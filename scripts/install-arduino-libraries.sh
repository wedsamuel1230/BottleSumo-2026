#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
manifest="$repo_root/ci/arduino-libraries.txt"

command -v arduino-cli >/dev/null 2>&1 || {
  echo "arduino-cli is required to install the candidate libraries" >&2
  exit 1
}

test -f "$manifest" || {
  echo "library manifest not found: $manifest" >&2
  exit 1
}

arduino-cli lib update-index

while IFS= read -r library || [ -n "$library" ]; do
  case "$library" in
    ""|\#*) continue ;;
  esac
  arduino-cli lib install "$library"
done < "$manifest"
