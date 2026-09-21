# Meet 伴伴机 — Domain Glossary

Lean firmware domain language for the ESP32 companion device. Keep terms stable across firmware, backend, and product docs.

## Language

**CompanionDevice**:
Physical Meet companion hardware. Canonical product shape is **太空舱** (engineering board `zhengchen-minicam`). Runs this firmware. Owns local UI, audio duplex, and device-side credentials.
_Avoid_: Xiaozhi box, phone browser client, agent device, zhengchen-cam (different codec/pins)

**太空舱**:
The sitting, landscape-oriented minicam CompanionDevice this firmware targets. Same object as CompanionDevice in the current hardware generation.
_Avoid_: 竖屏默认、官方 zhengchen-cam、任意 ESP32 小智盒子

**WifiProvisioning**:
The device is collecting or joining household Wi-Fi (SoftAP or STA connect). Distinct from account Pairing.
_Avoid_: 配网当绑定, activation, BluFi

**PairingCode**:
Short human-readable code shown on device during binding. A household member claims it in Meet web so the device joins without typing passwords on-device.
_Avoid_: activation code, OTP login

**DeviceBinding**:
Successful claim of a PairingCode that associates this CompanionDevice with a Meet account/household. Produces a DeviceCredential stored in NVS.
_Avoid_: login, OTA activate

**DeviceCredential**:
Bearer secret issued after DeviceBinding. Used as `Authorization: Bearer …` for Meet HTTP/WebSocket. Rotated only via re-pair.
_Avoid_: password, meet_session cookie, vendor API key

**SelectedCharacter**:
The character currently chosen for calls on this device (`id` + display name). Persisted locally and reflected via `PATCH /api/devices/me`.
_Avoid_: agent, bot, xiaozhi role

**Ready**:
Bound device waiting for a Call. Shows SelectedCharacter. Not a listen/speak mode.
_Avoid_: idle-as-xiaozhi-standby, listening

**Connecting**:
Opening the Meet realtime channel after the user starts a Call, before audio is live. Waits for relay/session ready.
_Avoid_: already InCall, xiaozhi connecting-as-listen

**Call**:
A duplex realtime session: create conversation → (child: teaching prepare chat_only) → WebSocket PCM → uplink mic / downlink playback → complete/close. Product meaning is a phone call, not listen/speak turn-taking.
_Avoid_: xiaozhi listening/speaking states as the primary UX

**InCall**:
UI/state while a Call is live and the realtime channel is ready.
_Avoid_: 聆听中, 说话中

**IdleHangup**:
Automatic end of Call after configurable idle period (default 90s). Completes the conversation and returns to Ready.
_Avoid_: silence timeout as crash, close-without-complete

**FactorySerial**:
Write-once device serial in NVS `factory`. Empty boards persist `MEET-` plus MAC suffix.
_Avoid_: DeviceCredential as serial, xiaozhi MAC activation

**MeetEmotion**:
Named face shown on Ready/InCall (`neutral`…`confused`, same 20 names as 太空舱). Local mapping plus optional `{type:"meet.emotion"}` from the Meet relay.
_Avoid_: xiaozhi `llm` JSON, GIF/PSRAM emoji packs

## Avoid on device

| Anti-pattern | Why |
| --- | --- |
| Device password login | PairingCode + DeviceBinding replaces typing account passwords on the companion. |
| Xiaozhi session / xiaozhi.me protocol | Protocol boundary is Meet private backend WebSocket PCM realtime, not vendor opus sessions. |
| Vendor API keys on device | Qwen/provider keys stay server-side; device only holds DeviceCredential + Meet origin. |
| Listen/speak as main modes | Meet Call is full-duplex, matching the web client. |
