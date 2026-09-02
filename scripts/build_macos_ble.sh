#!/bin/sh
set -eu

PROJECT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SOURCE="$PROJECT_DIR/macos/JuffBLE/Sources/main.swift"
INFO_PLIST="$PROJECT_DIR/macos/JuffBLE/Info.plist"
APP="$PROJECT_DIR/macos/build/JuffBLE.app"
CONTENTS="$APP/Contents"
EXECUTABLE="$CONTENTS/MacOS/JuffBLE"
MODULE_CACHE="$PROJECT_DIR/macos/build/swift-module-cache"

mkdir -p "$CONTENTS/MacOS" "$MODULE_CACHE"

CLANG_MODULE_CACHE_PATH="$MODULE_CACHE" \
SWIFT_MODULECACHE_PATH="$MODULE_CACHE" \
XDG_CACHE_HOME="$PROJECT_DIR/macos/build/cache" \
/usr/bin/xcrun swiftc \
  -O \
  -swift-version 5 \
  -module-cache-path "$MODULE_CACHE" \
  -framework Foundation \
  -framework CoreBluetooth \
  "$SOURCE" \
  -o "$EXECUTABLE"

/usr/bin/install -m 644 "$INFO_PLIST" "$CONTENTS/Info.plist"
/usr/bin/codesign --force --sign - "$APP"
/usr/bin/plutil -lint "$CONTENTS/Info.plist"
/usr/bin/codesign --verify --deep --strict "$APP"

echo "$EXECUTABLE"
