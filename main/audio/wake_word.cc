#include "wake_word.h"

#include <esp_log.h>

namespace meet {
namespace {
constexpr char TAG[] = "wake";
}

WakeWord& WakeWord::Instance() {
    static WakeWord w;
    return w;
}

esp_err_t WakeWord::Start() {
    running_ = true;
    ESP_LOGW(TAG, "WakeWord Start stub — esp-sr AFE TODO; use Boot for Ready↔InCall");
    return ESP_OK;
}

void WakeWord::Stop() {
    running_ = false;
}

void WakeWord::SetOnDetected(WakeWordDetectedCb cb) {
    on_detected_ = std::move(cb);
    (void)running_;
}

}  // namespace meet
