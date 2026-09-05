# Supplemental third-party license texts

These files supplement notices missing from the component packages used by
the firmware. Each `provenance.json` records the official source, reviewed
component hash, text checksum and scope. Packaging checks these values before
using the text; updating a component requires reviewing its supplement again.

- `espressif__dl_fft/`: the upstream MIT license for the repository revision
  declared by dl_fft 0.6.0. File-level Apache-2.0 notices are collected separately.
- `espressif__esp-sr/`: the upstream WebRTC license accompanying the VAD used by
  ESP-SR 2.5.3. Its exact WebRTC source revision is not published by the component;
  the provenance file makes that limit explicit.

These supplements are not the complete firmware notice bundle. The packager
also reads the actual application and bootloader link maps and collects
component, ESP-IDF and target runtime notices from their installed source trees.
Each firmware ZIP contains `LICENSE`, `THIRD_PARTY_NOTICES.md` and a `licenses/`
directory with an inventory. Private build configuration and source-machine
paths are not included.
