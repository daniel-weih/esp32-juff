# Security policy

## Supported versions

Security fixes are applied to the latest release and the `main` branch.

## Reporting a vulnerability

Please do not open a public issue for a vulnerability, leaked credential, or
pairing bypass. Once the repository is hosted on GitHub, use its private
security-advisory form. If that is unavailable, contact a maintainer privately
before sharing reproduction details.

Include the affected version, transport (BLE or Wi-Fi), impact, and the
smallest safe reproduction. Do not include real DashScope keys, Wi-Fi
passwords, device tokens, BLE bonding data, or full flash dumps.

## Credential handling

- DashScope credentials belong in `~/.config/qwaudio/config.env` (or the
  equivalent path under `$XDG_CONFIG_HOME`) only.
- `host/.env`, `firmware/sdkconfig`, NVS dumps, and `backups/` are ignored.
- The Qwen Gateway binds to loopback by default.
- The Wi-Fi Bridge must use a unique random device token before it is exposed
  to a LAN.
- Rotate any credential that was pasted into an issue, log, chat, or commit.
