#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
local_idf="$project_root/.tools/esp-idf"

export IDF_SKIP_CHECK_SUBMODULES=1
export PYTHONDONTWRITEBYTECODE=1
export XDG_CACHE_HOME="${JUFF_XDG_CACHE_HOME:-$project_root/.tools/cache}"

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
