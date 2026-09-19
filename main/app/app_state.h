#pragma once

#include <functional>
#include <vector>

namespace meet {

enum class AppState {
    Unprovisioned,  // WiFi / server URL
    Pairing,        // show PairingCode
    Ready,          // SelectedCharacter, wait Boot/wake
    InCall,         // duplex call
    Settings,
};

using AppStateCallback = std::function<void(AppState from, AppState to)>;

class AppStateMachine {
public:
    AppState Get() const { return state_; }
    void Set(AppState next);
    void AddListener(AppStateCallback cb);

private:
    AppState state_ = AppState::Unprovisioned;
    std::vector<AppStateCallback> listeners_;
};

const char* AppStateName(AppState state);

}  // namespace meet
