# ADR 0001: Meet WebSocket PCM instead of Xiaozhi Opus

## Status

Accepted

## Context

征辰 minicam 参考固件来自太空舱 / xiaozhi 生态，默认走厂商 opus 会话与 xiaozhi.me 协议。Meet 伴伴机需要对接私有 Meet 后端，与 Web 端 `QwenWebSocketRealtimeClient` 同一套 PCM realtime 中继。

## Decision

- Device ↔ Meet: HTTP for pairing/characters/conversations; WebSocket PCM for duplex audio.
- Do **not** embed xiaozhi session client, opus codec path, or xiaozhi.me endpoints on device.
- Reuse board/audio pin config from 太空舱; keep application protocol Meet-owned.

## Consequences

- Firmware stays lean and independent (no full xiaozhi Application fork).
- Audio path must supply 16 kHz PCM frames (20 ms) for uplink; downlink may arrive at provider rate and needs generation-tagged playback clearing on barge-in.
- Device holds DeviceCredential only; provider keys remain server-side.
