# JUFF：ESP32-S3 × Qwen 实时语音伴侣

[English](README.en.md) · [架构](docs/architecture.md) · [固件说明](firmware/README.md) · [参与贡献](CONTRIBUTING.md)

JUFF 把 Waveshare **ESP32-S3-Touch-LCD-3.5** 变成一个带屏幕、触摸、
麦克风和扬声器的实时语音终端。默认链路是加密 Bluetooth LE：
ESP32 将麦克风音频送到 Mac，Mac 上的
`qwen-audio-agent` 连接 DashScope，再把流式语音回答送回设备播放。

Wi-Fi/WebSocket 是可选的备用链路。DashScope API Key 始终留在 Mac，
不会写入 ESP32。

> 项目目前处于硬件原型阶段，目标板型和引脚固定。它不是阿里云、
> Qwen 或 Waveshare 的官方项目。

## 工作方式

```text
ESP32 麦克风
    │  16 kHz G.711 μ-law
    ▼
加密 BLE ──► Mac JuffBLE ──► Qwen Audio Agent ──► DashScope
    ▲                                  │
    │       24 kHz G.711 μ-law         │
    └──────── ESP32 扬声器 ◄───────────┘
```

默认是半双工语音：JUFF 播放回答时暂停上传麦克风，屏幕或 GPIO 0 按钮
仍可立即打断。完整设计见 [docs/architecture.md](docs/architecture.md)。

## 支持的硬件

- Waveshare ESP32-S3-Touch-LCD-3.5
- ESP32-S3、16 MB Flash、8 MB Octal PSRAM
- ST7796 320×480 LCD、FT6336 触摸
- ES8311 Codec、板载麦克风与 NS4150B 功放

外观相似的 Touch-AMOLED-1.8 使用不同的屏幕和引脚，不能直接刷入本固件。

## 已刷好固件的设备：新 Mac 五分钟启动

### 准备

- 一台 Mac，以及可用的蓝牙
- Xcode Command Line Tools（提供 Swift 编译器）
- Node.js `22.22.2+`、`24.15.0+` 或 `26+`；推荐读取项目
  [`.nvmrc`](.nvmrc) 的版本
- DashScope API Key

首次安装 Xcode Command Line Tools：

```bash
xcode-select --install
```

### 1. 克隆并初始化

仓库发布后，将 `<repository-url>` 替换成实际地址：

```bash
git clone <repository-url> juff
cd juff
./scripts/bootstrap_macos.sh
```

初始化脚本会：

1. 用 `npm ci` 安装锁定版本的依赖；
2. 创建权限为私有的 `host/.env` 和随机设备令牌；
3. 编译并签名原生 JuffBLE companion；
4. 以隐藏输入方式保存 DashScope Key。

它不会烧录 ESP32，也不会覆盖已有的本地配置。

### 2. 配对 ESP32

1. 在 JUFF 首页轻触右上角蓝牙胶囊。
2. 选择 **PAIR A NEW MAC**，设备进入 120 秒可发现窗口。
3. 在 Mac 的配对弹窗中输入设备屏幕显示的 6 位码。
4. JUFF 显示 **New Mac is ready** 后即完成。

ESP32 最多保存两台 Mac 的 bonding 信息，但同一时刻只有一台 Mac 使用
语音链路。

### 3. 启动

```bash
make start
```

随后打开 <http://127.0.0.1:3101/> 查看 Qwen Audio Agent WebUI。
所有 Mac 服务都在当前终端前台运行，按 `Ctrl-C` 会一起停止。

遇到问题时先运行：

```bash
make doctor
```

## 从空白开发板开始

ESP-IDF 只在构建或烧录固件时需要，不是日常语音使用的依赖。

```bash
make firmware-setup
make firmware-build
./scripts/idf.sh -p /dev/cu.usbmodemXXXX flash monitor
```

请将 `/dev/cu.usbmodemXXXX` 换成当前 Mac 上实际出现的串口。首次刷机前
建议自行备份原厂 Flash；备份可能包含设备数据且可能受厂商许可限制，
因此 `backups/` 永远不会进入本仓库。

ESP-IDF 项目固定使用 5.5.x。更多硬件、自检和引脚资料见
[`firmware/README.md`](firmware/README.md)。

## 可选 Wi-Fi 备用链路

纯 BLE 语音不需要 Wi-Fi。默认 Bridge 只监听 `127.0.0.1`，不会暴露到
局域网。需要启用 Wi-Fi 时：

1. 将 `host/.env` 中 `JUFF_BRIDGE_HOST` 改为 `0.0.0.0`；
2. 保留初始化脚本生成的随机 `JUFF_DEVICE_TOKEN`；
3. 运行 `python3 scripts/provision_ble.py`，通过已加密 BLE 写入 Wi-Fi
   和 Bridge 配置。

也可以通过 USB 配置；该方式额外需要 `pyserial`：

```bash
python3 -m venv .venv
.venv/bin/pip install pyserial
.venv/bin/python scripts/provision_wifi.py
```

## 常用命令

| 命令 | 用途 |
| --- | --- |
| `make setup` | 初始化一台新 Mac |
| `make doctor` | 检查 Node、Swift、配置和设备 |
| `make start` | 启动 Gateway、WebUI、BLE companion 和备用 Bridge |
| `make test` | 仓库检查、Node 测试和 JuffBLE 自检 |
| `make firmware-build` | 构建 ESP32 固件 |
| `make firmware-flash` | 烧录并打开串口监视器 |

## 仓库结构

```text
firmware/   ESP-IDF 固件、显示、音频、BLE 和 Wi-Fi 客户端
host/       Node.js 主控、Qwen Gateway 连接和 WebSocket Bridge
macos/      原生 CoreBluetooth companion
scripts/    初始化、诊断、构建、配网和仓库检查
docs/       架构与协议文档
.github/    CI、依赖更新和 Issue/PR 模板
```

## 密钥与隐私

- DashScope Key 默认写在 `~/.config/qwaudio/config.env`（或
  `$XDG_CONFIG_HOME/qwaudio/config.env`），不在仓库内。
- `host/.env`、`firmware/sdkconfig`、NVS/Flash 备份和全部构建输出均被忽略。
- Gateway/WebUI 默认只监听本机。
- 将 Bridge 暴露到局域网前必须使用随机设备令牌。
- 不要把真实 API Key、Wi-Fi 密码、配对码或日志中的凭据提交到 Issue。

发布前可运行 `./scripts/check_repository.sh` 做最小敏感信息与可移植性检查。
安全问题请按 [SECURITY.md](SECURITY.md) 私下报告。

## 开发与贡献

```bash
./scripts/check_repository.sh
cd host && npm test
./scripts/build_macos_ble.sh
./macos/build/JuffBLE.app/Contents/MacOS/JuffBLE --self-test
```

提交 Pull Request 前请阅读 [CONTRIBUTING.md](CONTRIBUTING.md) 和
[CHANGELOG.md](CHANGELOG.md)。

## 许可证

项目代码与文档采用 [Apache License 2.0](LICENSE)。第三方组件按各自许可证
发布；使用 DashScope 需要用户自己的阿里云账户，并受其服务条款和计费规则
约束。
