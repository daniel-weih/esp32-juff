# JUFF architecture

JUFF is split into an ESP32 firmware, a native macOS Bluetooth companion, and
a Node.js host supervisor. The cloud credential and model session exist only
on the Mac.

## Runtime components

```text
┌──────────────────── ESP32-S3 ────────────────────┐
│ LCD/touch  microphone  codec  speaker  GPIO 0    │
│              UI + audio + BLE                   │
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

The ESP32 is a NimBLE peripheral named `JUFF-XXXX`; the suffix comes from its
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
amplifier. Both transports pause microphone upload during playback. Normal
builds for both hardware profiles use half-duplex audio and manual interruption.
Touch and BOOT/GPIO 0 stop controls route interruption to the active transport.

### Experimental voice interruption

Repository defaults disable `CONFIG_JUFF_VOICE_BARGE_IN` for both boards. The
two profiles support explicit opt-in through `menuconfig`; package
manifests record the actual setting in `features.voice_interrupt` and the
reference type in `features.aec_reference`. See the
[firmware guide](../firmware/README.md) for build instructions.

On the 1.54-inch board, the frontend treats ES7210 TDM slot 0 as the microphone and
slot 1 as the reference, corresponding to physical MIC3. The DAC output is
sampled before the power amplifier through a resistor network with about
24 dB attenuation. `CONFIG_JUFF_CODEC_REFERENCE_GAIN_DB` sets MIC3 gain
independently: 24 dB by default, or 21 dB above compile-time speaker volume 85.
The reference stays on the device. Electrical tests confirmed the reference
path but also found waveform distortion at maximum volume. The absence of
ADC clipping does not establish a clean signal.

On the 3.5-inch board, ES8311 digital feedback returns the microphone in the
left receive slot and the right transmit slot's PCM in the right receive slot.
The single input/output codec opens both directions as two-channel PCM16 at
24 kHz. Playback duplicates each mono sample into both slots, including
diagnostic tones. Reference samples are before DAC volume/mute and do not
measure analog distortion. No ES7210 channel gain is applied to this board.
See the [large-board record](waveshare-lcd-3.5-aec-feasibility.md).

Each 20 ms block of 24 kHz interleaved input is resampled to 16 kHz using the
same sample positions for both channels. ESP-SR 2.5.3 runs
`AEC_MODE_FD_HIGH_PERF`, followed by WebRTC VAD. The small board retains
`AEC_NLP_LEVEL_NORMAL`; the large board uses `AEC_NLP_LEVEL_VERYAGGR` for residual
echo from its digital reference path. Its AEC profile defaults to 24 dB
microphone digital scaling to reduce full-scale PCM output during playback.
This driver setting does not change the ES8311 analog PGA. Saved configurations
retain their previous values.
Detection waits for actual PCM playback and active reference energy, then
400 ms of warmup. It requires 120 ms of continuous speech above
`max(180 RMS, 3 × estimated noise RMS)`.

On detection, the device stops playback and sends `interrupt` before resuming
microphone upload, including 300 ms of speech history and frames captured
before upload resumes. BLE transmission remains half-duplex; the cloud gets
no microphone stream during playback. `input.suspend` disables detection and
clears buffered speech.

Both profiles use a 64 KB data cache with 64-byte lines and external
NimBLE allocations. The frontend's 4800-sample history and 8000-sample pending
buffer occupy 25,600 bytes in PSRAM. DMA buffers, AEC work frames and task
stacks remain internal. Failed frontend initialization releases partial
allocations and falls back to ordinary half-duplex audio.

Historical `0.6.0-dev` tests found self-interruptions and missed near-end speech;
one later calibrated acoustic smoke test passed. Broader acceptance remains
incomplete. The [validation record](voice-interruption-validation.md) separates
software checks, electrical measurements, offline DSP results and acoustic
limits. Startup audio diagnostics are disabled by default; `audio_test` is an
explicit command.

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
