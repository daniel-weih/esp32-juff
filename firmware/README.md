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
| 电池检测 | GPIO 1 / ADC1_CH0，200kΩ / 100kΩ 分压（校准值 ×3）；GPIO 3 低电平表示充电 |
| 电源／按键 | PWR/GPIO 5 低电平按下；GPIO 2 保持电池供电；BOOT/GPIO 0 打断语音 |

`0.6.4-dev` 起，PWR 长按约 3 秒后显示睡觉表情和 `Release to power off`，
松手即可关机；短按不会关机。仅电池供电时释放 GPIO 2 电源保持，
USB 插着时关闭屏幕、音频与无线并进入深度睡眠，充电电路仍可工作。
再次按 PWR 开机；开机时持续按住不会被误认为关机，必须先松手才重新计时。
PLUS/GPIO 4 暂未分配功能，BOOT 保留语音打断。此功能只作用于 1.54 英寸板。

PWR 同时直接控制电池开关，所以固件等待按键稳定释放后才断开电池保持。
USB 经独立支路给 VSYS 供电，无法用 GPIO 2 切断；深睡仅以 PWR/GPIO 5 低电平
唤醒，没有自动唤醒定时器。深睡仍有板载电路耗电，不等于 USB 电源完全断开。

`0.6.8-dev` 为两种硬件加入有界音频缓冲：设备在 PSRAM 中最多暂存 192,000 字节
PCM（24 kHz、16-bit 单声道约 4 秒），首次播放前预存 300 ms。短回答收到结束标记
就开始播放；发生断流后先回填 250 ms，等待最多 600 ms。Mac 根据设备上报的
剩余空间发送，合成超前的部分在 Mac 排队（μ-law 数据总上限 2 MiB），不再使用
新固件上的固定 25,200 B/s 限速。持续低于播放需求的链路仍可能耗尽缓冲。

BLE `status` 中 `speakerFlow: 1` 表示支持该协议。Mac 仅对支持设备启用额度与
带流编号、字节偏移的音频包；旧固件继续使用原协议。`speaker.credit` 上报
`received`、`limit`（均为不含包头的 μ-law 字节数）、`queuedMs`、`underruns`
和 `dropped`。语音打断会清除两端待播数据，旧流的迟到数据不会进入下一回答。
这些计数是应用层收发和排队信息，不能视为蓝牙硬件的最高吞吐量。

`0.6.10-dev` 将额度快照与通知发送串行化，避免并发通知乱序导致 Mac 误判并
清空播放。Wi-Fi 的 4096 字节接收块在缓冲满时等待播放释放空间，空间等待
预算为 200 ms；失败则取消当前回答并忽略后续音频，直到下一个有效 BEGIN。
等待中的旧音频不能进入或取消新回答；BLE 继续按接收额度发送。

同版固件在麦克风流切换或丢弃半帧后，先可靠排入 `microphone.reset` 通知，
再发送新音频。Mac 收到通知会清空组帧残留。此修复需要同时升级固件与 Mac
BLE 程序；新版 Mac 仍可接收旧固件原有的连续 μ-law 数据。

小屏 `0.6.8-dev` 自动回放验证：一次性送入 8 秒测试音，设备缓冲峰值约
3.77 秒，实际播放持续约 8.04 秒；另一轮在已收到前 4 秒后暂停供数 1.5 秒，
8 秒音频仍连续完成。两轮及短回答、空回答、缓冲中取消后开始新回答的测试均
未增加设备断流或丢数据计数。测试时协商为 2M PHY；首秒应用层接收速率约
33–43 KB/s，不代表链路极限。此验证使用低音量合成测试音，并暂停麦克风上传，
未覆盖云端长期抖动、外界干扰或声学打断准确率；大屏本次仅完成构建和软件测试。


`0.6.7-dev` 保留右上角电池百分比，修正小屏电压采集量程：

- 有效读数始终显示 0–100 的整数和电池填充；充电时保留数字，使用绿色和
  左侧闪电。20% 及以下使用红色；只有校准或读取失败时显示 `--`。
- ADC 采集和 eFuse 曲线校准统一使用 6 dB。电池分压后约 0.87–1.45 V，
  落在 ESP32-S3 的 6 dB 校准量程内。官方 ADC 测试条件下，该量程的误差
  为 ±10 mV，原 12 dB 为 ±50 mV；这是 ADC 引脚规格，不能等同于整机
  或电量百分比精度。参见 [ESP32-S3 datasheet Table 5-6](https://documentation.espressif.com/esp32_s3_datasheet_en.pdf)。
- 独立低优先级任务每 2 秒取八个 ADC 样本，去掉极值后校准并乘以分压倍率；
  三次读数取中值，以拒绝一个孤立噪声或短暂负载压降，持续变化在第二个
  新样本时跟进。采样不占用音频、显示或电源键任务。
- 充电信号独立消抖。状态变化时清空滤波历史，继续显示最后数字，下一次
  实际采样立即替换；不会将充电电压带入长时间 EMA，也不等待或隐藏数字。
  采样失败或间隔超过 6 秒同样清空旧历史，恢复后立即使用新读数。
- BLE 的 `status` 消息为小屏附带 `battery` 对象，含 `valid`、`charging`、
  `percent` 和 `millivolts`；未知百分比为 `null`，大屏省略此对象。
  `millivolts` 是最新一次校准的端电压，数字来自中值滤波，因此短暂噪声时
  两者不必严格对应。拔掉 USB 后仍可通过蓝牙记录动态，无需读取屏幕数字。

电量数字来自通用单节 3.7V 锂电池电压曲线，尚未针对实际电池的容量、
电流、温度和负载标定。充电会抬高端电压，负载会使其下降；固件没有加入
未经实测的固定补偿，也不能把 4.2 V 直接当作已完成充电的证明。
板载分压电阻的容差不在 eFuse 校准范围内，进一步校准绝对电压需要外部
参考测量。BLE 日志可验证动态和重复性，不能独立证明剩余容量准确度。

CHG_STAT 高仅表示未充电，也可能仍插着 USB 且充电已停止；并非可靠 USB
在位或充满信号。硬件也没有可靠的电池在位信号，USB 供电且未装电池时，
充电芯片输出可能落在合理电压范围，图标不代表已检测到电池。
此功能不自动触发低电关机；大屏没有复用小屏的电池引脚。

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

播放期间持续运行本地检测，触发后先停止回答，再恢复上行并补发 800 ms 历史
语音及恢复前的新帧。有效参考出现后暖机 400 ms；触发需连续 120 ms 满足语音
判定及 `max(180 RMS, 3 × 环境噪声 RMS)` 门限。`input.suspend` 关闭检测并清除
缓存。屏幕和 BOOT/GPIO 0 手动停止始终发送到当前活动传输链路。

`0.6.9-dev` 为小屏增加按完整 AEC 块统计的约 128 ms 能量窗口：参考活跃时，
消除后功率不足原始麦克风功率 1% 的候选不能触发打断。该经验判断不增加
候选等待时间；窗口用于减少块边界波动，并非精确的 DSP 延迟对齐或通用双讲
分类器。历史从 300 ms 延长到 800 ms 仅用于补回插话开头，不延后停止。
大屏保持独立的检测参数和 300 ms 历史。

BLE 麦克风发送拥塞时，每次失败至少等待一个系统 tick，同一分片最多重试
200 ms，整帧最多 400 ms。此前 100 Hz 系统下的 1/2 ms 等待会换算为零 tick，历史突发可能丢帧。
队列可容纳 12 个 100 ms 帧；已成功发送的分片不会重复，静音或连接/会话切换
会废弃旧采集代次的待发数据。

此板使用双 240×24 行 DMA 绘图缓冲、64 KB 数据缓存及 64 字节缓存行。NimBLE
使用 `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL`；前端历史和待发送缓存共
57,600 字节分配到 PSRAM，DMA 缓冲、AEC 工作帧和任务栈保留在内部内存。
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
