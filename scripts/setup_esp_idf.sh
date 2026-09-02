#!/bin/sh
set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
idf_path="$project_root/.tools/esp-idf"
idf_version="${JUFF_IDF_VERSION:-v5.5.5}"

if [ "$(uname -s)" != "Darwin" ]; then
  echo "This helper currently targets macOS. Follow Espressif's installer on other systems." >&2
  exit 1
fi

if [ ! -f "$idf_path/export.sh" ]; then
  command -v git >/dev/null 2>&1 || { echo "git is required" >&2; exit 1; }
  if [ -e "$idf_path" ]; then
    cat >&2 <<EOF
$idf_path exists but is not a complete ESP-IDF checkout.
Move that local tool-cache directory aside, then run this command again.
EOF
    exit 1
  fi
  mkdir -p "$project_root/.tools"
  echo "Cloning ESP-IDF $idf_version into .tools/esp-idf"
  git clone --branch "$idf_version" --depth 1 --recursive --shallow-submodules \
    https://github.com/espressif/esp-idf.git "$idf_path"
fi

echo "Installing the ESP32-S3 toolchain"
"$idf_path/install.sh" esp32s3

cat <<'EOF'

ESP-IDF is installed for this repository.
Build the firmware with: ./scripts/idf.sh build
EOF
