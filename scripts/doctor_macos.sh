#!/bin/sh
set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
failures=0

ok() { printf 'OK    %s\n' "$1"; }
warn() { printf 'WARN  %s\n' "$1"; }
fail() { printf 'FAIL  %s\n' "$1"; failures=$((failures + 1)); }

if [ "$(uname -s)" = "Darwin" ]; then ok "macOS detected"; else fail "macOS is required for BLE"; fi

if command -v node >/dev/null 2>&1 && node -e '
const [major, minor, patch] = process.versions.node.split(".").map(Number)
if (!(major >= 26 || (major === 24 && minor >= 15) || (major === 22 && (minor > 22 || (minor === 22 && patch >= 2))))) process.exit(1)
'; then
  ok "Node.js $(node --version) is supported"
else
  fail "install Node.js 22.22.2+, 24.15.0+, or 26+"
fi

if command -v xcrun >/dev/null 2>&1 && xcrun --find swiftc >/dev/null 2>&1; then
  ok "Swift compiler is available"
else
  fail "run xcode-select --install"
fi

if [ -d "$project_root/host/node_modules/qwen-audio-agent" ]; then
  ok "Node.js dependencies are installed"
else
  fail "run ./scripts/bootstrap_macos.sh"
fi

if [ -x "$project_root/macos/build/JuffBLE.app/Contents/MacOS/JuffBLE" ]; then
  ok "JuffBLE companion is built"
else
  fail "run ./scripts/build_macos_ble.sh"
fi

if [ -f "$project_root/host/.env" ]; then
  token=$(sed -n 's/^JUFF_DEVICE_TOKEN=//p' "$project_root/host/.env" | head -n 1)
  case "$token" in
    ''|change-*|replace-*) fail "host/.env needs a random JUFF_DEVICE_TOKEN" ;;
    *)
      if [ "${#token}" -ge 16 ]; then ok "host configuration uses a private token"; else fail "JUFF_DEVICE_TOKEN is too short"; fi
      ;;
  esac
else
  fail "host/.env is missing"
fi

config_home="${XDG_CONFIG_HOME:-$HOME/.config}"
qwen_config="$config_home/qwaudio/config.env"
if [ -f "$qwen_config" ] && grep -q '^DASHSCOPE_API_KEY=sk-' "$qwen_config"; then
  ok "DashScope key is configured outside the repository"
else
  fail "run python3 scripts/configure_qwen.py"
fi

if ls /dev/cu.usbmodem* >/dev/null 2>&1; then
  ok "an ESP USB serial device is visible"
else
  warn "no ESP USB serial device found (USB is optional for BLE use)"
fi

if [ "$failures" -ne 0 ]; then
  printf '\n%d check(s) need attention.\n' "$failures" >&2
  exit 1
fi

printf '\nAll required Mac components are ready.\n'
