# ADR 0003: Event queue + single state owner + control/data plane split

## Status

Accepted

## Context

P0 defects (stack overflow on Boot/wake HTTP, LVGL multi-task races, uplink draining at 10 fps instead of 50, SoftAP httpd self-stop deadlock) shared one root cause: no single state owner and no clear thread boundary. Producers invoked heavy `AppController` work via `std::function` on tiny stacks; UI was written from five tasks.

## Decision

1. **Control plane**: fixed-size `AppEvent` queue. Board, Wi-Fi, WebSocket, and wake AFE only `xQueueSend`. The `app` task is the sole consumer and the only task that mutates app state or runs blocking HTTPS.
2. **Data plane**: uplink PCM (AFE → `SendInputPcm`) and downlink PCM (WS → playback queue) bypass the control queue. Realtime speech-started clears the playback generation without going through LVGL.
3. **UI**: command queue; only the `ui` task calls `lv_*`. LCD flush completion uses `on_color_trans_done` → `lv_display_flush_ready`.
4. **Settings**: in-memory singleton loaded at boot; NVS writes are serialized. Protocol layer never opens NVS.

## Consequences

- Concurrent correctness rests on queue discipline, not ad-hoc locks.
- Audio latency stays independent of UI/HTTP stalls.
- Stack budget: `app` 12K (TLS), producers ~2.5K (post only).
