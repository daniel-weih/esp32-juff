#!/bin/sh
set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
skip_key=false

usage() {
  cat <<'EOF'
Usage: ./scripts/bootstrap_macos.sh [--skip-key]

Installs locked Node dependencies, creates a private host configuration,
builds the native BLE companion, and configures DashScope interactively.
EOF
}

for argument in "$@"; do
  case "$argument" in
    --skip-key) skip_key=true ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $argument" >&2; usage >&2; exit 2 ;;
  esac
done

if [ "$(uname -s)" != "Darwin" ]; then
  echo "JUFF's BLE companion currently requires macOS." >&2
  exit 1
fi

for command_name in node npm python3 xcrun openssl; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    echo "Missing required command: $command_name" >&2
    echo "Install Xcode Command Line Tools and Node.js 24.15+ before retrying." >&2
    exit 1
  fi
done

if ! node -e '
const [major, minor, patch] = process.versions.node.split(".").map(Number)
const supported = major >= 26
  || (major === 24 && minor >= 15)
  || (major === 22 && (minor > 22 || (minor === 22 && patch >= 2)))
if (!supported) process.exit(1)
'; then
  echo "Unsupported Node.js $(node --version). Use 22.22.2+, 24.15.0+, or 26+." >&2
  exit 1
fi

if ! xcrun --find swiftc >/dev/null 2>&1; then
  echo "Swift compiler not found. Run: xcode-select --install" >&2
  exit 1
fi

echo "[1/4] Installing locked Node.js dependencies"
(cd "$project_root/host" && npm ci --ignore-scripts)

if [ ! -f "$project_root/host/.env" ]; then
  echo "[2/4] Creating private host/.env"
  token=$(openssl rand -hex 24)
  umask 077
  sed "s/^JUFF_DEVICE_TOKEN=.*/JUFF_DEVICE_TOKEN=$token/" \
    "$project_root/host/.env.example" > "$project_root/host/.env.tmp"
  mv "$project_root/host/.env.tmp" "$project_root/host/.env"
else
  echo "[2/4] Keeping existing private host/.env"
fi

echo "[3/4] Building the native Bluetooth companion"
"$project_root/scripts/build_macos_ble.sh" >/dev/null

if [ "$skip_key" = true ]; then
  echo "[4/4] Skipping DashScope key setup"
else
  config_home="${XDG_CONFIG_HOME:-$HOME/.config}"
  qwen_config="$config_home/qwaudio/config.env"
  if [ -f "$qwen_config" ] && grep -q '^DASHSCOPE_API_KEY=sk-' "$qwen_config"; then
    echo "[4/4] Keeping the existing DashScope configuration"
  else
    echo "[4/4] Configuring DashScope (input is hidden)"
    python3 "$project_root/scripts/configure_qwen.py"
  fi
fi

cat <<'EOF'

JUFF is ready on this Mac.

1. On the ESP32, open the Bluetooth panel and choose PAIR A NEW MAC.
2. Start JUFF with: make start
3. Open the dashboard at http://127.0.0.1:3101/
4. Stop all Mac services with Ctrl-C in the same terminal.
EOF
