# Changelog

All notable changes to JUFF are documented here. The project follows
[Semantic Versioning](https://semver.org/).

## [Unreleased]

Current firmware development version: `0.6.10-dev`.

### 0.6.10-dev transport review fixes

- Serialize BLE credit snapshots through notification publication so concurrent
  updates cannot appear out of order and falsely cancel speech on the Mac.
- Apply bounded Wi-Fi receive backpressure when the playback buffer is full.
  Cancel an incomplete response on failure and prevent delayed audio from
  entering or cancelling a newer response.
- Send an ordered microphone reset boundary after stream changes or a partially
  transmitted frame. The updated Mac helper discards any incomplete old frame
  before assembling the next audio, while retaining support for older firmware.
- Refresh both native-resolution UI previews from the current firmware renderer,
  including the small board's battery percentage, plus its charging preview.

### 0.6.9-dev residual echo and interruption recovery

- Add a small-board residual-echo check using a window of complete AEC blocks
  before accepting a voice interruption; retain the existing VAD timing and
  the large board's separate detection parameters.
- Preserve 800 ms of small-board speech history so interruption recovery can
  include more of the opening words; keep 300 ms on the large board.
- Retry congested BLE microphone fragments with a real scheduler delay and a
  200 ms bound. Size the queue for history bursts and discard queued audio at
  mute, session and connection boundaries.
- Keep shared audio clocks running between naturally completed responses.

### 0.6.8-dev buffered BLE playback

- Buffer up to four seconds of decoded speech in PSRAM on either hardware profile.
  Prefill 300 ms before playback; completed short responses start without waiting
  for that threshold. Refill 250 ms after an underrun, with a 600 ms wait ceiling.
- Negotiate receiver credits so the Mac can fill available device memory without
  a fixed sending rate. Queue excess synthesized audio on the Mac, within a 2 MiB
  encoded-audio limit, and retain compatibility with older firmware.
- Fence packets with stream IDs and byte offsets, serialize responses through
  audible completion, and clear pending speech on interruption. Reject missing
  data or invalid credits instead of playing an incomplete response silently.
- Report playback queue depth, underruns, rejected data, and negotiated BLE PHY.

### 0.6.7-dev battery measurement and continuous percentages

- Keep numeric percentages and fill visible during charging and power transitions.
  Show 0 as a number, use 1% display increments, and reserve `--` for unavailable data.
- Use the ESP32-S3's lower-error 6 dB ADC range, with matching eFuse calibration,
  for the small board's divided battery voltage. External divider tolerance and
  the uncharacterized cell still limit voltage and capacity accuracy.
- Reject isolated voltage spikes with a three-sample median. Clear filter history
  at charge-state transitions so the next real sample replaces the previous estimate.
- Include battery voltage, estimate, and charging state in small-board BLE status
  for logging during battery operation; omit unsupported readings on the large board.

### 0.6.5-dev small-board battery indicator

- Add a compact battery icon with the estimated percentage inside and a fill
  level that follows the estimate. Use green with a lightning bolt when charging
  and red at 20% or below; unavailable readings show `--`.
- Read the small board's calibrated battery voltage and charge-status signal
  on a separate low-priority task, with filtering and display hysteresis.
  Keep battery sampling off the audio, display, and power-button tasks.
- Retain the large board's existing header without showing unsupported readings.

### 0.6.4-dev small-board power key

- Hold the 1.54-inch board's PWR key for 3 seconds, then release to shut down.
  Stop microphone capture and playback, mute the amplifier, and blank the display.
- Release the battery power latch; use deep sleep with PWR wakeup when USB
  continues supplying power. Ignore a held startup key until its first release.
- Keep BOOT interruption and the 3.5-inch hardware profile unchanged.

### 0.6.3-dev publication cleanup

- Restore the large board's 80 MHz display clock and single initialization
  sequence; remove temporary LCD, backlight and power-register diagnostics.
- Distinguish the earlier AEC measurements from the subsequent black-screen
  investigation. Display recovery remains unconfirmed on the physical board.
- Reject hardware-derived device IDs in publishable files and staged content,
  including when only the working copy has been sanitized.

### 0.6.2-dev large-board AEC

- Add opt-in ES8311 digital feedback on the 3.5-inch board, capturing microphone
  and playback reference together while preserving the 1.54-inch ES7210 path.
- Duplicate mono playback into both ES8311 transmit slots using bounded writes;
  handle partial writes, cancellation, and diagnostic playback consistently.
- Give the large board a 64 KB data cache and external NimBLE allocations.
- Use board-specific residual echo suppression and a 24 dB microphone-gain
  default for large-board AEC; report microphone peaks and full-scale samples
  so full-scale PCM output is visible during acoustic validation.
- Record AEC enablement and reference type in each hardware's firmware manifest.
- Document electrical measurements and acoustic validation separately in the
  [large-board record](docs/waveshare-lcd-3.5-aec-feasibility.md).

### 0.6.1-dev UI changes

- Replace the gradient orb with seven animated abstract expressions, a warm
  paper background, and green/coral controls on both display sizes. Draw the
  face with LVGL primitives, without new fonts, bitmaps, or display buffers.
- Keep touch interruption available during active turns, including notices;
  offer connection management from the idle action button.

### Repository and firmware packaging

- Replace device identifiers with placeholders and keep public validation
  records focused on methods, results and limitations.
- Scan new nonignored files and Git index contents as well as tracked working
  files. Report credential and device-ID locations without exposing their values.
- Include project and third-party license texts with firmware packages,
  using app and bootloader link maps to identify component and runtime inputs.

### Earlier unreleased work

- Add experimental local voice interruption to the 1.54-inch profile using
  ESP-SR 2.5.3 AEC and WebRTC VAD. Repository defaults disable it on both boards;
  the 1.54-inch profile allows explicit opt-in as described in the
  [firmware instructions](firmware/README.md). The package manifest's
  `features.voice_interrupt` boolean records the actual setting. Calibrate the
  physical speaker-reference gain, wait for actual reference audio before warmup, and
  use high-performance AEC with normal nonlinear suppression. Historical
  `0.6.0-dev` checks included failures and one calibrated smoke-test pass; they
  do not establish acoustic acceptance. Broader validation remains incomplete; see the
  [validation record](docs/voice-interruption-validation.md).
  The `0.6.1-dev` UI changes add no acoustic validation.
  Default builds retain half-duplex audio and manual interruption.
- Keep the 1.54-inch profile's 64 KB data cache while moving NimBLE allocations
  and speech history to PSRAM so BLE, display and AEC can initialize together.
- Disable automatic startup audio diagnostics; the explicit audio test remains.
- Preserve the experimental stop-before-upload design, including 300 ms of
  speech history and frames captured before upload resumes. BLE audio remains
  half-duplex.
- Disable voice interruption and clear speech buffers on `input.suspend`;
  route touch and BOOT stop actions to the active transport during playback.
- Add a Waveshare ESP32-S3-Touch-LCD-1.54 profile with ST7789/CST816 drivers,
  ES7210 microphone capture, GPIO amplifier control, and battery power hold.
- Reflow the voice and pairing screens for 240×240, with larger pairing codes.
- Keep each hardware profile under `firmware/boards/<board>/`, with separate
  build directories and packages named by hardware and software version.
- Require an explicit board for builds and serial port for flashing; validate
  hardware IDs and exclude embedded credentials when exporting firmware packages.
- Build and save both hardware versions in CI; test profile selection and packaging.
- Prepare the project for public development and reproducible setup on macOS.

## [0.5.0] - 2026-08-23

- Add the product-style 320×480 touch interface.
- Add encrypted BLE pairing and two-host bonding.
- Add bidirectional G.711 μ-law audio over BLE.
- Add paced speaker delivery and an extended playback buffer.
- Retain an authenticated Wi-Fi/WebSocket transport as an optional fallback.
- Add USB and BLE provisioning tools.
