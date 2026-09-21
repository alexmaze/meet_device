# Meet 伴伴机固件 (`meet_esp32`)

征辰 minicam（太空舱）ESP32-S3 上的 **Meet 伴伴机** 精简固件。独立工程，不 fork 整套 xiaozhi。

## 要求

- ESP-IDF **≥ 5.5.2**（当前锁文件构建环境为 **IDF 6.1.x**）
- 目标芯片：`esp32s3`
- 后端：私有 **Meet** 服务（非 xiaozhi.me），需已迁移 `0024_companion_devices`

## 构建

```bash
idf.py set-target esp32s3
idf.py build
# idf.py -p /dev/ttyUSB0 flash monitor
```

### Mac 上预览 UI（不用烧录）

同一套 `main/ui` 控件 / CJK 字体 / 黄脸，桌面 SDL 窗口秒编：

```bash
# 依赖：brew install sdl2
cmake -S host/ui_preview -B build/ui_preview
cmake --build build/ui_preview
./build/ui_preview/meet_ui_preview
```

按键：`1`–`7` 切页面，`e` 表情，`t` toast，`c` 字幕，`o` 横竖屏，`q` 退出。真机仍需验 ST7789 颜色和按键手感。

`menuconfig` 中可改：

- `MEET_SERVER_URL`：家庭 Meet Origin（默认 `https://meet.refme.cc`）
- `MEET_IDLE_HANGUP_MS`：空闲挂断（默认 90000）
- `MEET_USE_DEVICE_AEC`：设备端 AEC（默认开）

## 产品流程

1. 设备联网后进入 **Pairing**，向 `POST /api/devices/pairing-sessions` 申请 6 位码并显示
2. 家庭成员在网页「我的 → 陪伴设备」输入配对码完成绑定，设备轮询拿到 **DeviceCredential**
3. **Ready**：Boot 单击 / 唤醒词进入 **InCall**；双击进设置（切角色、重新配对、重新配网、检查更新）
4. **InCall**：建会话 → 儿童 `chat_only` prepare → WebSocket PCM 全双工；插话清空播放队列；IdleHangup 后 `complete`

按键：Boot 单击接通/挂断（设置内激活项）；Boot 双击进出设置；Boot 长按是外壳关机；音量键调音量（设置内上下移动）。

## 架构要点

控制面走 `AppEvent` 队列（`app` 任务唯一持有状态）；音频数据面直连；UI 命令队列独占 LVGL。见 [ADR-0003](./docs/adr/0003-event-queue-and-planes.md)。

## 边界

| 来源 | 用途 |
| --- | --- |
| 太空舱 / zhengchen-minicam 板级 | 引脚、I2C/SPI、ST7789、ES8388、Boot/音量 ADC |
| Meet WebSocket PCM realtime | 双工通话协议（`session.update` / PCM base64 / `response.audio.delta`） |

I2S 默认 **16 kHz**（与 AFE / 上行对齐）；下行 24 kHz 经抗混叠重采样到 16 kHz 播放。

## 当前实现状态

系统性改造后：

- 事件队列 + 常驻音频链路 + Settings 单例 + UI 单线程
- SoftAP 配网（表单提交无死锁）、配对 410/409、Connecting 先反馈、runtime 预取
- 儿童 teaching `audio_gate` ACK、OTA 回滚配置、分区表按实际 model 体积调整

仍需真机验证：ES8388@16kHz、AEC/插话、唤醒率、teaching 闸门、配网切 STA。

## 领域词与方案

- 领域词：[CONTEXT.md](./CONTEXT.md)
- 硬件手册（太空舱 / zhengchen-minicam）：[docs/hardware-zhengchen-minicam.md](./docs/hardware-zhengchen-minicam.md)
- 整机产品与技术方案：[docs/product-and-architecture.md](./docs/product-and-architecture.md)
- 协议边界：[docs/adr/0001-meet-pcm-over-xiaozhi-protocol.md](./docs/adr/0001-meet-pcm-over-xiaozhi-protocol.md)
- 工程策略：[docs/adr/0002-port-minicam-layers-not-fork-xiaozhi.md](./docs/adr/0002-port-minicam-layers-not-fork-xiaozhi.md)
- 并发模型：[docs/adr/0003-event-queue-and-planes.md](./docs/adr/0003-event-queue-and-planes.md)

## 目录概览

```
main/
  boards/zhengchen-minicam/  # 板级 bring-up
  app/                       # 事件队列 / Settings / 状态机
  ui/                        # LVGL（命令队列）+ face
  meet/                      # HTTP API + realtime WS（不依赖 audio/）
  audio/                     # 常驻 pipeline / AfeUnit / resampler
  net/                       # SoftAP · STA
  main.cc
```
