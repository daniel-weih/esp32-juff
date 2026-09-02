#!/usr/bin/env python3
"""Provision Juff Wi-Fi and Bridge settings over encrypted Bluetooth LE."""

from __future__ import annotations

import argparse
import getpass
import json
from pathlib import Path
import selectors
import socket
import subprocess
import sys
import time


PROJECT_ROOT = Path(__file__).resolve().parents[1]
BUILD_SCRIPT = PROJECT_ROOT / "scripts" / "build_macos_ble.sh"
BLE_EXECUTABLE = (
    PROJECT_ROOT / "macos" / "build" / "JuffBLE.app" / "Contents" / "MacOS" / "JuffBLE"
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


def ensure_companion() -> None:
    if BLE_EXECUTABLE.exists():
        return
    print("正在构建 Mac 蓝牙 companion……")
    subprocess.run([str(BUILD_SCRIPT)], cwd=PROJECT_ROOT, check=True)


def provision(payload: dict[str, str], device: str | None) -> None:
    ensure_companion()
    command = [str(BLE_EXECUTABLE), "--exit-after-ack", "provision"]
    if device:
        command.extend(["--device", device])

    print("正在扫描 JUFF 蓝牙设备……")
    print("首次连接时，请把 ESP32 屏幕上的 6 位配对码输入 Mac 弹窗。")
    process = subprocess.Popen(
        command,
        cwd=PROJECT_ROOT,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    assert process.stdin is not None
    assert process.stdout is not None
    process.stdin.write(json.dumps(payload, ensure_ascii=False, separators=(",", ":")) + "\n")
    process.stdin.flush()

    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    deadline = time.monotonic() + 90
    acknowledged = False
    try:
        while time.monotonic() < deadline:
            if process.poll() is not None:
                break
            for key, _ in selector.select(timeout=0.25):
                line = key.fileobj.readline().strip()
                if not line:
                    continue
                if line.startswith("STATE "):
                    print("蓝牙：" + line.removeprefix("STATE "))
                    continue
                if not line.startswith("DEVICE "):
                    continue
                try:
                    message = json.loads(line.removeprefix("DEVICE "))
                except json.JSONDecodeError:
                    continue
                if message.get("type") != "ack" or message.get("request") != "provision":
                    continue
                if message.get("ok") is True:
                    acknowledged = True
                    break
                raise SystemExit(f"ESP32 拒绝蓝牙配网：{message.get('error', 'unknown')}")
            if acknowledged:
                break
    finally:
        selector.close()
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()

    if not acknowledged:
        raise SystemExit("ESP32 未确认蓝牙配网；请确认蓝牙已开启并查看板上配对码。")
    print("蓝牙配网成功。ESP32 已保存配置并正在切换到 Wi-Fi 语音链路。")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", help="指定设备名，例如 JUFF-A1B2")
    parser.add_argument("--ssid", help="Wi-Fi SSID；省略时交互输入")
    parser.add_argument("--bridge-host", help="运行 Bridge 的本机局域网 IP")
    parser.add_argument("--bridge-uri", help="高级用法：直接指定完整 WebSocket URI")
    args = parser.parse_args()

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
    print(f"ESP32 将通过 Wi-Fi 连接 Bridge：{bridge_uri}")
    provision(
        {
            "type": "provision",
            "ssid": ssid,
            "password": password,
            "bridgeUri": bridge_uri,
            "deviceToken": host_env["JUFF_DEVICE_TOKEN"],
        },
        args.device,
    )


if __name__ == "__main__":
    try:
        main()
    except (KeyboardInterrupt, EOFError):
        print("\n已取消。", file=sys.stderr)
        raise SystemExit(130)
    except subprocess.CalledProcessError as error:
        raise SystemExit(f"Mac 蓝牙 companion 构建失败：{error}") from error
