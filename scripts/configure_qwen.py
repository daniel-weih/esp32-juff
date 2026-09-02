#!/usr/bin/env python3
"""Store the DashScope API key in Qwen Audio Agent's local config."""

from __future__ import annotations

import argparse
import getpass
import html
import os
from pathlib import Path
import subprocess
import sys


DEFAULT_CONFIG = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "qwaudio" / "config.env"
KEY_NAME = "DASHSCOPE_API_KEY"


def normalize_api_key(value: str) -> str:
    api_key = html.unescape(value).strip()
    if len(api_key) >= 2 and api_key[0] == api_key[-1] == "`":
        api_key = api_key[1:-1].strip()
    # Accept a key copied from Markdown where an underscore was escaped.
    api_key = api_key.replace(r"\_", "_")
    if not api_key or any(character.isspace() for character in api_key):
        raise ValueError("API Key 不能为空或包含空白字符。")
    if not api_key.startswith("sk-"):
        raise ValueError("剪贴板内容不像 DashScope API Key（应以 sk- 开头）。")
    return api_key


def update_config(path: Path, api_key: str) -> None:
    api_key = normalize_api_key(api_key)

    lines = path.read_text(encoding="utf-8").splitlines() if path.exists() else []
    replacement = f"{KEY_NAME}={api_key}"
    updated: list[str] = []
    replaced = False
    for line in lines:
        if line.startswith(f"{KEY_NAME}="):
            if not replaced:
                updated.append(replacement)
                replaced = True
        else:
            updated.append(line)
    if not replaced:
        updated.append(replacement)

    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp")
    try:
        temporary.write_text("\n".join(updated) + "\n", encoding="utf-8")
        os.chmod(temporary, 0o600)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument(
        "--clipboard",
        action="store_true",
        help="从 macOS 剪贴板读取 Key，且不在终端或进程参数中暴露",
    )
    args = parser.parse_args()
    if args.clipboard:
        clipboard = subprocess.run(
            ["/usr/bin/pbpaste"],
            check=True,
            capture_output=True,
            text=True,
        )
        api_key = clipboard.stdout
    else:
        api_key = getpass.getpass("DashScope API Key（输入不会显示）: ")
    update_config(args.config.expanduser(), api_key)
    print(f"已安全写入 {args.config.expanduser()}（权限 0600）。")


if __name__ == "__main__":
    try:
        main()
    except (KeyboardInterrupt, EOFError):
        print("\n已取消。", file=sys.stderr)
        raise SystemExit(130)
    except ValueError as error:
        raise SystemExit(str(error)) from error
