#pragma once

#include <cstdint>
#include <esp_err.h>

namespace meet {

esp_err_t UiInit();

/** All UiShow* / UiSet* post to the UI command queue; only the ui task touches LVGL. */
void UiShowPairing(const char* code, const char* hint = nullptr);
void UiShowReady(const char* character_name);
void UiShowUnprovisioned();
void UiShowWifiConfig(const char* ap_ssid, const char* url);
void UiShowWifiConnecting(const char* ssid);
void UiShowConnecting();
void UiShowInCall(const char* subtitle);
/** Pre-formatted settings body (lines with "> " marker already applied). */
void UiShowSettings(const char* body);
void UiShowToast(const char* text);
void UiSetEmotion(const char* name);
void UiSetCaption(const char* text);
void UiClearCaption();

/** Must be called only from the ui task. */
void UiTick(uint32_t elapsed_ms);

/** Called from panel IO color-trans-done ISR/callback. */
void UiNotifyFlushDone();

}  // namespace meet
