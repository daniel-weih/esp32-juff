#!/usr/bin/env python3
"""Export a built JUFF board as a named, self-contained firmware package."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import tempfile
import zipfile

from firmware_licenses import collect_firmware_licenses


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def package_firmware(project_root, board_id):
    if not re.fullmatch(r"[a-z0-9][a-z0-9.-]*", board_id):
        raise ValueError("Invalid board ID")
    board = read_json(project_root / "firmware/boards" / board_id / "board.json")
    if board["id"] != board_id:
        raise ValueError("Board metadata does not match the requested hardware")

    build = (project_root / "firmware/build" / board_id).resolve()
    project = read_json(build / "project_description.json")
    config = read_json(build / "config/sdkconfig.json")
    flasher = read_json(build / "flasher_args.json")
    selected = [key for key, value in config.items() if key.startswith("JUFF_BOARD_") and value is True]
    if selected != [board["kconfig_symbol"]]:
        raise ValueError("Built firmware does not match the requested hardware; rebuild this board")
    if project["target"] != board["chip"] or flasher["extra_esptool_args"]["chip"] != board["chip"]:
        raise ValueError("Built firmware has the wrong chip target")
    if Path(project["config_file"]).resolve() != build / "sdkconfig":
        raise ValueError("Built firmware must use this board's isolated sdkconfig")
    for key in ("JUFF_WIFI_SSID", "JUFF_WIFI_PASSWORD", "JUFF_DEVICE_TOKEN"):
        if config.get(key):
            raise ValueError(f"{key} must be empty in distributable firmware; provision the device instead")

    version = project["project_version"]
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9.+_-]*", version):
        raise ValueError("Firmware version cannot be used as a package name")
    prefix = f"juff-{board_id}-v{version}"
    files = {}
    images = []
    for offset, relative in sorted(flasher["flash_files"].items(), key=lambda item: int(item[0], 0)):
        source = (build / relative).resolve()
        if not source.is_relative_to(build):
            raise ValueError("Flash image must be inside the board's build directory")
        data = source.read_bytes()
        role = next((key for key in ("bootloader", "partition-table", "app")
                     if flasher.get(key, {}).get("file") == relative), None)
        if role is None:
            raise ValueError(f"Unsupported flash image: {relative}")
        if role == "app" and board_id.encode("ascii") + b"\0" not in data:
            raise ValueError("Application binary is missing the matching hardware ID; rebuild this board")
        filename = f"{prefix}-{role}.bin"
        files[filename] = data
        images.append({"role": role, "offset": offset, "file": filename,
                       "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    if {image["role"] for image in images} != {"bootloader", "partition-table", "app"}:
        raise ValueError("Expected a bootloader, partition table, and application")

    # Resolve every license before touching an existing export or its archive.
    files.update(collect_firmware_licenses(project_root, build))

    voice_interrupt = bool(config.get("JUFF_VOICE_BARGE_IN"))
    aec_reference = ("es8311-digital" if board_id == "waveshare-lcd-3.5"
                     else "es7210-analog") if voice_interrupt else "none"
    manifest = {"schema_version": 1, "firmware_version": version, "board": board,
                "features": {"voice_interrupt": voice_interrupt,
                             "acoustic_echo_cancellation": voice_interrupt,
                             "aec_reference": aec_reference},
                "flash_settings": flasher["flash_settings"], "images": images}
    files["manifest.json"] = (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode()
    checksums = "".join(f"{image['sha256']}  {image['file']}\n" for image in images)
    files["SHA256SUMS"] = checksums.encode()
    # Preserve ESP-IDF's esptool arguments, compatible with its bundled esptool.
    flash_args = list(flasher["write_flash_args"])
    for image in images:
        flash_args.extend((image["offset"], image["file"]))
    files["flash_args"] = (shlex.join(flash_args) + "\n").encode()
    files["README.md"] = f"""# JUFF {version} · {board['name']}

此固件仅适用于 **{board['name']}**（`{board_id}`），屏幕为
{board['display']['controller']} {board['display']['width']}×{board['display']['height']}。
请核对设备板型；ESP32-S3 的 USB 标识无法区分不同屏幕型号。

在已安装 esptool 的 Python 环境中进入此目录执行，先将 `PORT` 替换为目标设备的实际串口：

```bash
python3 -m esptool --chip {board['chip']} --port PORT write_flash @flash_args
```

`manifest.json` 记录板型、版本、每个镜像的写入地址和 SHA-256。
macOS 可运行 `shasum -a 256 -c SHA256SUMS` 校验；Linux 使用 `sha256sum -c SHA256SUMS`。
首次覆盖其他固件前请完整备份目标设备 Flash。此包仅写入所列镜像，不包含 NVS
或个人配置；同板型 JUFF 升级保留现有 NVS，配网与令牌通过 USB 或加密 BLE 设置。

This package is only for **{board['name']}**. Replace `PORT` with the target
device's serial port. Back up its flash before replacing other firmware.
See `manifest.json` for hardware details, write offsets, and image checksums.
License and copyright texts are included in `LICENSE`, `THIRD_PARTY_NOTICES.md`
and `licenses/`. The latter directory also records linked archive provenance.
""".encode()

    output_root = project_root / "dist/firmware" / version
    output = output_root / board_id
    output.mkdir(parents=True, exist_ok=True)
    # Validate all inputs before replacing any previously exported package.
    for filename, data in files.items():
        target = output / filename
        target.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(dir=target.parent, delete=False) as temp:
            temp.write(data)
            temporary_path = Path(temp.name)
        temporary_path.replace(target)
    archive = output_root / f"{prefix}.zip"
    with tempfile.NamedTemporaryFile(dir=output_root, suffix=".zip", delete=False) as temp:
        temporary_archive = Path(temp.name)
    with zipfile.ZipFile(temporary_archive, "w", compression=zipfile.ZIP_DEFLATED) as bundle:
        for filename, data in sorted(files.items()):
            info = zipfile.ZipInfo(f"{board_id}/{filename}", date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            bundle.writestr(info, data)
    temporary_archive.replace(archive)
    return archive


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("board", choices=sorted(path.parent.name for path in
                                               (root / "firmware/boards").glob("*/board.json")))
    args = parser.parse_args()
    try:
        archive = package_firmware(root, args.board)
    except (OSError, KeyError, ValueError) as error:
        parser.exit(1, f"Cannot package firmware: {error}\nRun the matching board's build first.\n")
    print(f"Firmware package: {archive}")


if __name__ == "__main__":
    main()
