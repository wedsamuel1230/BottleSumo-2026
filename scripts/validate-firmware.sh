#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
firmware_root="$repo_root/firmware"

run_platformio() {
  if [[ -n "${PLATFORMIO_BIN:-}" ]]; then
    "$PLATFORMIO_BIN" "$@"
    return
  fi

  command -v uv >/dev/null 2>&1 || {
    echo "uv is required to run PlatformIO; install uv or set PLATFORMIO_BIN" >&2
    exit 1
  }

  uv run --frozen --python 3.12 platformio "$@"
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

  if [[ ! -f "$sketch_dir/platformio.ini" ]]; then
    echo "PlatformIO project config missing for sketch: $relative_sketch_dir" >&2
    exit 1
  fi

  echo "Compiling PlatformIO Arduino program: $relative_sketch_dir"
  run_platformio run --project-dir "$sketch_dir" -e n16r8
done < <(find "$firmware_root" -type d -name .pio -prune -o -type f -name '*.ino' -print0 | sort -z)

if [[ "$sketch_count" -eq 0 ]]; then
  echo "no Arduino sketches found under $firmware_root" >&2
  exit 1
fi

echo "Validated $sketch_count PlatformIO Arduino project(s)"
echo "Compiling native ESP-IDF robot and ESP-NOW receiver"
run_platformio run --project-dir "$repo_root" -e robot -e espnow_receiver
