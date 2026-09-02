# JUFF: an ESP32-S3 × Qwen real-time voice companion

[简体中文](README.md) · [Architecture](docs/architecture.md) · [Firmware](firmware/README.md) · [Contributing](CONTRIBUTING.md)

JUFF turns the Waveshare **ESP32-S3-Touch-LCD-3.5** into a touch-enabled,
real-time voice terminal. Its default transport is encrypted Bluetooth LE:
the ESP32 streams microphone audio to a Mac, `qwen-audio-agent` talks to
DashScope, and the spoken response is streamed back to the onboard speaker.

Wi-Fi/WebSocket remains available as an optional fallback. The DashScope API
key stays on the Mac and is never provisioned to the ESP32.

> JUFF is a hardware prototype with a board-specific firmware target. It is
> not an official Alibaba Cloud, Qwen, or Waveshare project.

## Supported hardware

- Waveshare ESP32-S3-Touch-LCD-3.5
- ESP32-S3 with 16 MB flash and 8 MB octal PSRAM
- ST7796 320×480 display and FT6336 touch controller
- ES8311 codec, onboard microphone, and NS4150B amplifier

The visually similar Touch-AMOLED-1.8 has different hardware and is not
compatible with this firmware.

## Set up a new Mac

You need working Bluetooth, Xcode Command Line Tools, a DashScope API key, and
Node.js `22.22.2+`, `24.15.0+`, or `26+`. The version in [`.nvmrc`](.nvmrc)
is recommended.

```bash
xcode-select --install
git clone https://github.com/daniel-weih/esp32-juff.git juff
cd juff
./scripts/bootstrap_macos.sh
```

The bootstrap script installs the locked Node dependencies, creates a private
`host/.env` with a random token, builds the native BLE companion, and stores
the DashScope key using hidden input. It does not flash the ESP32 or overwrite
existing local configuration.

On JUFF, open the Bluetooth panel and choose **PAIR A NEW MAC**. Enter the
six-digit code shown by the device in the macOS pairing dialog, then run:

```bash
make start
```

Open <http://127.0.0.1:3101/> for the Qwen Audio Agent WebUI. Press `Ctrl-C`
in the terminal to stop all Mac services. Run `make doctor` to diagnose a
new installation.

## Build and flash the firmware

ESP-IDF is only needed for firmware development:

```bash
make firmware-setup
make firmware-build
./scripts/idf.sh -p /dev/cu.usbmodemXXXX flash monitor
```

Replace the serial-device placeholder with the port on your Mac. Back up a
board's factory flash before its first reflash. Device backups are deliberately
excluded from this repository.

## Optional Wi-Fi fallback

BLE-only voice use needs no Wi-Fi. The Bridge binds to `127.0.0.1` by
default. To enable the Wi-Fi transport, bind it to `0.0.0.0`, retain the
random device token created during setup, and run:

```bash
python3 scripts/provision_ble.py
```

See [the firmware documentation](firmware/README.md) and
[the architecture guide](docs/architecture.md) for protocol and hardware
details.

## Development

```bash
./scripts/check_repository.sh
make test
```

Local secrets, ESP-IDF configuration, build output, bonding information, and
flash backups are ignored. Read [SECURITY.md](SECURITY.md) before reporting a
vulnerability and [CONTRIBUTING.md](CONTRIBUTING.md) before opening a pull
request.

JUFF is licensed under [Apache-2.0](LICENSE). Third-party components keep their
own licenses. A separate Alibaba Cloud account is required for DashScope usage.
