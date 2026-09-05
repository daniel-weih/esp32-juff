# Experimental voice interruption: methods and results

The acoustic measurements below are historical results from experimental
`0.6.0-dev` firmware on the Waveshare ESP32-S3-Touch-LCD-1.54. One calibrated
acoustic smoke test passed; broader acceptance remains incomplete. Earlier
self-interruptions, missed near-end speech and startup failure remain part of
the record. `0.6.1-dev` changes the UI only and does not add acoustic validation.

Repository defaults disable `CONFIG_JUFF_VOICE_BARGE_IN` for both boards. It
can be explicitly enabled for the 1.54-inch profile as described in the
[firmware guide](../firmware/README.md). A package's `manifest.json` records its
actual setting in `features.voice_interrupt`; a version number alone does not
identify whether the experiment is enabled.

## Test boundaries

- **Software regression checks** compile production stream/control code with
  deterministic AEC/VAD mocks. They cover buffering, mute boundaries, playback
  generations, allocation failures and interruption routing. They do not
  measure real echo suppression or speech classification.
- **Electrical measurements** hold the speaker amplifier disabled, drive a
  known DAC signal and measure the ADC reference. They establish reference
  routing and headroom under the tested conditions, not acoustic performance.
- **Offline DSP checks** run real ESP-SR on prerecorded microphone/reference
  inputs. Input labels determine which detection results can be called false
  positives; absence of an added speech track does not establish echo-only input.
- **Acoustic checks** exercise the complete BLE firmware with a calibrated
  external near-end source. A successful check requires both local detection
  and playback cancellation. Broader acceptance also requires speech
  preservation and sustained robot-only playback tests.

## Electrical reference path

The [board schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.54/ESP32-S3-LCD-1.54-Schematic.pdf)
connects ES8311 DAC `OUTP/OUTN`, before the power amplifier, to ES7210
`MIC3P/MIC3N`. Each leg contains 2.2 kΩ, 10 kΩ and 20 kΩ series resistors;
a 4.3 kΩ resistor bridges the differential output. Ignoring capacitors and ADC
loading gives `4.3 / (2 × 32.2 + 4.3) = 0.0626`, approximately 24.1 dB
attenuation. This is a circuit estimate, not an exact broadband gain.

The ES7210 TDM I²S slot order is MIC1, MIC3, MIC2, MIC4. Physical MIC3 gain
therefore uses channel-mask bit 2 while its captured TDM slot is 1. A probe
held PA GPIO 7 low, drove a 750 Hz sine into the DAC and read all four slots.
At device software volume 70, DAC register `0x32` was `0xa8` (−11.5 dB):

| Source PCM peak | MIC3 gain | Reference RMS | Reference peak | Clipped samples |
| ---: | ---: | ---: | ---: | ---: |
| 0 | 0 dB | 0.6 | 2 | 0 |
| 1500 | 0 dB | 14.6 | 22 | 0 |
| 6000 | 0 dB | 58.4 | 84 | 0 |
| 12000 | 0 dB | 116.6 | 166 | 0 |
| 6000 | 24 dB | 749.4 | 1061 | 0 |
| 12000 | 24 dB | 1497.2 | 2114 | 0 |

For nonzero tones, coherent 750 Hz RMS matched total reference RMS to the
shown precision. Scaling with source amplitude and MIC3 gain confirms a
working reference path under these conditions. A 50–60 RMS reference at
0 dB is consistent with the board's attenuation and is not evidence of a
hardware defect.

At device volume 100 and source PCM peak 30000, DAC register `0x32` was
`0xc6` (+3.5 dB). PA remained disabled:

| MIC3 gain | Total RMS | Coherent 750 Hz RMS | Absolute peak | Clipped / 7200 samples |
| ---: | ---: | ---: | ---: | ---: |
| 21 dB | 9128.6 | 8723.5 | 13971 | 0 |
| 24 dB | 12871.0 | 12298.9 | 19758 | 0 |
| 30 dB | 24583.5 | 23633.4 | 32768 | 1235 |

The 30 dB setting clips the ADC. At 21/24 dB, energy outside the intended sine
remains despite no ADC clipping, consistent with upstream distortion such as
DAC clipping; its exact source was not established. Zero clip counts do not
prove waveform quality. The implementation defaults to 24 dB reference gain,
or 21 dB when compile-time device volume exceeds 85. This does not establish
headroom for every maximum-volume signal.

## Offline DSP measurements

The paired baseline uses the first eight seconds of Espressif's
`aec_in_near.wav` and `aec_in_far.wav`, repeated over 12-second cases. The
[official description](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/acoustic_echo_cancellation/README.html)
labels the microphone file as containing echo, without establishing that this
excerpt contains no near-end speech. The full recording also contains strong
microphone activity independent of the reference: at 27.2–28.2 s their RMS
values are approximately 1800 and 3 respectively. This does not label the
first eight seconds. Results below use **paired baseline, no added near-end**,
not verified echo-only input.

An early frontend triggered at 1.46–1.50 s before additional near-end audio was
mixed in. These are observed detections on unlabelled input, not established
physical false positives. Separately, stream tests exposed a timing defect:
leading PCM silence consumed AEC warmup. The revised design waits for actual
PCM playback and active reference energy before counting 400 ms of warmup.
It then requires 120 ms of continuous speech above
`max(180 RMS, 3 × estimated noise RMS)`.

With that timing change, the LOW_COST / AGGR configuration produced:

| Input | Reference multiplier | Baseline offset | Observed result |
| --- | ---: | --- | --- |
| No added near-end | 0.25 | 0, 1, 2, 4 s | No interruption in four 12 s cases |
| No added near-end | 0.5012 | 0 s | No interruption in one 12 s case |
| Near-end added at 6 s | 0.25 | 0, 2 s | Trigger at 6.260 s in both cases |
| Near-end added at 6 s | 0.5012 | 0 s | Trigger at 6.300 s |
| No added near-end | 0.0316 | 0 s | Trigger at 9.500 s |

Thus five baselines ran for 60 s without interruption and three added-near-end
cases triggered 260–300 ms after the addition. These are specific-input
results, not a false-positive rate or acoustic acceptance. Reference
multipliers scale prerecorded data; they are not ADC gain settings.

An eight-case HIGH_PERF matrix used the complete `aec_process` call, reference
multiplier 0.25, offsets 0/2 s and added-near-end strengths 0/0.10. Metrics were
outside the timed AEC call:

| NLP | Offset | No added near-end: first trigger | Near-end added at 6 s: first trigger |
| --- | ---: | --- | --- |
| NORMAL | 0 s | 1.980 s | 1.980 s, before addition |
| NORMAL | 2 s | None in 12 s | 6.320 s |
| AGGR | 0 s | None in 12 s | 7.580 s |
| AGGR | 2 s | None in 12 s | 6.300 s |

All eight cases completed without DSP errors or input clipping; the standalone
AEC call averaged about 8.7 ms per 32 ms frame. Unlabelled inputs prevent
classifying these timestamps as physical false positives or acoustic passes.

## Historical complete-firmware acoustic results: 0.6.0-dev

Initial experimental firmware both interrupted speech and stopped during
robot-only playback. Early one-second microphone/reference/output RMS
measurements were `844/60/793`, `2995/48/2637` and `7233/49/620`. Low reference
energy alone did not establish whether gain, distortion, timing or detection
was the principal cause.

Later checks used device volume 70, 24 dB MIC3 gain, a 64 KB data cache,
external NimBLE allocations and PSRAM history buffers. Each check established
15 seconds of quiet BLE connectivity, played the robot alone for four seconds,
then introduced a separate near-end TTS source while playback continued.
Near-end source strength was measured at the device with robot playback off.

| Configuration | Near-end source measurement | Outcome |
| --- | --- | --- |
| FD_LOW_COST / AGGR | Source RMS 132.4; ambient 65.3; peak 100 ms frame 297.3 RMS | No timely interrupt/cancel pair: failed |
| FD_HIGH_PERF / NORMAL | Same weak source | No timely interrupt/cancel pair: failed |
| FD_HIGH_PERF / NORMAL | Calibrated peak 100 ms frame: 990 RMS | One bounded smoke test passed |

In the LOW_COST / AGGR failure, final output windows measured 5, 38 and 71 RMS.
Those averages were below the detector's minimum. Source weakness and
suppression remained possible contributors; neither was established as the
sole cause. Earlier mic/reference statistics were taken after a writable AEC
call, so they cannot be assumed to be unmodified ADC values. Subsequent
telemetry measures those inputs before processing.

The calibrated HIGH_PERF / NORMAL check produced no interruption during its
four-second robot-only interval. BLE reported `interrupt` and
`playback.cancelled` 944 ms after starting the external playback process; serial telemetry
independently confirmed local detection. On resuming upload, 310 ms of buffered
audio was sent. Buffer delivery does not establish first-word intelligibility.
The two weak-source failures remain failures; the later pass does not resolve
performance for ordinary speech across speakers, levels and distances.

## Resource checks and remaining limits

A complete build initially failed during NimBLE startup with
`ble_att_svr_init` returning `rc=6`, before acoustic playback. Moving NimBLE
allocations and 25,600 bytes of history/pending audio to PSRAM resolved startup
for the tested candidate. DMA buffers, AEC work frames and task stacks remain
internal. Partial frontend allocations are released on failure, retaining
ordinary half-duplex operation. The 27 software checks then passed.

The calibrated `0.6.0-dev` check measured 43,283 bytes of internal heap free
space after BLE initialization, with a 31,744-byte largest block. One RX drop
occurred during boot and none during the test. Peak logged AEC time was
26,051 μs per 32 ms frame. These are single-run observations, not sustained
memory or timing guarantees.

Broader acceptance is still needed for quiet speech, greater distances,
multiple speakers, long sessions, sustained robot-only playback and first-word
intelligibility. Muting must also prevent detection and discard buffered
speech across unmute. The microphone/speaker self-test and mock regression
suite do not establish these acoustic properties. Neither the historical smoke
test nor the `0.6.1-dev` UI update makes voice interruption an accepted default.
