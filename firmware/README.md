# 固件说明

目标板为 Waveshare ESP32-S3-Touch-LCD-3.5，固件基于 ESP-IDF 5.5.x、`esp_codec_dev` 和 `esp_websocket_client`。

## 屏幕与触摸

- ST7796：320×480 竖屏，MOSI GPIO 1、CLK GPIO 5、DC GPIO 3
- LCD 复位：TCA9554 `0x20` 的 `EXIO1`
- 背光：GPIO 6，5 kHz PWM，界面可切换 100% / 65% / 30%
- FT6336：共享板载 I²C，总线地址 `0x38`
- LVGL 8.4：双 320×24 行 DMA 绘图缓冲，RGB565 字节交换已启用

首页为 JUFF 产品化语音伴侣界面。渐变语音球、旋转弧线和七段声波对应 `idle`、`listening`、`processing`、`speaking` 四种实时状态；顶部连接胶囊和 `QWEN` / `MIC` 状态胶囊隐藏底层传输细节。播放或处理中可轻触语音球/主按钮打断，右侧按钮切换亮度。音频自检保留在 BLE/WebUI 命令中，不再占用首页空间。TCA9554 的 LCD 复位与功放输出使用同一互斥保护和输出影子寄存器，避免并发操作覆盖其他 EXIO 位。

## 板载音频

- I²C：SCL GPIO 7、SDA GPIO 8
- I²S：MCLK 12、BCLK 13、DIN 14、WS 15、DOUT 16
- ES8311 地址：`0x18`
- NS4150B 功放：TCA9554 `0x20` 的 `EXIO7`
- Codec：24 kHz / PCM16 / mono
- 上行：24 kHz 每 2400 样本线性重采样为 16 kHz 每 1600 样本

无 NVS Wi-Fi 配置时，启动会先丢弃 500 ms ADC 瞬态，再测环境值并播放 660/880 Hz 双音。麦克风检测到明显声学增量即报告自检通过。

## USB 配网

USB Serial/JTAG 始终运行一个不回显秘密的 JSON 配网端点。使用：

```bash
python3 -m venv .venv
.venv/bin/pip install pyserial
.venv/bin/python scripts/provision_wifi.py
```

SSID、密码、Bridge URI 和设备令牌会写入 `juff` NVS 命名空间；重新配网会覆盖旧值并重启。固件常量只作为 NVS 尚未配置时的回退值。

## BLE 语音、配网、控制与状态

NimBLE 外设广播名为 `JUFF-xxxx`，自定义服务 UUID 为 `B8A5FCA1-8A0F-4DB1-9C6C-7B5B6C49A001`。Mac 写入控制特征前必须完成 LE Secure Connections + MITM 配对，随机 6 位码只显示在板载屏幕上，bonding key 持久化到 NVS。

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

Wi-Fi Bridge 使用原始 PCM16；BLE 使用 G.711 μ-law。两条链路当前都采用半双工：扬声器播放期间 RX DMA 继续工作，但麦克风帧不上传。全双工自然插话需要后续加入 AEC。
