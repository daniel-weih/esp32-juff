# 固件说明

支持 Waveshare ESP32-S3-Touch-LCD-3.5 和 ESP32-S3-Touch-LCD-1.54，固件基于 ESP-IDF 5.5.x、`esp_codec_dev` 和 `esp_websocket_client`。

## 按硬件分版本

两个硬件版本独立配置和打包，使用相同的软件版本号和公共语音代码。

| 硬件 | 配置目录 | 构建并打包 |
| --- | --- | --- |
| 3.5 英寸（非 B 版） | `boards/waveshare-lcd-3.5/` | `make firmware-build-35` |
| 1.54 英寸 | `boards/waveshare-lcd-1.54/` | `make firmware-build-154` |

每个目录包含 `board.h`（显示／触摸引脚）、`sdkconfig.defaults`（板型选择）
和 `board.json`（固件包硬件清单）。音频 GPIO 默认值由 `main/Kconfig.projbuild`
按板型选择。`main/board_config.h` 只负责引入选中的板型，公共功能不分叉。

在仓库根目录执行 `make firmware-build-all` 可一次生成两版。
仅构建或直接使用 ESP-IDF 时，必须明确指定板型：

```bash
JUFF_BOARD=waveshare-lcd-1.54 ./scripts/idf.sh build
JUFF_BOARD=waveshare-lcd-1.54 ./scripts/idf.sh -p /dev/cu.usbmodemXXXX flash monitor
```

3.5 英寸板使用 `JUFF_BOARD=waveshare-lcd-3.5`。固件和 `sdkconfig` 保存在
`firmware/build/<板型>/`，默认值来自公共 `sdkconfig.defaults` 和
`boards/<板型>/sdkconfig.defaults`。未指定 `JUFF_BOARD` 会报错，不再读取原有
`firmware/build/` 与本地 `firmware/sdkconfig`。入口还会拒绝不匹配的已保存板型
及覆盖构建／配置目录的参数。切换硬件时选择对应板型，不要在另一板型的
`menuconfig` 中切换硬件；每块板的自定义参数留在各自配置目录中。

烧录和串口监视必须使用 `-p PORT` 或 `ESPPORT` 指定设备，避免自动选中另一块板。
也可运行 `make firmware-flash JUFF_BOARD=<板型> PORT=<串口>`。通用 USB 标识
只能识别 ESP32-S3，不能替代物理板型确认。

## 固件包

构建并打包命令在 `dist/firmware/<软件版本>/` 下生成：

```text
juff-waveshare-lcd-3.5-v<软件版本>.zip
juff-waveshare-lcd-1.54-v<软件版本>.zip
waveshare-lcd-3.5/
waveshare-lcd-1.54/
```

每个目录／压缩包包含带板型和版本名称的 bootloader、分区表、应用镜像，以及
`manifest.json`、`SHA256SUMS`、`flash_args`、烧录说明和许可证文件。打包会核对编译配置、
芯片目标和应用二进制内的硬件 ID；带 Wi-Fi 凭据或设备令牌的构建会拒绝打包。
设备配置通过 USB 或加密 BLE 写入 NVS。`manifest.json` 的
`features.voice_interrupt` 和 `acoustic_echo_cancellation` 记录该包是否启用实验功能，
`aec_reference` 区分大屏 `es8311-digital` 与小屏 `es7210-analog`；关闭时为 `none`。

包内的 `LICENSE`、`THIRD_PARTY_NOTICES.md` 和 `licenses/` 保留项目、组件与
目标运行库的许可及版权文本。打包依据应用和引导程序的链接记录收集，需保留
对应的 ESP-IDF、组件和完整工具链目录。缺少许可或来源不匹配会在覆盖旧包前
报错；补充文本的来源见 [licenses/README.md](../licenses/README.md)。

两版都会在 CI 中编译并分别保存固件包。生成目录 `dist/` 与 `firmware/build/`
不进入 Git。新增硬件时增加独立板型目录、Kconfig 选项和构建／CI 入口，并保留
现有板型。验证结果应区分编译通过与实机验证。

首次覆盖设备固件前，应完整备份 Flash。`backups/` 已被 Git 忽略；备份包含
原固件和设备配置，应仅在本机保存。恢复时使用对应设备的完整备份，从 Flash
地址 `0x0` 写入。不要把一块板的备份或固件刷到另一种板型。

## 1.54 英寸板

| 功能 | 配置 |
| --- | --- |
| LCD | ST7789，240×240，SPI3，模式 3，40 MHz，RGB565，颜色反转 |
| LCD 引脚 | MOSI 39、CLK 38、DC 45、CS 21、RST 40、背光 46 |
| 触摸 | CST816，I²C `0x15`，RST 47；关闭自动休眠以读取按下／松开事件 |
| 共享 I²C | SCL 41、SDA 42 |
| 音频 I²S | MCLK 8、BCLK 9、WS 10、DIN 11、DOUT 12 |
| 音频器件 | ES7210 ADC `0x40`、ES8311 DAC `0x18`、功放使能 GPIO 7 |
| 电源／按键 | GPIO 2 保持电源使能；BOOT/GPIO 0 打断语音 |

ES7210 使用四槽 TDM，普通构建只采集槽 0 的麦克风，输出为 24 kHz 单声道。
两款硬件的普通构建均保持半双工，通过屏幕或 BOOT/GPIO 0 手动停止播放。

`CONFIG_JUFF_VOICE_BARGE_IN` 是两款硬件各自可启用的实验开关。仓库默认配置
对两款板均关闭此功能；已保存的 `sdkconfig` 会保留此前选择，不能据此判断默认值。
需要试验时，在仓库根目录运行：

```bash
JUFF_BOARD=waveshare-lcd-1.54 ./scripts/idf.sh menuconfig
```

在 `Juff voice terminal > Experimental speech interruption during playback`
启用选项并保存，再运行 `make firmware-build-154`。生成包的 `manifest.json`
通过 `features.voice_interrupt` 声明实际开关值；软件版本号不代表开关状态。

历史 `0.6.0-dev` 实验出现过自行误停及未响应近端人声的失败；一次校准后的
声学冒烟测试通过，但轻声、远距离、长时间运行和首字保真等仍未验收。
`0.6.1-dev` 仅更新界面，没有新增声学验证。小屏测试方法、数据和限制集中记录在
[语音打断验证记录](../docs/voice-interruption-validation.md)，不应将一次冒烟结果
理解为可靠性验收或默认启用依据。

实验实现使用 ESP-SR 2.5.3 的 `AEC_MODE_FD_HIGH_PERF`、`AEC_NLP_LEVEL_NORMAL`
和 WebRTC VAD。按槽 0 为麦克风、槽 1 为参考（物理 MIC3）采集 24 kHz 双通道，
再以相同采样位置重采样至 16 kHz。DAC 回采支路位于功放之前，电阻网络约衰减
24 dB。`CONFIG_JUFF_CODEC_REFERENCE_GAIN_DB` 单独控制 MIC3：默认 24 dB；
编译时 `CONFIG_JUFF_CODEC_VOLUME > 85` 则默认 21 dB，以增加高音量余量。
电气测量确认了回采通路，但最大音量仍可能出现波形失真；没有 ADC 削顶不能
保证参考质量。参考信号仅供本地处理，不作为麦克风音频上传。

播放期间持续运行本地检测，触发后先停止回答，再恢复上行并补发 300 ms 历史
语音及恢复前的新帧。有效参考出现后暖机 400 ms；触发需连续 120 ms 满足语音
判定及 `max(180 RMS, 3 × 环境噪声 RMS)` 门限。`input.suspend` 关闭检测并清除
缓存。屏幕和 BOOT/GPIO 0 手动停止始终发送到当前活动传输链路。

此板使用双 240×24 行 DMA 绘图缓冲、64 KB 数据缓存及 64 字节缓存行。NimBLE
使用 `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL`；前端历史和待发送缓存共
25,600 字节分配到 PSRAM，DMA 缓冲、AEC 工作帧和任务栈保留在内部内存。
前端初始化失败会释放部分分配并退回普通半双工。

引脚与驱动依据 [Waveshare 官方资料](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.54)
及其 [原理图](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.54/ESP32-S3-LCD-1.54-Schematic.pdf)，
并与 [小智板型配置](https://github.com/78/xiaozhi-esp32/blob/main/main/boards/waveshare/esp32-s3-touch-lcd-1.54/config.h)
交叉核对。

## 3.5 英寸板：屏幕与触摸

- ST7796：320×480 竖屏，MOSI GPIO 1、CLK GPIO 5、DC GPIO 3
- LCD 复位：TCA9554 `0x20` 的 `EXIO1`
- 背光：GPIO 6，5 kHz PWM，界面可切换 100% / 65% / 30%
- FT6336：共享板载 I²C，总线地址 `0x38`
- LVGL 8.4：双 320×24 行 DMA 绘图缓冲，RGB565 字节交换已启用

## 界面与交互

首页使用暖纸色背景、墨绿色抽象表情和珊瑚色点缀。离线、就绪、倾听、思考、说话、麦克风暂停和异常各有神态，带短暂眨眼、轻微移动和说话嘴形变化。`companion_face.c` 用 LVGL 线条和圆角图形等比绘制，适配 240×240 和 320×480，不增加位图、字体或显示缓冲区。

活动语音轮次中可轻触表情或底部停止／取消按钮打断；临时提示不会隐藏停止操作。待机时底部按钮打开连接页，顶部蓝牙按钮始终可用，右侧按钮切换亮度。配对页保留六位码、倒计时、取消和记住多台 Mac 的功能。音频自检保留在 BLE/WebUI 命令中。3.5 英寸板的 TCA9554 LCD 复位与功放输出仍使用同一互斥保护和输出影子寄存器，避免并发操作覆盖其他 EXIO 位。

## 3.5 英寸板：板载音频

- I²C：SCL GPIO 7、SDA GPIO 8
- I²S：MCLK 12、BCLK 13、DIN 14、WS 15、DOUT 16
- ES8311 地址：`0x18`
- NS4150B 功放：TCA9554 `0x20` 的 `EXIO7`
- Codec：24 kHz / PCM16 / mono
- 上行：24 kHz 每 2400 样本线性重采样为 16 kHz 每 1600 样本

`0.6.2-dev` 起，大屏可在自己的配置中启用实验 AEC 和语音打断：

```bash
JUFF_BOARD=waveshare-lcd-3.5 ./scripts/idf.sh menuconfig
make firmware-build-35
```

在 `Juff voice terminal > Experimental speech interruption during playback`
启用并保存。大屏启用 ES8311 内部数字反馈，24 kHz 双槽同时采集左槽麦克风和
右槽播放参考。播放及自检将单声道 PCM 复制到左右两槽，保持播放速度和参考
一致，再将采集的两路同步重采样送入 ESP-SR。大屏不使用 ES7210 的参考增益。
算法初始化失败时，仍只提取麦克风槽恢复普通半双工，参考槽不会上传。

大屏使用 `AEC_NLP_LEVEL_VERYAGGR` 抑制残余回声，小屏保持 `NORMAL`。大屏 AEC
配置的麦克风增益默认值为 24 dB：30 dB 在自动播放测试中出现麦克风 PCM 满幅。
此驱动设置控制 ES8311 的 ADC 数字缩放，并不改变模拟 PGA；没有 PCM 满幅
也不能排除前级失真。
已有 `sdkconfig` 会保留原值，升级时需要核对 `Microphone gain (dB)`；调整增益
或扬声器音量后应同时检查 `mic_fullscale` 和近端人声打断。

大屏使用 64 KB 数据缓存、64 字节缓存行和外部 NimBLE 内存，为显示 DMA、DSP
工作缓冲和任务栈保留内部 RAM。数字参考位于 DAC 音量／静音之前，不能代表
功放实际发声，也不包含模拟失真；检测仍受播放状态、功放状态和麦克风暂停控制。
电气和声学结果见[大屏 AEC 记录](../docs/waveshare-lcd-3.5-aec-feasibility.md)。

两款硬件的 `CONFIG_JUFF_AUDIO_STARTUP_SELF_TEST` 均默认关闭，正常启动不播放
诊断音。需要检查音频通路时，可显式发送 BLE/WebUI 的 `audio_test` 命令；
该自检会丢弃 ADC 瞬态、测量环境值并播放 660/880 Hz 双音，再判断声学增量。

## USB 配网

USB Serial/JTAG 始终运行一个不回显秘密的 JSON 配网端点。使用：

```bash
python3 -m venv .venv
.venv/bin/pip install pyserial
.venv/bin/python scripts/provision_wifi.py
```

SSID、密码、Bridge URI 和设备令牌会写入 `juff` NVS 命名空间；重新配网会覆盖旧值并重启。固件常量只作为 NVS 尚未配置时的回退值。

## BLE 语音、配网、控制与状态

NimBLE 外设广播名为 `JUFF-XXXX`，自定义服务 UUID 为 `B8A5FCA1-8A0F-4DB1-9C6C-7B5B6C49A001`。Mac 写入控制特征前必须完成 LE Secure Connections + MITM 配对，随机 6 位码只显示在板载屏幕上，bonding key 持久化到 NVS。

自定义服务包含控制、状态、麦克风和扬声器 4 个特征。控制特征接收不超过 512 字节的换行分隔 JSON，可执行 `status`、`interrupt`、`audio_test`、`brightness` 和可选的 `provision`。状态特征支持 read/notify，周期报告 Wi-Fi、BLE、Bridge、Qwen voice、音频硬件和播放状态。

纯 BLE 模式无需配置 Wi-Fi。麦克风将 16 kHz PCM16 编码为 G.711 μ-law，以 100 ms / 1600 字节一帧通知 Mac；扬声器接收 Mac 发来的 24 kHz G.711 μ-law 并解码为 PCM16。每段回答以 `audio.begin` 开始，固件先暂停麦克风上行；`playback.ended` 或取消后恢复采集，以避免 BLE 上下行争用。

## Bridge 协议

连接后设备先发送：

```json
{
  "type": "hello",
  "token": "...",
  "deviceId": "esp32s3-XXXXXXXXXXXX",
  "audioInputEnabled": true,
  "audioOutputEnabled": true,
  "inputSampleRate": 16000,
  "outputSampleRate": 24000
}
```

之后直接上传 PCM16 little-endian 二进制帧。下行每段回复依次为 `audio.begin`、若干二进制 PCM 分片和 `audio.done`；设备回报 `playback.started/ended/cancelled`。`playback.clear` 会清空队列并关闭功放。

Wi-Fi Bridge 使用原始 PCM16；BLE 使用 G.711 μ-law。两条链路在扬声器播放期间
均暂停麦克风上行，RX DMA 继续工作。两款硬件默认保持半双工和手动停止。
两款硬件的实验打断也采用停止后才恢复上行的设计，云端不会在播放期间收到连续麦克风音频。
