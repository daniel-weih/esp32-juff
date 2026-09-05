# Contributing to JUFF

Thank you for helping improve JUFF. Small, focused pull requests are easiest to
review.

## Development setup

On macOS, run:

```bash
./scripts/bootstrap_macos.sh --skip-key
./scripts/check_repository.sh
make test
```

Firmware development additionally requires ESP-IDF 5.5.x:

```bash
./scripts/setup_esp_idf.sh
make firmware-build-all
```

Keep hardware profiles under `firmware/boards/<board>/`. Build both supported
boards when changing shared firmware. For a single target, use
`make firmware-build JUFF_BOARD=waveshare-lcd-3.5` or
`make firmware-build JUFF_BOARD=waveshare-lcd-1.54`. Flashing additionally
requires an explicit `PORT`; never reuse another board's saved SDK config.

## Before opening a pull request

- Explain the user-visible behavior and the hardware used for testing.
- Run `./scripts/check_repository.sh`, `make test`, and any relevant firmware
  build or on-device test. The repository check scans tracked files, new
  nonignored files and the Git index; diagnostics show locations without
  printing credential values.
- Never commit `.env` files, `firmware/sdkconfig`, flash backups, API keys,
  Wi-Fi credentials, device tokens, or BLE bonding data.
- Keep protocol changes backward compatible where practical and document them
  in `docs/architecture.md`.
- Update `CHANGELOG.md` for user-visible changes.

By submitting a contribution, you agree that it is licensed under Apache-2.0.
