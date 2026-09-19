# 移植太空舱板级层，不 fork 小智

太空舱资料包是 xiaozhi 2.2.6 + 独有 `zhengchen-minicam`；官方 xiaozhi v2.5.0 没有这块板，且默认 MQTT、hello/Opus、xiaozhi OTA 激活。Meet 终端继续独立仓库 `meet_esp32`，只移植配网、ES8388、AFE、ADC、LCD 交互，会话协议保持 Meet HTTP + WebSocket PCM。

备选是整仓 fork 小智再改协议：能更快点亮硬件，但会把 171 板型、MCP 主控和厂商会话栈带进主树，之后很难拆。配网用 SoftAP（SSID `Meet-XXXX`），不做 BluFi / 声波。
