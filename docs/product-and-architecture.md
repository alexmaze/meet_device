# Meet 终端：产品与技术方案

状态：Accepted  
日期：2026-09-19  
对照：太空舱资料包 `zhengchen-minicam`（xiaozhi 2.2.6）、官方 `xiaozhi-esp32` v2.5.0、Meet ADR-0038 / 固件 ADR-0001

本文是伴伴机整机方案。领域词见 [CONTEXT.md](../CONTEXT.md)。协议边界见 [ADR-0001](./adr/0001-meet-pcm-over-xiaozhi-protocol.md)。工程策略见 [ADR-0002](./adr/0002-port-minicam-layers-not-fork-xiaozhi.md)。

## 产品定位

Meet 终端不是「又一个小智盒子」，而是家庭里的 Meet 角色实体：插上电、配上网、对着屏上的 6 位码在网页绑定，之后按一下就能和家里选中的角色通话。

| | 小智 / 太空舱 | Meet 终端 |
| --- | --- | --- |
| 身份 | 激活到小智云，控制台配 Agent | 绑到家庭账号，凭证在设备上 |
| 对话对象 | 云端下发的智能体 | **SelectedCharacter**（本机可选、可改） |
| 通话形态 | 聆听 / 说话轮转，可选实时打断 | **电话式全双工**，和网页同一条 WS PCM |
| 儿童 | 无家庭账号模型 | 儿童账号强制 `chat_only` |
| 结束 | 说完回待命，或关通道 | **IdleHangup ~90s** + 主动挂断 |
| 控制面 | MCP + 小智 JSON（tts/stt/llm） | Meet HTTP + `session.update`，不做 MCP 主控 |

第一版只做**语音陪伴**。太空舱有摄像头，先关掉，避免和 I2C/PSRAM 抢资源。

## 用户旅程

```
开箱配网 → 配对绑定 → Ready ⇄ Call
                ↗ Settings → 重新配对 / 重新配网
```

1. **WifiProvisioning**（学太空舱 SoftAP，换皮）  
   无 Wi-Fi，或长按 Boot / 设置「重新配网」→ SoftAP（SSID `Meet-XXXX`，不用 `Xiaozhi`）→ 手机连热点、浏览器填家里 Wi-Fi → 写入 NVS → STA，约 60 秒超时再回配网。不做 BluFi / 声波配网。

2. **Pairing**（学小智激活码，换协议）  
   屏上 6 位 **PairingCode** → 网页「我的 → 陪伴设备」claim → 轮询拿到一次性 **DeviceCredential**。过期重申；失败不得一直显示 `------`。

3. **Ready / Call**  
   Ready 显示 **SelectedCharacter**。Boot 单击或唤醒词进入 **Connecting**，通道就绪后进入 **InCall**；再按挂断。主状态不做小智的「聆听中 / 说话中」切换——产品语义是一路电话。

4. **Settings**（学 minicam 菜单，改内容）  
   双击进菜单。P0 之后：选角色、重新配对、Wi-Fi、横竖屏、音量。AEC 默认开，不进第一版菜单。

重配网不清 DeviceCredential；重新配对才清凭证。

## 系统拆分

```
太空舱硬件 + meet_esp32
  UI / 状态机 / SoftAP·STA / ES8388+AFE / Meet HTTP / Meet WS PCM
        │
        ▼
Meet 后端：配对与凭证 · 角色 runtime · 会话与 teaching · Realtime 中继
        │
        ▼
网页（绑定与选角）     云端语音供应商
```

已锁定、不要改：

- 设备只持有 DeviceCredential + Meet Origin，厂商 Key 不上板。
- 实时音频走 `wss://…/api/characters/{id}/realtime/websocket`，16 kHz PCM 上行 / 24 kHz 下行，对齐网页客户端。
- 不要 hello、Opus、MQTT+UDP、xiaozhi OTA、MCP 当主控。

## 固件分层

继续独立工程 `meet_esp32`。官方 xiaozhi v2.5.0 **没有** `zhengchen-minicam`；板级以太空舱资料包为准。

**从太空舱移植（改命名空间，不整仓 fork）：**

| 层 | 来源 | 用途 |
| --- | --- | --- |
| 引脚 / ES8388 / I2S 24 kHz + reference | `zhengchen-minicam/config.h`、`es8388_audio_codec.cc` | 真麦真喇叭、AEC 参考声 |
| SoftAP + SSID 存储 | `wifi_board.cc` + `esp-wifi-connect` | 配网 |
| AFE 采集 / 播放 / 唤醒 / 设备 AEC | `afe_audio_processor`、`afe_wake_word` | 可说话、可打断 |
| ADC 音量梯、双 ADC 电池、GPIO47 状态线 | `zhengchen_minicam.cc` | 按键和电量 |
| 横竖屏 NVS + 设置菜单交互 | `zhengchen_minicam_lcd_display.cc` | 少造一套手势 |
| 中文字体 | `font_puhui` 一类 CJK | 屏上能读中文 |

**自己写：** 状态机、Meet HTTP、WS PCM、配对轮询、IdleHangup、儿童 `chat_only`。

**丢掉：** hello / Opus 60 ms / MQTT+UDP / `api.tenclass.net` 激活 / MCP / 小智 tts·stt·llm JSON / 171 板型 Kconfig。

## 状态机

```
Unprovisioned ──配上网──► Pairing ──拿到凭证──► Ready
Ready ──单击/唤醒──► Connecting ──session.updated──► InCall
InCall ──挂断/IdleHangup/断线失败──► Ready
Ready ──双击──► Settings ──重配网/重配对──► Unprovisioned / Pairing
任意联网失败 ──► Unprovisioned（保留凭证，只丢 Wi-Fi）
```

比早期五态多 **Connecting**（开 WS、等 `relay.ready` / `session.updated`），避免一点 Boot 就显示「通话中」。

## 音频

```
麦 24 kHz ──ES8388──► AFE（AEC + VAD + 唤醒）
                ──► 降到 16 kHz / 20 ms / 320 sample
                ──► input_audio_buffer.append

response.audio.delta 24 kHz ──► 播放队列（带 generation）
speech_started ──► ClearGeneration + response.cancel
                ──► ES8388 播出
```

- Codec 按板级 24 kHz 双声道（含 reference）初始化。
- 上行在 AFE 之后 16 kHz mono 20 ms。
- 下行保持 24 kHz 播放。
- AEC 默认设备端。没有 AEC 不宣传「随时插话」。
- 唤醒词用 esp-sr AFE；P0 必须 Boot 能打通电话。

## 存储

NVS 分两块：

- `wifi`：SSID / 密码
- `meet`：origin、DeviceCredential、deviceId、SelectedCharacter、横竖屏

## 协议稳健性

- 配对看 `expiresAt`，过期重申；轮询 1–2 s，禁止每 100 ms 同步 HTTP。
- 创建配对会话带 `displayName`（例如 `太空舱-XXXX`）。
- 通话等 `relay.ready` 再 `session.update`。
- WS 断线：Connecting / InCall 重试 1–2 次，失败回 Ready 并提示。
- `GET /api/auth/me` 401 → 清凭证回 Pairing。
- 第一版不做 session renewal、消息落库、teaching 控制帧。

## 屏幕

状态栏（Wi-Fi / 电量）+ 大字主状态 + 设置列表。中文必须用 CJK 字体。P0 最低四屏：配网说明、配对码、待命（角色名）、通话中。表情 / GIF 放到更后。

## 分阶段

**P0 — 家里能打通一通电话**

1. SoftAP 配网 + STA
2. 从太空舱搬 ES8388：真采集 + 真播放 + 16k/24k 对齐
3. 配对过期重试、失败提示
4. Connecting 态 + 等 `relay.ready` + 断线回 Ready
5. 中文字体

**P1 — 能当日常设备**

唤醒词、设备 AEC 全双工、音量 ADC、电量、横竖屏、设置菜单、凭证失效回配对。

**P2 — 能铺货**

HTTPS OTA、断网 / 弱网提示音、多 SSID、字幕、表情、可选拍照、生产烧录 / 序列号。

P0 不做：摄像头、MCP、4G、BluFi、小智 OTA 激活包。OTA 用 Meet 自己的 HTTPS 地址，第一版可继续 `idf.py flash`。

## 工程约束

- 板级以太空舱 `main/boards/zhengchen-minicam/` 为准，不要抄官方 `zhengchen-cam`（ES8311+ES7210，引脚不同）。
- 小智新代码只当参考，按模块移植进 `meet_esp32`。
- 后端已有设备 API 够 P0；缺口在固件。
- 网页继续承担绑定和管理；设备不输入账号密码。
