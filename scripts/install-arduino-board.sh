#!/usr/bin/env bash
# Install (or remove) the BottleSumo board profile in the installed ESP32
# Arduino core, so the Arduino IDE offers
#   Tools > Board > BottleSumo 2026 (ESP32-S3 N16R8)
# with every flash, PSRAM, partition, USB and upload setting already pinned.
#
# Usage:
#   ./scripts/install-arduino-board.sh            # install / refresh
#   ./scripts/install-arduino-board.sh --remove   # take it back out
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fragment="${repo_root}/arduino/bottlesumo-n16r8/boards.local.txt"
begin_marker="# >>> bottlesumo-n16r8 >>>"
end_marker="# <<< bottlesumo-n16r8 <<<"
board_id="bottlesumo_n16r8"

usage() {
  cat <<'EOF'
Install the BottleSumo board profile into the ESP32 Arduino core.

  --remove   strip the profile again (the rest of boards.local.txt is kept)
  -h, --help this text

Set ARDUINO_DIRECTORIES_DATA to override the Arduino15 data directory.
EOF
}

mode=install
for arg in "$@"; do
  case "$arg" in
    --remove) mode=remove ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $arg" >&2; usage >&2; exit 2 ;;
  esac
done

die() { echo "error: $*" >&2; exit 1; }

[[ -f "$fragment" ]] || die "missing board fragment: $fragment"

data_dir="${ARDUINO_DIRECTORIES_DATA:-}"
if [[ -z "$data_dir" ]]; then
  for candidate in "$HOME/Library/Arduino15" "$HOME/.arduino15" "$HOME/AppData/Local/Arduino15"; do
    if [[ -d "$candidate" ]]; then data_dir="$candidate"; break; fi
  done
fi
[[ -n "$data_dir" ]] || die "no Arduino15 data directory found; set ARDUINO_DIRECTORIES_DATA"

core_root="${data_dir}/packages/esp32/hardware/esp32"
[[ -d "$core_root" ]] || die "the ESP32 Arduino core is not installed (looked in $core_root)"

# Newest installed core version that actually has a boards.txt.
core_dir=""
while IFS= read -r candidate; do
  [[ -f "${candidate}/boards.txt" ]] && core_dir="$candidate"
done < <(find "$core_root" -mindepth 1 -maxdepth 1 -type d | sort -V)
[[ -n "$core_dir" ]] || die "no core version with a boards.txt under $core_root"

target="${core_dir}/boards.local.txt"

strip_block() {
  awk -v b="$begin_marker" -v e="$end_marker" '
    $0 == b { skipping = 1; next }
    $0 == e { skipping = 0; next }
    !skipping { print }
  ' "$1"
}

tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT

if [[ "$mode" == remove ]]; then
  [[ -f "$target" ]] || { echo "nothing to remove: $target does not exist"; exit 0; }
  strip_block "$target" > "$tmp"
  if grep -qF "$board_id" "$tmp"; then
    die "$board_id is still in $target outside the managed block; fix it by hand"
  fi
  cp "$tmp" "$target"
  echo "removed the BottleSumo profile from $target"
  exit 0
fi

# The profile names the core's own partition table; without it the app cannot
# be built for the OTA slots.
partitions="${core_dir}/tools/partitions/default_16MB.csv"
[[ -f "$partitions" ]] || die "missing $partitions; the core version is not usable"

if [[ -f "$target" ]]; then
  strip_block "$target" > "$tmp"
else
  : > "$tmp"
fi
# Separate the managed block from hand-written content.
[[ -s "$tmp" ]] && printf '\n' >> "$tmp"
awk -v b="$begin_marker" -v e="$end_marker" '
  $0 == b { keep = 1 }
  keep { print }
  $0 == e { keep = 0 }
' "$fragment" >> "$tmp"
cp "$tmp" "$target"

if ! grep -qF "${board_id}.name=" "$target"; then
  die "the profile block did not land in $target"
fi

# A stray "; comment" line makes arduino-cli reject the whole file and the board
# silently disappears, so verify the platform still parses.
if command -v arduino-cli >/dev/null 2>&1; then
  if ! arduino-cli board listall 2>/dev/null | grep -qF "esp32:esp32:${board_id}"; then
    die "the ESP32 core rejects boards.local.txt (no ${board_id} in the board list); check the file for lines that are not 'key=value' or '#'"
  fi
  echo "verified: arduino-cli lists esp32:esp32:${board_id}"
fi

echo "installed the BottleSumo profile in ${target}"
echo "  board:  BottleSumo 2026 (ESP32-S3 N16R8)"
echo "  FQBN:   esp32:esp32:${board_id}"
echo "  flash:  16MB, QIO 80MHz, OPI PSRAM, USB CDC on boot, 921600 baud"
echo "  slots:  default_16MB — app0/app1 6.25 MiB each, otadata at 0xe000"
echo
echo "Restart the Arduino IDE, then select"
echo "  Tools > Board > BottleSumo 2026 (ESP32-S3 N16R8)"
