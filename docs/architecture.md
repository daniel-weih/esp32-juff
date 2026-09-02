# JUFF architecture

JUFF is split into an ESP32 firmware, a native macOS Bluetooth companion, and
a Node.js host supervisor. The cloud credential and model session exist only
on the Mac.

## Runtime components

```text
┌──────────────────── ESP32-S3 ────────────────────┐
│ LCD/touch  microphone  ES8311  speaker  GPIO 0   │
│          firmware 0.5.x: UI + audio + BLE        │
└───────────────────────┬───────────────────────────┘
                        │ encrypted BLE
                        │ control + G.711 μ-law
┌───────────────────────▼──────── macOS ────────────┐
│ JuffBLE (Swift/CoreBluetooth)                     │
│       │ stdin/stdout                              │
│ run.mjs supervisor ─ GatewayClient                │
│       ├─ qwen-audio-agent + WebUI :3101           │
│       └─ optional Wi-Fi Device Bridge :8765       │
└────────────────────────┬──────────────────────────┘
                         │ TLS
                         ▼
                      DashScope
```

`npm start` runs `host/src/run.mjs`. The supervisor starts or reuses the
Qwen Gateway, starts JuffBLE, creates the optional Device Bridge, and shuts all
children down when it receives SIGINT or SIGTERM.

## BLE transport

The ESP32 is a NimBLE peripheral named `JUFF-xxxx`; the suffix comes from its
Bluetooth address. The Mac is the CoreBluetooth central.

- Bluetooth LE Secure Connections, MITM pairing, and persistent bonding
- Up to two remembered Mac hosts; one active connection
- A 120-second explicit new-host pairing window
- One service with control, status, microphone, and speaker characteristics
- Sensitive writes require an encrypted, authenticated connection

The custom service uses these UUIDs:

| Purpose | UUID suffix |
| --- | --- |
| Service | `B8A5FCA1-8A0F-4DB1-9C6C-7B5B6C49A001` |
| Control | `...A002` |
| Status | `...A003` |
| Microphone | `...A004` |
| Speaker | `...A005` |

Control messages are newline-delimited JSON with a maximum encoded size of
512 bytes.

## Audio formats and flow control

The ES8311 side uses 24 kHz, signed PCM16 little-endian, mono.

| Direction | Gateway/device format | BLE wire format | Rate |
| --- | --- | --- | --- |
| Microphone | PCM16 mono | 8-bit G.711 μ-law | 16 kHz |
| Speaker | PCM16 mono | 8-bit G.711 μ-law | 24 kHz |

Microphone capture is linearly resampled from 24 kHz to 16 kHz and sent in
100 ms frames. Downstream Qwen PCM is μ-law encoded by JuffBLE. The Mac starts
with a 300 ms speaker buffer, then uses a roughly 25.2 kB/s token bucket so
CoreBluetooth does not burst data faster than the ESP32 playback queue can
consume it. The device has roughly eight seconds of downstream buffering.

Each response has `audio.begin`, streamed audio, and `audio.done` lifecycle
events. `playback.clear` immediately discards queued data and turns off the
amplifier. Current operation is half-duplex to prevent acoustic feedback;
natural full-duplex barge-in would require acoustic echo cancellation.

## Optional Wi-Fi transport

The ESP32 can alternatively connect to the Device Bridge over an authenticated
WebSocket. This path transports PCM16 directly:

- 16 kHz PCM16 microphone frames upstream
- 24 kHz PCM16 speaker frames downstream
- a random shared device token in the initial `hello` message
- `audio.begin`, `audio.done`, playback, state, and interruption events

The Bridge is disabled for LAN access by default because it binds to loopback.
Changing `JUFF_BRIDGE_HOST` to `0.0.0.0` is an explicit opt-in.

## Security boundaries

- DashScope credentials never cross the Mac/ESP32 boundary.
- The Gateway and WebUI listen on loopback.
- BLE control and provisioning require authenticated encryption.
- Device tokens, Wi-Fi credentials, and bonding keys are machine-local.
- Flash backups and generated firmware configuration are excluded from Git.

This is a prototype security model, not a formal security audit. Report issues
using [SECURITY.md](../SECURITY.md).
