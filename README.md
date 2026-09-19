# Meet 伴伴机固件 (`meet_esp32`)

征辰 minicam（太空舱）ESP32-S3 上的 **Meet 伴伴机** 精简固件。独立工程，不 fork 整套 xiaozhi。

## 要求

- ESP-IDF **≥ 5.5.2**
- 目标芯片：`esp32s3`
- 后端：私有 **Meet** 服务（非 xiaozhi.me），需已迁移 `0024_companion_devices`

## 构建

```bash
idf.py set-target esp32s3
idf.py build
# idf.py -p /dev/ttyUSB0 flash monitor
```

`menuconfig` 中可改：

- `MEET_SERVER_URL`：家庭 Meet Origin（默认 `https://meet.refme.cc`）
- `MEET_IDLE_HANGUP_MS`：空闲挂断（默认 90000）
- `MEET_USE_DEVICE_AEC`：设备端 AEC 钩子（默认开）

## 产品流程

1. 设备联网后进入 **Pairing**，向 `POST /api/devices/pairing-sessions` 申请 6 位码并显示
2. 家庭成员在网页「我的 → 陪伴设备」输入配对码完成绑定，设备轮询拿到 **DeviceCredential**
3. **Ready**：Boot 单击 / 唤醒词进入 **InCall**；双击进设置（切角色、重新配对）
4. **InCall**：拉 runtime → 建会话 → 儿童强制 `chat_only` prepare → WebSocket PCM 全双工；插话清空播放队列；IdleHangup 后 `complete`

## 边界

| 来源 | 用途 |
| --- | --- |
| 太空舱 / zhengchen-minicam 板级 | 引脚、I2C/SPI、ST7789、ES8388、Boot/音量 ADC |
| Meet WebSocket PCM realtime | 双工通话协议（`session.update` / PCM base64 / `response.audio.delta`） |

板级与音频路径参考太空舱资料；会话与协议走 Meet HTTP + WebSocket，不使用 xiaozhi opus 会话栈。

## 当前实现状态

- 已实现：状态机、配对/角色 HTTP、Realtime WS 客户端、generation 清空播放队列、Boot / IdleHangup / 儿童 prepare
- Stub：Wi-Fi SoftAP 配网、ES8388 真采集（现为静音帧上行）、esp-sr AFE 唤醒词与 AEC、完整 LVGL 横竖屏菜单

## 领域词与方案

- 领域词：[CONTEXT.md](./CONTEXT.md)
- 整机产品与技术方案：[docs/product-and-architecture.md](./docs/product-and-architecture.md)
- 协议边界：[docs/adr/0001-meet-pcm-over-xiaozhi-protocol.md](./docs/adr/0001-meet-pcm-over-xiaozhi-protocol.md)
- 工程策略：[docs/adr/0002-port-minicam-layers-not-fork-xiaozhi.md](./docs/adr/0002-port-minicam-layers-not-fork-xiaozhi.md)

## 目录概览

```
main/
  boards/zhengchen-minicam/  # 板级 bring-up
  app/                       # 状态机 / 控制器
  ui/                        # LVGL 占位屏
  meet/                      # HTTP API + realtime WS
  audio/                     # PCM pipeline / wake stub
  main.cc
```
