#include "app_state.h"

#include <esp_log.h>

namespace meet {
namespace {
constexpr char TAG[] = "app_state";
}

const char* AppStateName(AppState state) {
    switch (state) {
        case AppState::Unprovisioned: return "Unprovisioned";
        case AppState::Pairing: return "Pairing";
        case AppState::Ready: return "Ready";
        case AppState::Connecting: return "Connecting";
        case AppState::InCall: return "InCall";
        case AppState::Settings: return "Settings";
    }
    return "?";
}

void AppStateMachine::Set(AppState next) {
    if (next == state_) {
        return;
    }
    const AppState from = state_;
    state_ = next;
    ESP_LOGI(TAG, "%s -> %s", AppStateName(from), AppStateName(next));
    for (auto& cb : listeners_) {
        if (cb) {
            cb(from, next);
        }
    }
}

void AppStateMachine::AddListener(AppStateCallback cb) {
    listeners_.push_back(std::move(cb));
}

}  // namespace meet
