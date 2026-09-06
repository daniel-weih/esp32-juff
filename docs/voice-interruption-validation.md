# Experimental voice interruption: methods and results

The initial acoustic measurements below are historical results from experimental
`0.6.0-dev` firmware on the Waveshare ESP32-S3-Touch-LCD-1.54. One calibrated
acoustic smoke test passed; broader acceptance remains incomplete. Earlier
self-interruptions, missed near-end speech and startup failure remain part of
the record. `0.6.1-dev` changes the UI only and does not add acoustic validation.

Repository defaults disable `CONFIG_JUFF_VOICE_BARGE_IN` for both boards. It
can be explicitly enabled for either hardware profile as described in the
[firmware guide](../firmware/README.md). A package's `manifest.json` records its
actual setting in `features.voice_interrupt`; a version number alone does not
identify whether the experiment is enabled.

## Wake-mode interruption regression

During `0.7.0-dev` testing, normal replies on the small board repeatedly stopped
after local interrupt events while audio remained buffered. A controlled replay
of a 24-second synthesized speech excerpt, following the wake acknowledgement,
also stopped at 19.035 seconds without an added near-end source. Its terminal
credit reported no playback underruns or dropped speaker bytes.

The echo guard was checked only after the 120 ms speech candidate had already
accumulated. Frames rejected as echo could therefore pre-fill the candidate,
and a brief change in the guard's energy window could immediately release it.
The corrected detector resets the candidate whenever any qualification fails,
including the echo guard.
A regression test fails on the old implementation at AEC chunk sizes 160, 256
and 512 and passes after the correction.

With the first counter correction, the same 24-second excerpt completed twice
at source gain 1.0. Four
independent synthetic interruptions, started 1 or 4 seconds into playback,
produced stop events 894–944 ms after launching the external audio player and
resumed microphone delivery. The two earlier insertions overlapped active
speaker reference (192 and 232 RMS across the guard window), rather than only
testing a pause. All these streams retained zero underruns and dropped bytes.
These timings use process launch, not measured acoustic onset; the room setup
and synthetic sources do not establish a general false-positive rate.

A longer 69.69-second fixture exposed a remaining weakness: it still stopped
around audio position 53 seconds, including a replay with zero underruns. The
accepted AEC window retained only 1.1% of raw microphone power. A later repeat
still stopped at 67.588 seconds: a quiet raw MIC made the retained-power ratio
look strong beside an active reference. The final candidate therefore requires
360 ms of continuous evidence when retained power is 1–5% of MIC power or less
than 25% of reference power, while speaker reference is active. Stronger speech
and reference pauses retain 120 ms. The fast path requires its own consecutive strong frames;
the longer path preserves continuously qualified speech across strength changes.
The 1% rejection floor, absolute RMS floor, AEC/NLP settings and large-board
policy remain unchanged. Weak sustained speech remains eligible for detection.

Long playback also exposed a separate transport problem. One 69.69-second
local replay completed without an interrupt but took 80.718 seconds with 32
buffer underruns. Before starvation began, a new encryption event re-requested
connection parameters and the interval changed from 15 to 13.75 ms. The previous
7.5–15 ms request range was below Apple's documented minimum. The firmware now
requests min=max=15 ms and logs rejected updates as well as the actual interval.
The central still chooses the final parameters; a shorter interval does not
establish higher application throughput. See
[Apple's connection parameter guidance](https://developer.apple.com/forums/thread/822187).

The Mac helper also declares a finite latency-critical I/O activity while a
response is queued or playing, and releases it at completion, cancellation or
disconnection. This protects remote audio from discretionary background
throttling; App Nap was not established as the cause of the measured starvation.

The final small-board candidate (application SHA-256 prefix
`2a10a29abee3c7e2`) was installed while preserving the saved WAKE mode and pairing.
Controlled checks on that binary include:

| Case | Observation |
| --- | --- |
| Production host, cached wake cue and a local simulated gateway streaming the 69.69-second fixture | Exactly one wake cue; speech ended after 69.729 seconds; no local interrupt, underrun or dropped speaker bytes |
| Two consecutive local replays of the same 69.69-second fixture after a wake cue | Both ended after 69.734 seconds; no local interrupt, underrun or dropped bytes; peak device queue 3.94–3.95 seconds |
| Two normal-level independent synthetic interruptions during active speaker reference | Local events after 926 and 908 ms; playback cancelled after 938 and 922 ms; microphone delivery resumed; no underruns or dropped bytes |
| Two quieter independent synthetic interruptions during active speaker reference | Local events after 913 and 932 ms; playback cancelled after 914 and 933 ms; microphone delivery resumed; no underruns or dropped bytes |

The simulated gateway produced 400 ms audio chunks every 100 ms, including one
400 ms source gap. It used the production conversation client, native BLE helper
and device firmware, but made no cloud requests. Microphone packets received by
the local gateway were discarded immediately. Near-end calibration measured
1447 RMS for normal level and 778 for quieter speech at the loudest 100 ms
microphone frame; event times start at external player launch. Earlier quieter
misses and late stops remain in the record below; these limited trials do not
establish general reliability. A cold-start synthetic wake was also missed
once before playback began; wake recognition accuracy is a separate open limit.

Speech preservation was an acceptance gap in those four trials. Offline
Whisper recognized the complete fixed phrase in the silent-robot calibration. After normalizing
simplified/traditional characters, greedy decoding recovered both parts of the
phrase in two of the four interrupted clips; one quieter clip lacked the
opening, and one normal-level clip produced unrelated text. A five-beam decode
did not consistently recover the missing material and made one previously
recognized quieter clip worse. These results do not by themselves distinguish
AEC distortion from ASR error. They establish that four successful stop events
must not be presented as four reliably recognized follow-up utterances.

### Subsequent two-board acceptance check

On 2026-09-07, both installed `0.7.0-dev` devices were checked again with the
same fixed near-end phrase and the updated Mac speaker packet sizing. The
1.54-inch board stayed in LIVE and the 3.5-inch board in WAKE. Each board ran
two normal-level and two quieter interruptions, with the source launched one
or four seconds after playback began. The one-second insertions overlapped
active reference; later insertions also covered reference pauses.

All eight cases stopped playback, resumed microphone delivery and retained
both parts of “等一下，换个话题” in an unprompted, offline Whisper decode of
the received 16 kHz microphone PCM, after normalizing traditional characters.
Robot-silent calibration clips also retained both parts. Normal calibration
peaks were 1270–1331 RMS per 100 ms; quieter peaks were 697–713. Stop events
arrived 864–947 ms after starting the external player, not measured acoustic
onset. Every interrupted stream and the following fresh reply added zero
underruns and zero dropped speaker bytes.

Both boards also completed a 69.69-second robot-only response without a local
interrupt or new underrun/drop: 69.731 seconds on the small board and 69.716
seconds on the large board. These runs used 15 ms BLE intervals. A separate
earlier 30 ms run had 20 underruns and triggered further transport validation;
the same 69.69-second fixture subsequently completed at an actual 30 ms
interval in 69.733 seconds with zero underruns/drops and a 3.9-second peak
queue. That check used the 244-byte GATT write cap, the unchanged audio codec
and a temporary firmware connection request covering 15–30 ms. The exact
original application was restored after the check; production still requests
15 ms and the central chooses the actual interval.

A forced 45 ms pressure test remained below the stream's required throughput:
about 8.6 KB/s of received encoded audio against 24 KB/s needed for real-time
24 kHz μ-law. Repeated buffer underruns prevented completion before the
deadline. This is a documented link-capacity boundary, not a passing acoustic
test. The packet change does not promise continuous real-time playback on a link with
insufficient sustained throughput; a finite buffer cannot compensate for
that condition indefinitely.

These controlled repetitions close the short-phrase check for this setup.
They do not erase the earlier failed clips or establish human speech accuracy,
distance, long-term false-interruption rate, or reliability at every BLE rate.
Recordings and transcripts remain ignored local artifacts; no cloud request
was made by these checks.

## Buffered playback regression investigation

The `0.6.8-dev` buffer/throughput tests suspended microphone input and used
low-volume tones. They did not validate acoustic interruption. A subsequent
automated test kept local AEC/VAD enabled and replayed a 22.24-second synthesized
voice fixture through the device at its normal configured speaker volume.

The same fixture caused an unwanted interruption around 11.8 seconds on both
the previous `0.6.7-dev` firmware and the buffered implementation. Keeping TX/RX
clocks continuous between natural responses did not by itself resolve this
case. Neither AGGR nor VERYAGGR nonlinear suppression reliably resolved it, so
the small board retains NORMAL. These observations establish a reproducible
residual-echo failure, not a proven change in the physical echo path.

At one unwanted interruption, the current raw microphone/reference/clean AEC
block RMS values were 7133/1042/281. The existing 180-RMS floor still accepted
the residual speech. These raw and processed block measurements help compare
signal levels; they are not a sample-aligned double-talk measurement.

The `0.6.9-dev` small-board candidate adds a roughly 128 ms window of complete
AEC blocks. When its reference RMS is at least 80, accepted speech must retain
at least 1% of the raw microphone power; otherwise the original speech candidate
remains blocked. The existing 120 ms VAD candidate duration is unchanged. This
is an empirical residual-echo rejection rule, not a general double-talk
classifier or an estimate of the DSP's exact delay. The large board retains
its separate detection parameters.

Interruption recovery also exposed lost BLE microphone fragments: at the
configured 100 Hz scheduler rate, the old 2 ms retry delay converted to zero
ticks. The fix waits at least one tick after rejection, retries only rejected
fragments, and allows 200 ms per fragment / 400 ms per complete frame. A
12-frame queue accommodates history bursts. The small board retains 800 ms
of pre-interruption speech instead of 300 ms; the large board stays at 300 ms.

Controlled local results, with microphone detection enabled throughout:

| Case | Observation |
| --- | --- |
| Five complete 22.24-second synthesized responses | All completed, no unwanted interruption; three at source gain 1.0, one at 0.65, one at 0.35 |
| Two normal-level independent synthetic interruptions | Both stopped playback; local offline ASR recovered both parts of the known opening phrase |
| Two quieter independent synthetic interruptions | Both produced stop events, but one responded late and did not preserve an identifiable opening phrase; the other recovered both parts |
| Transport during these runs | Zero reported playback underruns/rejected speaker bytes and no microphone-congestion drops |

Near-source calibration measured approximately 1421 RMS at the loudest 100 ms
microphone frame for the normal level and 715 for the quieter level. These are
device PCM measurements, not sound-pressure or distance measurements. Stop
events in the quieter test arrived about 2.55 s and 0.91 s after launching the
independent playback process; process launch is not measured acoustic onset.
The earlier diagnostic firmware also responded late in the quieter first
round. That trial did not meet speech-preservation acceptance: a stop event
alone does not establish usable recovered speech. The results above do not establish long-term,
far-distance or real-user reliability. Test microphone recordings and ASR
outputs remain local ignored artifacts; none were uploaded for transcription.

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
