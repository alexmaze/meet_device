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

    /** Tick from FreeRTOS task — pairing poll, idle hangup, etc. */
    void Tick();

private:
    AppController() = default;

    void EnterPairing();
    void EnterReady();
    void EnterUnprovisioned();
    void EnterInCall();
    void LeaveInCall();
    void EnterSettings();
    void HandleSettingsActivate();

    void StartIdleHangupTimer();
    void StopIdleHangupTimer();
    void OnIdleHangup();
    void NoteCallActivity();

    AppStateMachine state_;
    std::string pairing_code_ = "------";
    std::string pairing_session_id_;
    std::string conversation_id_;
    std::string character_name_;
    int settings_index_ = 0;
    bool wifi_ready_ = false;  // stub until WiFi provisioned
    int64_t last_activity_us_ = 0;
    bool idle_hangup_armed_ = false;
};

}  // namespace meet
