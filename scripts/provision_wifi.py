#!/usr/bin/env python3
"""Provision Wi-Fi credentials to Juff over the local USB serial link."""

from __future__ import annotations

import argparse
import getpass
import glob
import json
from pathlib import Path
import socket
import sys
import time

import serial


PROJECT_ROOT = Path(__file__).resolve().parents[1]


def default_port() -> str:
    candidates = sorted(glob.glob("/dev/cu.usbmodem*"))
    if len(candidates) == 1:
        return candidates[0]
    if not candidates:
        raise SystemExit("未找到 /dev/cu.usbmodem*；请重新插入 ESP32。")
    raise SystemExit(
        "发现多个 USB 串口，请用 --port 指定：\n  " + "\n  ".join(candidates)
    )


def validate(ssid: str, password: str) -> None:
    if not ssid or len(ssid.encode("utf-8")) > 31:
        raise SystemExit("Wi-Fi SSID 必须是 1–31 个 UTF-8 字节。")
    if len(password.encode("utf-8")) > 63:
        raise SystemExit("Wi-Fi 密码不能超过 63 个 UTF-8 字节。")


def read_host_env() -> dict[str, str]:
    env_path = PROJECT_ROOT / "host" / ".env"
    if not env_path.exists():
        raise SystemExit(f"缺少 {env_path}；请先准备主机 Bridge 配置。")
    result: dict[str, str] = {}
    for raw_line in env_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        result[key.strip()] = value.strip()
    token = result.get("JUFF_DEVICE_TOKEN", "")
    if len(token) < 16:
        raise SystemExit("host/.env 中 JUFF_DEVICE_TOKEN 必须至少 16 字符。")
    return result


def detect_bridge_host() -> str:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.connect(("192.0.2.1", 9))
            address = probe.getsockname()[0]
            if address and not address.startswith("127."):
                return address
    except OSError:
        pass
    return ""


def provision(
    port: str,
    ssid: str,
    password: str,
    bridge_uri: str,
    device_token: str,
) -> None:
    payload = (
        json.dumps(
            {
                "type": "juff.provision.v1",
                "ssid": ssid,
                "password": password,
                "bridgeUri": bridge_uri,
                "deviceToken": device_token,
            },
            ensure_ascii=False,
            separators=(",", ":"),
        ).encode("utf-8")
        + b"\n"
    )

    print(f"正在通过 {port} 等待 ESP32 配网通道……")
    started = time.monotonic()
    sent = False
    acknowledged = False
    try:
        with serial.Serial(port, 115200, timeout=0.2, write_timeout=2) as device:
            device.dtr = False
            device.rts = False
            deadline = started + 15
            while time.monotonic() < deadline:
                raw = device.readline()
                line = raw.decode("utf-8", errors="replace").strip()
                if not sent and (
                    "JUFF_PROVISION_READY" in line
                    or time.monotonic() - started >= 4
                ):
                    device.write(payload)
                    device.flush()
                    sent = True
                if "JUFF_PROVISION_OK" in line:
                    acknowledged = True
                    break
                if "JUFF_PROVISION_ERROR" in line:
                    raise SystemExit(f"ESP32 拒绝配网：{line}")
    except serial.SerialException as error:
        if not acknowledged:
            raise SystemExit(f"USB 串口通信失败：{error}") from error

    if not acknowledged:
        raise SystemExit("ESP32 未确认配网；请重插设备后再试。")
    print("配网成功。ESP32 已安全保存凭据并正在重启。")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="ESP32 USB 串口；默认自动识别")
    parser.add_argument("--ssid", help="Wi-Fi SSID；省略时交互输入")
    parser.add_argument("--bridge-host", help="运行 Bridge 的本机局域网 IP；默认自动识别")
    parser.add_argument("--bridge-uri", help="高级用法：直接指定完整 WebSocket URI")
    args = parser.parse_args()

    port = args.port or default_port()
    ssid = args.ssid if args.ssid is not None else input("Wi-Fi SSID: ").strip()
    password = getpass.getpass("Wi-Fi 密码（输入不会显示，开放网络可留空）: ")
    validate(ssid, password)
    host_env = read_host_env()
    bridge_uri = args.bridge_uri
    if bridge_uri is None:
        bridge_host = args.bridge_host or detect_bridge_host()
        if not bridge_host:
            bridge_host = input("运行 Bridge 的本机局域网 IP: ").strip()
        bridge_port = int(host_env.get("JUFF_BRIDGE_PORT", "8765"))
        device_path = host_env.get("JUFF_DEVICE_PATH", "/device")
        bridge_uri = f"ws://{bridge_host}:{bridge_port}{device_path}"
    print(f"ESP32 将连接 Bridge：{bridge_uri}")
    provision(port, ssid, password, bridge_uri, host_env["JUFF_DEVICE_TOKEN"])


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n已取消。", file=sys.stderr)
        raise SystemExit(130)
