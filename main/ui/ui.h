#pragma once

#include <cstdint>
#include <esp_err.h>

namespace meet {

esp_err_t UiInit();

void UiShowPairing(const char* code, const char* hint = nullptr);
void UiShowReady(const char* character_name);
void UiShowUnprovisioned();
void UiShowWifiConfig(const char* ap_ssid, const char* url);
void UiShowWifiConnecting(const char* ssid);
void UiShowConnecting();
void UiShowInCall(const char* subtitle);
void UiShowSettings(const char* const* items, int count, int index);
void UiShowToast(const char* text);
void UiApplyOrientation(bool landscape);

void UiTick(uint32_t elapsed_ms);

}  // namespace meet
