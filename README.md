# JUFF：ESP32-S3 × Qwen 实时语音伴侣

[English](README.en.md) · [架构](docs/architecture.md) · [固件说明](firmware/README.md) · [参与贡献](CONTRIBUTING.md)

| 3.5 英寸 · 320×480 | 1.54 英寸 · 240×240 |
| :---: | :---: |
| <img src="docs/images/juff-ui-3.5.png" alt="JUFF 大屏固件界面" width="320"> | <img src="docs/images/juff-ui-1.54.png" alt="JUFF 小屏固件界面" width="240"> |

*由当前固件 UI 离屏渲染，按两款屏幕的原生分辨率展示。*

JUFF 把 Waveshare **ESP32-S3-Touch-LCD-3.5 / 1.54** 变成一个带屏幕、触摸、
麦克风和扬声器的实时语音终端。默认链路是加密 Bluetooth LE：
ESP32 将麦克风音频送到 Mac，Mac 上的
`qwen-audio-agent` 连接 DashScope，再把流式语音回答送回设备播放。

Wi-Fi/WebSocket 是可选的备用链路。DashScope API Key 始终留在 Mac，
不会写入 ESP32。

> 项目目前处于硬件原型阶段，请按实际板型选择固件配置。它不是阿里云、
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

仓库默认配置下，两款硬件均关闭本地 AEC/VAD 语音打断，采用半双工：播放
回答时暂停上传麦克风，使用屏幕或 GPIO 0 按钮手动停止。两款硬件均可按
[固件说明](firmware/README.md) 显式开启实验性语音打断；声学验证尚不完整。
固件包 `manifest.json` 中的 `features.voice_interrupt` 和 `aec_reference`
记录实际开关与该硬件的参考通路。

[语音打断验证记录](docs/voice-interruption-validation.md) 保留了 `0.6.0-dev`
的失败结果及一次校准声源冒烟测试通过结果，尚不构成声学验收。
`0.6.2-dev` 新增大屏 ES8311 内部数字参考适配；当前开发版本为 `0.6.3-dev`，
已清理临时显示诊断代码。大屏黑屏问题尚未得到实物恢复确认，先前声学结果
不代表显示验收，见 [大屏 AEC 记录](docs/waveshare-lcd-3.5-aec-feasibility.md)。
完整设计见 [docs/architecture.md](docs/architecture.md)。

## 支持的硬件

| 板型 | 显示与触摸 | 音频 |
| --- | --- | --- |
| ESP32-S3-Touch-LCD-3.5 | ST7796，320×480，FT6336 | ES8311 输入／输出，TCA9554 控制功放 |
| ESP32-S3-Touch-LCD-1.54 | ST7789，240×240，CST816 | ES7210 麦克风输入，ES8311 输出，GPIO 7 控制功放 |

两款均使用 ESP32-S3、16 MB Flash、8 MB Octal PSRAM。1.54 英寸版提供紧凑
首页和配对页，保留语音、打断、亮度控制与蓝牙配对功能。

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

```bash
git clone https://github.com/daniel-weih/esp32-juff.git juff
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
make firmware-build-35     # 3.5 英寸版：构建并打包
make firmware-build-154    # 1.54 英寸版：构建并打包
# 或一次生成两个版本
make firmware-build-all
```

两种硬件分别维护配置，公共语音功能共用代码。固件包按硬件和软件版本命名：

| 硬件版本 | 板型 ID | 固件包（位于 `dist/firmware/<软件版本>/`） |
| --- | --- | --- |
| 3.5 英寸 | `waveshare-lcd-3.5` | `juff-waveshare-lcd-3.5-v<软件版本>.zip` |
| 1.54 英寸 | `waveshare-lcd-1.54` | `juff-waveshare-lcd-1.54-v<软件版本>.zip` |

烧录时必须明确指定板型和串口，例如烧录 1.54 英寸板：

```bash
make firmware-flash JUFF_BOARD=waveshare-lcd-1.54 PORT=/dev/cu.usbmodemXXXX
```

3.5 英寸板使用 `JUFF_BOARD=waveshare-lcd-3.5`。构建目录分别为
`firmware/build/<板型>/`，不复用另一板型的 GPIO 或本地 `firmware/sdkconfig`。
通用命令 `make firmware-build` 也必须指定 `JUFF_BOARD`；未指定时停止执行。
USB 的通用 ESP32 标识无法区分这两块板，因此烧录入口不自动选择板型或串口。
每个固件包包含烧录说明、板型清单和 SHA-256 校验值；CI 也分别构建并保存两版。

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

多台 JUFF 在附近时，可指定要使用的设备，例如
`JUFF_BLE_DEVICE=JUFF-XXXX make start`。将 `JUFF-XXXX` 替换为板载蓝牙连接页
显示的设备名称。

| 命令 | 用途 |
| --- | --- |
| `make setup` | 初始化一台新 Mac |
| `make doctor` | 检查 Node、Swift、配置和设备 |
| `make start` | 启动 Gateway、WebUI、BLE companion 和备用 Bridge |
| `make test` | 仓库检查、固件工具与 Node 测试、JuffBLE 自检 |
| `make firmware-build-35` / `make firmware-build-154` | 构建并打包对应硬件版本 |
| `make firmware-build-all` | 构建并打包两个硬件版本 |
| `make firmware-build JUFF_BOARD=<板型>` | 仅构建指定硬件版本 |
| `make firmware-flash JUFF_BOARD=<板型> PORT=<串口>` | 烧录指定设备并打开串口监视器 |

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
