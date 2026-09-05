#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
local_idf="$project_root/.tools/esp-idf"

export IDF_SKIP_CHECK_SUBMODULES=1
export PYTHONDONTWRITEBYTECODE=1
export XDG_CACHE_HOME="${JUFF_XDG_CACHE_HOME:-$project_root/.tools/cache}"

if [[ "${1:-}" != "--help" && "${1:-}" != "-h" && "${1:-}" != "--version" ]]; then
  case "${JUFF_BOARD:-}" in
    waveshare-lcd-1.54|waveshare-lcd-3.5) ;;
    *) echo "Set JUFF_BOARD=waveshare-lcd-1.54 or JUFF_BOARD=waveshare-lcd-3.5 to select the hardware version." >&2; exit 1 ;;
  esac
  # Separate configs prevent a previously built board's saved GPIO values from
  # overriding the selected board's defaults. Keep secrets out of new profiles.
  board_build="$project_root/firmware/build/$JUFF_BOARD"
  board_defaults="$project_root/firmware/boards/$JUFF_BOARD/sdkconfig.defaults"
  expected_board=$(grep '^CONFIG_JUFF_BOARD_.*=y$' "$board_defaults")
  if [[ -f "$board_build/sdkconfig" ]] && ! grep -Fqx "$expected_board" "$board_build/sdkconfig"; then
    echo "Saved config in $board_build does not match $JUFF_BOARD; use that board's own build directory." >&2
    exit 1
  fi
  needs_port=false
  serial_port="${ESPPORT:-}"
  take_port=false
  for argument in "$@"; do
    if [[ "$take_port" == true ]]; then
      serial_port="$argument"
      take_port=false
      continue
    fi
    case "$argument" in
      -p|--port) take_port=true ;;
      --port=*) serial_port="${argument#--port=}" ;;
      -p?*) serial_port="${argument#-p}" ;;
      flash|*-flash|erase-flash|erase_flash|monitor) needs_port=true ;;
      -B*|--build-dir*|-C*|--project-dir*|-DSDKCONFIG*|-D=SDKCONFIG*|--define-cache-entry=SDKCONFIG*|SDKCONFIG[=:]*|SDKCONFIG_DEFAULTS[=:]*)
        echo "Board build/config paths are managed by JUFF_BOARD; do not override them." >&2
        exit 1 ;;
    esac
  done
  if [[ "$take_port" == true || ( "$needs_port" == true && ( -z "$serial_port" || "$serial_port" == -* ) ) ]]; then
    echo "Specify the target device with -p PORT or ESPPORT; serial auto-selection is disabled." >&2
    exit 1
  fi
  set -- -B "$board_build" \
    -D "SDKCONFIG=$board_build/sdkconfig" \
    -D "SDKCONFIG_DEFAULTS=$project_root/firmware/sdkconfig.defaults;$board_defaults" "$@"
fi

if command -v idf.py >/dev/null 2>&1; then
  exec idf.py -C "$project_root/firmware" "$@"
fi

idf_path="${JUFF_IDF_PATH:-${IDF_PATH:-$local_idf}}"
if [[ ! -f "$idf_path/export.sh" ]]; then
  cat >&2 <<EOF
ESP-IDF 5.5.x was not found.

Run:
  ./scripts/setup_esp_idf.sh

Or activate an existing ESP-IDF environment before running this command.
EOF
  exit 1
fi

# ESP-IDF's export script supplies its Python environment and toolchain paths.
# shellcheck disable=SC1090
source "$idf_path/export.sh" >/dev/null
exec idf.py -C "$project_root/firmware" "$@"
