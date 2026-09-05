# AEC feasibility: Waveshare ESP32-S3-Touch-LCD-3.5

Assessed and electrically tested on 2026-09-05 for the **non-B** board.
The application integration was introduced in JUFF `0.6.2-dev`.

## Current status

The current source version is `0.6.3-dev`. It removes temporary black-screen
diagnostics and restores the large board's 80 MHz SPI clock and single LCD
initialization sequence. It preserves the AEC configuration described below.

After the acoustic runs, a black screen was reported on the large device.
Temporary images also labelled `0.6.2-dev` added a 1 MHz SPI clock, LCD readback,
repeated resets, and backlight/power-register logging. Successful driver calls
and register logs did not confirm a visible image. Display recovery has not
been confirmed on the physical device; removing diagnostics is not a verified
black-screen fix.

The measurements below apply to the earlier AEC candidate, whose boot-reported
ELF SHA-256 prefix was `7ae28ef84`, with the 80 MHz display configuration.
They do not validate the later diagnostic images or the `0.6.3-dev` rebuilds.
The cleanup does not install firmware; it leaves the connected device on its
last diagnostic image. Packages are generated separately for each board and
version; use their manifest checksums to identify the actual binary.

Both board profiles were rebuilt and packaged as `0.6.3-dev` with their saved
AEC-enabled configurations. The 61 software regression tests passed, and each
package's images and checksums matched its corresponding build. These checks
cover compilation, packaging and software behavior; they add no physical
display or acoustic acceptance results.

## Assessment

The board has a documented path for acoustic echo cancellation without adding
an ES7210 or changing the PCB: receive the microphone and the ES8311's internal
digital playback reference, then run AEC on the ESP32-S3. The reference path
has now been measured on the board, and `0.6.2-dev` implements opt-in AEC and
speech interruption. Repository defaults remain off; the exported manifest
records each package's actual setting and reference type.

The ES8311 supplies a reference signal; it does not independently remove acoustic
echo. AEC and the decision to interrupt playback still need software, resource
validation, and acoustic tests.

## Primary evidence

- The [non-B board schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-3.5/ESP32-S3-Touch-LCD-3.5-Schematic.pdf#page=1)
  connects the microphone to the ES8311 ADC, and ES8311 OUTP/OUTN to the
  NS4150B amplifier. It shows no separate analog feedback branch from the DAC
  or speaker output to another ADC channel. The serial interface is fully wired:
  GPIO12 MCLK, GPIO13 BCLK, GPIO14 ASDOUT/RX, GPIO15 LRCK, GPIO16 DSDIN/TX.
- The [ES8311 User Guide](https://files.waveshare.com/wiki/common/ES8311.user.Guide.pdf#page=25),
  section 10.4, documents digital feedback for echo-cancellation applications.
  Register `0x44[6:4] = 5` selects ADC plus DACR on the serial output. Section 1
  also describes routing DAC data to the digital output.
- Espressif's [ES8311 configuration header](https://github.com/espressif/esp-adf/blob/release/v2.x/components/esp_codec_dev/device/include/es8311_codec.h)
  defines `no_dac_ref = false` as enabling the DAC reference in the right channel
  of a two-channel recording. The [driver implementation](https://github.com/espressif/esp-adf/blob/release/v2.x/components/esp_codec_dev/device/es8311/es8311.c)
  writes `0x58` to register `0x44` for this mode. The locally installed
  `esp_codec_dev` 1.5.11 has the same behavior.
- The [ESP-IDF 5.5.1 I2S guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/api-reference/peripherals/i2s.html#full-duplex)
  supports simultaneous standard-mode TX and RX with shared BCLK/WS. Shared
  clocks alone do not guarantee that independent software read/write calls
  align sample-for-sample; returning mic and reference in the same RX stream
  avoids relying solely on the timing of a host-side playback copy.
- [ESP-SR's AEC API](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/acoustic_echo_cancellation/README.html)
  accepts a microphone signal and a playback-reference signal. JUFF already
  includes ESP-SR 2.5.3 and an experimental frontend for the smaller board.

## Why 0.6.1-dev could not interrupt by speech

Before the large-board integration, `firmware/main/audio_io.c`:

1. ES8311 is opened in ADC/DAC mode, and both I2S directions are initialized.
   The hardware interface is not intrinsically half-duplex.
2. `no_dac_ref = true` disables the useful internal feedback mode.
3. The codec opens with one channel, so the second receive slot is not retained.
4. The normal capture loop continues reading but discards audio during playback.

`firmware/main/Kconfig.projbuild` restricted `JUFF_VOICE_BARGE_IN` to the 1.54-inch
profile. Simply removing that restriction would have been incorrect: the
enabled path called ES7210-specific channel-gain configuration, and its
two-channel capture assumption did not match the large board's mono stream.

## Implemented adaptation

Hardware profiles and release artifacts remain separate. The large-board
experimental profile implements the following:

1. Select ES8311 digital feedback and capture two slots as `[microphone,
   reference]`. Do not apply the small board's ES7210 reference gain.
2. Configure compatible 24 kHz, 16-bit receive/transmit formats. The large board
   currently uses one codec handle for input and output, so changing its open
   format affects both directions.
3. Route all mono playback, including diagnostic tones, through one bounded
   conversion buffer that supplies the same sample in both transmit slots.
   This avoids an empty DACR reference and preserves the mono playback rate.
   Keep network packets and queued audio mono.
4. Reuse the frontend's synchronized conversion of both inputs from 24 kHz to
   16 kHz, AEC processing, speech detection, and local interruption routing.
5. Preserve manual stop and ordinary voice operation if frontend initialization
   fails. Keep reference audio local; never upload it as microphone speech.

The slot requirement is substantive: section 11.1 of the ES8311 guide (page 28)
selects the left input slot for the analog DAC by default, while feedback mode 5
returns the right input slot as the reference. Feeding identical PCM to both
slots ensures they represent the same source signal. The feedback is from the
serial digital input, not an analog measurement of the DAC or power amplifier.

Speech interruption does not require continuously sending microphone audio over
BLE while the robot speaks. The device can detect speech locally, stop playback,
and then resume the existing microphone upload. This is distinct from a fully
simultaneous network conversation mode.

## Electrical measurements

A standalone probe ran with the amplifier held disabled through TCA9554 EXIO7.
It preserved all other expander bits, including LCD reset on EXIO1, and did not
initialize NVS or radio components. The NVS bytes were identical before and
after testing. Each case settled for 200 ms and measured 7680 stereo frames
(320 ms at the configured 24 kHz rate).

For a 750 Hz sine and codec volume 70:

| TX peak | Reference RMS | Reference peak | Reference clips |
| ---: | ---: | ---: | ---: |
| 0 | 0.00 | 0 | 0 |
| 1500 | 1060.65 | 1500 | 0 |
| 6000 | 4242.68 | 6000 | 0 |
| 12000 | 8485.30 | 12000 | 0 |
| 30000 | 21213.19 | 30000 | 0 |

Nonzero cases had unit gain and correlation 1.00000 with the periodic source.
This confirms signal routing for these test tones, not an independent absolute
latency or sample-clock accuracy measurement. The microphone measured about
40–48 RMS, with less than 1 RMS at 750 Hz while the PA was disabled.

At TX peak 6000, codec volume 0, 40, 70 and 100 all produced the same 4242.68 RMS
reference. DAC mute also left it unchanged. This establishes that this reference
is before DAC volume/mute; reference energy cannot establish that the speaker
is audible. All ten cases completed in 5248 ms with zero TX errors and PA-low
guards passing. Two duplicate-disable messages occurred during probe cleanup
after sampling, because codec close had already disabled the I2S channels.

## Acoustic tuning observations

The automated fixture uses prerecorded robot speech sent over BLE and a separate
computer speaker for near-end speech. Each robot-only trial repeats a 6.94-second
fixture three times, at PCM gains 0.35, 0.65 and 1.0 with codec volume 70. Gain is
a multiplier on the fixture, not an acoustic sound-pressure measurement.

The initial 30 dB microphone / normal-NLP build completed the 0.35 trial but
self-interrupted after 13.37 seconds at gain 0.65. A second build using aggressive
NLP completed the first two trials but self-interrupted at 6.45 seconds at gain
1.0. Neither failed run reached its near-end interruption trials. Ambient noise
estimates differed, so these runs do not isolate the effect of NLP.

The second build's telemetry identified microphone PCM full-scale samples before
AEC processing: 275 across printed gain-0.65 windows and 563 across printed
gain-1.0 windows; the digital reference did not clip. These are totals over
reported windows, not every sample in each trial. They establish full-scale
codec output, not which analog or digital stage saturated. A third build reduced
the driver's microphone-gain setting from 30 to 24 dB. Its printed windows had
no full-scale samples, but gain 1.0 still self-interrupted at 17.55 seconds.

The [ES8311 guide, section 10.1](https://files.waveshare.com/wiki/common/ES8311.user.Guide.pdf#page=20)
defines the analog PGA as register `0x14[3:0]`, in 3 dB steps; the driver starts it
at 30 dB (`0x1a`). In contrast, the driver's `set_in_gain()` changes register
`0x16[2:0]`, a digital ADC amplitude scale in 6 dB steps described in
[section 10.6](https://files.waveshare.com/wiki/common/ES8311.user.Guide.pdf#page=27).
Reducing that setting does not reduce analog PGA gain, and the absence of
full-scale output does not exclude earlier distortion. The large-board AEC
profile uses a 24 dB digital-scale default; the small board and ordinary
large-board profile retain their existing gain settings. Saved configurations
must be checked explicitly because Kconfig defaults do not override them.

## Software regression checks

The regression suite includes production-code checks for two-slot packing, PCM
boundaries, partial writes, deadlines, cancellation, microphone/reference
separation, allocation-failure fallback, playback diagnostics, and board-specific
package metadata. It also checks board-specific Kconfig defaults and NLP levels
against the installed SDK when available. The native C tests use
address/undefined-behavior sanitizers.
They establish stream/control behavior, not acoustic performance.

## Acoustic verification of the earlier AEC candidate

The `0.6.2-dev` candidate tested before the display investigation used `AEC_MODE_FD_HIGH_PERF` with
`AEC_NLP_LEVEL_VERYAGGR`, driver digital scale 24 dB and codec volume 70. The
analog PGA retains the driver's 30 dB setting. The small board retains its
existing normal NLP and separate ES7210 configuration.

Two consecutive automated acoustic runs of this candidate completed all six
robot-only trials without self-interruption and all four near-end trials with
voice interruption. Both runs began with the highest fixture gain after reboot.
The separate near-end source measured 614 and 606 peak RMS respectively in
100 ms uploaded microphone frames while the robot was silent; these are signal
levels, not calibrated acoustic SPL or a measured speaking distance.

The final run added an upper playback-duration bound and produced:

| Robot-only PCM gain | Playback duration | Termination |
| ---: | ---: | --- |
| 1.00 | 20.774 s | Natural end |
| 0.65 | 20.790 s | Natural end |
| 0.35 | 20.793 s | Natural end |

The two near-end trials played independent speech four seconds into the robot's
answer. Interruption events arrived 1009 and 957 ms after starting the near-end
playback process; cancellation arrived at 1009 and 958 ms. These timings include
host playback startup, fixture onset, and BLE event delivery. Device logs
independently confirmed speech detection, stopped playback, and 330/310 ms of
buffered speech. Each trial resumed microphone upload (70,400 bytes observed).
The previous candidate run's two interruptions took 1014 and 961 ms.

The final robot-only windows showed approximately 22.97, 20.56 and 20.94 dB
reduction in aggregate microphone-to-clean RMS. This includes nonlinear
suppression and is **not an ERLE measurement**. The highest-gain trial contained
one full-scale microphone PCM sample; the other trials contained none in their
reported windows, and reference clipping counters remained zero. This does not
prove analog linearity or headroom for arbitrary louder sources.

With the display, BLE and AEC initialized, internal heap free space was 33,111
bytes and the largest block was 23,552 bytes. Maximum observed AEC processing
time in the final run was 26.39 ms per 32 ms frame. No RX overflow, crash or
audio read/write failure was recorded; one BLE microphone congestion event was
recorded. These are observations from bounded runs, not long-duration stress
acceptance or minimum heap measurements.

The earlier run of the same candidate took 26.580 and 24.197 seconds for two
20.82-second fixtures. The final run did not reproduce the excess duration;
its cause remains unconfirmed. Do not interpret natural playback completion
alone as proof of uninterrupted transport. First-syllable intelligibility,
different rooms and distances, stronger simultaneous signals, louder codec
settings and long-duration operation remain unvalidated.

The internal digital reference does not measure the external amplifier/speaker
output. Nonlinear distortion, microphone clipping, enclosure coupling, and
timing errors can leave residual echo. Espressif's
[microphone design guide](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/audio_front_end/Espressif_Microphone_Design_Guidelines.html#echo-reference-signal-design-suggestion)
recommends a clean reference near the analog playback path. These limitations
must be measured before calling large-board speech interruption reliable.

The electrical probe was temporary. Before the acoustic checks, the application
AEC firmware was restored without erasing pairing/configuration storage. Those
checks used the earlier candidate identified above. Subsequent display
diagnostics replaced the installed image, as described under Current status.
These results support a calibrated experimental configuration, not a claim of
reliable interruption in every acoustic setting or confirmed display recovery.
