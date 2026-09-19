#pragma once

#include "app_state.h"

#include <esp_err.h>
#include <esp_timer.h>
#include <string>

namespace meet {

class AppController {
public:
    static AppController& Instance();

    esp_err_t Start();

    AppStateMachine& state() { return state_; }
    const AppStateMachine& state() const { return state_; }

    void OnBootClick();
    void OnBootDoubleClick();
    void OnBootLongPress();
    void OnVolumeKey(int delta);
    void Tick();

private:
    AppController() = default;

    void EnterPairing();
    void EnterReady();
    void EnterUnprovisioned();
    void EnterConnecting();
    void FinishConnectingToInCall();
    void FailConnecting();
    void LeaveInCall();
    void EnterSettings();
    void HandleSettingsActivate();
    void HandleWifiPhase();
    void ApplyOnlineState();
    void HandleUnauthorized();
    void RefreshSettingsUi();
    void ApplyVolume(int volume, bool persist);
    void ApplyOrientation(bool landscape, bool persist);

    void StartIdleHangupTimer();
    void StopIdleHangupTimer();
    void OnIdleHangup();
    void NoteCallActivity();

    AppStateMachine state_;
    std::string pairing_code_ = "------";
    std::string pairing_hint_;
    std::string pairing_session_id_;
    std::string conversation_id_;
    std::string character_name_;
    int settings_index_ = 0;
    int volume_ = 70;
    bool landscape_ = false;
    int64_t last_activity_us_ = 0;
    int64_t pairing_deadline_us_ = 0;
    int64_t last_pair_poll_us_ = 0;
    int64_t connecting_started_us_ = 0;
    bool idle_hangup_armed_ = false;
    bool disconnect_seen_ = false;
};

}  // namespace meet
