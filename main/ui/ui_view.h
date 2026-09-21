#pragma once

#include <cstdint>

namespace meet {

/** Snapshot of board/wifi state so the view never touches Board/WifiService. */
struct UiEnv {
    int width = 320;
    int height = 240;
    bool landscape = true;
    bool wifi_ok = true;
    int rssi = -50;
    int battery_percent = 87;
    bool charging = false;
};

enum class UiCmdType : uint8_t {
    Pairing,
    Ready,
    Unprovisioned,
    WifiConfig,
    WifiConnecting,
    Connecting,
    InCall,
    Settings,
    Toast,
    Emotion,
    Caption,
    ClearCaption,
};

struct UiCommand {
    UiCmdType type = UiCmdType::Ready;
    char title[48] = {};
    char body[320] = {};
};

void UiViewSetEnv(const UiEnv& env);
void UiViewEnsureWidgets();
void UiViewRelayout();
void UiViewShowPlain(const char* title, const char* body);
void UiViewApplyCommand(const UiCommand& cmd);
/** Toast countdown + status bar refresh. Caller still runs lv_tick_inc / lv_timer_handler. */
void UiViewTick(uint32_t elapsed_ms);

}  // namespace meet
