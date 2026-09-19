#pragma once

#include <esp_err.h>

namespace meet {

esp_err_t UiInit();

void UiShowPairing(const char* code);
void UiShowReady(const char* character_name);  // nullptr → "未选择角色"
void UiShowUnprovisioned();
void UiShowInCall(const char* subtitle);
void UiShowSettings(const char* item_label);

/** Call from a dedicated task or after board LCD init. */
void UiTick(uint32_t elapsed_ms);

}  // namespace meet
