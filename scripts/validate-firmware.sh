#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fqbn="esp32:esp32:esp32s3-octal:FlashSize=32M"
firmware_root="$repo_root/firmware"

command -v arduino-cli >/dev/null 2>&1 || {
  echo "arduino-cli is required to validate firmware" >&2
  exit 1
}

test -d "$firmware_root" || {
  echo "firmware directory not found: $firmware_root" >&2
  exit 1
}

sketch_count=0
while IFS= read -r -d '' sketch; do
  sketch_count=$((sketch_count + 1))
  sketch_dir="$(dirname "$sketch")"
  relative_sketch_dir="${sketch_dir#"$repo_root/"}"
  echo "Compiling firmware sketch: $relative_sketch_dir"
  arduino-cli compile --clean --warnings all --fqbn "$fqbn" "$sketch_dir"
done < <(find "$firmware_root" -type f -name '*.ino' -print0)

if [ "$sketch_count" -eq 0 ]; then
  echo "no Arduino sketches found under $firmware_root" >&2
  exit 1
fi

echo "Validated $sketch_count firmware sketch(s) for $fqbn"
