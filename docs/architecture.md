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

`make admin` instead runs `host/src/admin.mjs` and an independent loopback-only
dashboard on port 3210. Its supervisor starts the shared Qwen gateway and
per-device `run.mjs` workers with `JUFF_SERVICE_MODE=ble` or `bridge`. These
workers require a running gateway and do not start their own. The legacy
default mode, `all`, keeps the original direct startup behavior.

Each device profile has an explicit hardware model, Bluetooth name, Bridge port
and session ID. The manager stores these in ignored `host/.local/devices.json`
with private permissions. It owns only child processes it spawned, serializes
operations, and stops dependent workers before the shared gateway. External
gateway and Bridge health is visible without acquiring process ownership.
See [service management](service-manager.md) for the HTTP API and boundaries.

## BLE transport

The ESP32 is a NimBLE peripheral named `JUFF-XXXX`; the suffix comes from its
Bluetooth address. The Mac is the CoreBluetooth central.

- Bluetooth LE Secure Connections, MITM pairing, and persistent bonding
- Up to two remembered Mac hosts; one active connection
- A 120-second explicit new-host pairing window
- One service with control, status, microphone, and speaker characteristics
- Sensitive writes require an encrypted, authenticated connection

The Mac first writes a `status` request to the protected control characteristic
and waits for its successful GATT completion before enabling status/microphone
notifications. This keeps the firmware's pairing-code notice visible until
authentication completes: enabling audio early triggers a conversation boundary
that clears transient display notices. The companion reports `authenticating`,
then `authenticated`, then `audio-connected` after subscriptions complete.

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

## Conversation lifetime and shared voice ownership

Firmware publishes `conversation.state` with its LIVE / WAKE mode, active flag
and generation; the host replies with `conversation.ack`. Each transition
retires the previous cloud client and tags control messages with the current
generation. WAKE standby keeps microphone audio local. Playback and detected
speech renew a 180-second idle deadline; expiry closes the cloud conversation
and requires another local wake. Mac-generated playback failure clears carry
the same generation as forwarded cloud controls.

Qwen `1.11.0` arbitrates one active voice client per owner, independently of
the `sessionId`. A busy or deactivated client can retain its WebSocket while
its audio frontend is disabled. On `voice.ownership: available`, a waiting
JUFF client sends one new `connect` request on that socket with `takeover: false`.
Duplicate notices cannot submit concurrent claims. Competing clients
therefore resume after release without taking the slot from another holder.
Initial connection policy remains separate from this recovery request.

Explicit mute/sleep disables automatic recovery, input mute survives a claim,
and closing a conversation cancels all future claims from its old socket.
The device receives `voice.ready` only when the audio frontend becomes ready;
opening a local WebSocket alone does not establish speech readiness. A cached
wake acknowledgement plays at most once per wake, including ownership recovery.

## Audio formats and flow control

The ES8311 side uses 24 kHz, signed PCM16 little-endian, mono.

| Direction | Gateway/device format | BLE wire format | Rate |
| --- | --- | --- | --- |
| Microphone | PCM16 mono | 8-bit G.711 μ-law | 16 kHz |
| Speaker | PCM16 mono | 8-bit G.711 μ-law | 24 kHz |

Microphone capture is linearly resampled from 24 kHz to 16 kHz and sent in
100 ms frames. Downstream Qwen PCM is μ-law encoded by JuffBLE. Both hardware
profiles reserve up to 192,000 bytes of decoded playback PCM in PSRAM, about
four seconds at 24 kHz. The device prefills 300 ms before playback and refills
250 ms after an underrun, with a 600 ms wait ceiling. An ended short response
can start before reaching the prefill threshold.

Firmware advertises `speakerFlow: 1` in `status`. For these devices, JuffBLE
adds `flowControl: 1` and a nonzero `streamId` to `audio.begin`, then waits for
`speaker.credit` before sending audio. Every speaker packet starts with two
little-endian 32-bit words: the stream ID and the μ-law byte offset, followed
by the encoded audio. `audio.done` carries the same stream ID.

For this credit-based transport, JuffBLE caps each GATT write value at 244
bytes (including its 8-byte stream header), while respecting a smaller
CoreBluetooth limit. JUFF requests a 251-byte link payload; the 4-byte L2CAP
header and 3-byte ATT write header leave 244 bytes for the value. A 253-byte
value permitted by an ATT MTU of 256 otherwise spans two link packets.
This avoids the short second fragment without changing the codec, sample
rate, receive window or send-rate policy. See the
[ESP-IDF packet layout](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/ble/get-started/ble-connection.html#maximum-transmission-unit).

Credits carry `responseId`, `streamId`, `received`, `limit`, and `state`.
`received` is the accepted μ-law byte count; `limit` is the cumulative upper
bound the Mac may send, excluding packet headers. The device accounts for
queued and currently written PCM plus BLE staging when publishing space.
Snapshot creation and notification publication are serialized to prevent
concurrent updates from arriving out of order. The Mac also respects
CoreBluetooth write readiness and queues excess synthesis within a 2 MiB
encoded-audio limit. Responses stay ordered through playback completion.
Older firmware retains headerless μ-law and the 25,200 B/s token bucket with
a 7,200-byte burst allowance.

Microphone notifications remain headerless μ-law. Starting with `0.6.10-dev`,
the microphone sender queues a complete `microphone.reset` status notification
before a new stream generation or after abandoning a partially sent frame.
The updated Mac helper clears its incomplete 1600-byte frame on this boundary;
firmware sends no new PCM until the boundary is queued successfully. Rejected
fragments retry with at least one scheduler tick, bounded by 200 ms per fragment
and 400 ms per frame. Both firmware and helper must be updated for boundary
recovery; the new helper still accepts older firmware's continuous byte stream.

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
The small board additionally checks a roughly 128 ms window of complete AEC
blocks. While reference RMS is at least 80, processed speech must retain at
least 1% of raw microphone power. This empirical residual-echo guard does not
change the large board's detection parameters.

On detection, the device stops playback and sends `interrupt` before resuming
microphone upload, including up to 800 ms of speech history on the small board
or 300 ms on the large board, plus frames captured before upload resumes.
BLE transmission remains half-duplex; the cloud gets
no microphone stream during playback. `input.suspend` disables detection and
clears buffered speech.

Both profiles use a 64 KB data cache with 64-byte lines and external
NimBLE allocations. Small-board history and pending buffers contain 12,800
and 16,000 PCM16 samples, occupying 57,600 bytes in PSRAM; the large board's
4,800- and 8,000-sample buffers occupy 25,600 bytes. DMA buffers, AEC work frames and task
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

The 4096-byte receive callback waits for playback to release space when the
shared PCM buffer is full, applying backpressure through TCP with a 200 ms
space-wait budget. Failure cancels that response and rejects its remaining
audio until another valid `audio.begin`. A waiting block retains its original
playback generation, so it cannot enter or cancel a newer response.

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
