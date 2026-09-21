#pragma once

#include "app_event.h"
#include "app_state.h"
#include "meet_api.h"
#include "wifi_service.h"

#include <esp_err.h>
#include <string>
#include <vector>

namespace meet {

class AppController {
public:
    static AppController& Instance();

    esp_err_t Start();
    /** Sole consumer of AppEventQueue — runs on the app task. */
    void RunLoop();

private:
    AppController() = default;

    void HandleEvent(const AppEvent& ev);
    void PeriodicWork();

    void OnBootClick();
    void OnBootDoubleClick();
    void OnVolumeKey(int delta);

    void EnterPairing();
    void EnterReady();
    void EnterUnprovisioned();
    void EnterConnecting();
    void FinishConnectingToInCall();
    void FailConnecting();
    void LeaveInCall();
    void EnterSettings();
    void HandleSettingsActivate();
    void HandleWifiPhase(WifiPhase phase);
    void ApplyOnlineState();
    void HandleUnauthorized();
    void RefreshSettingsUi();
    void ApplyVolume(int volume, bool persist);
    void MaybeReportIdentity();
    void MaybeCheckFirmware(bool from_settings);
    void PrefetchRuntime();
    void InvalidateRuntimeCache();
    void HandleWeakNet();
    void NoteCallActivity();
    void StopIdleHangupTimer();
    void OnIdleHangup();
    void MarkOtaValidIfNeeded();
    void PrintDiagnostics();

    AppStateMachine state_;
    std::string pairing_code_ = "------";
    std::string pairing_hint_;
    std::string pairing_session_id_;
    std::string conversation_id_;
    std::string character_name_;
    std::string caption_;
    bool caption_sentence_done_ = true;

    // Settings menu: SelectChar, RePair, ReWifi, CheckUpdate
    int settings_index_ = 0;
    bool char_browse_mode_ = false;
    std::vector<MeetCharacter> char_list_;
    size_t char_browse_index_ = 0;

    int volume_ = 70;
    int64_t last_activity_us_ = 0;
    int64_t pairing_deadline_us_ = 0;
    int64_t last_pair_poll_us_ = 0;
    int64_t connecting_started_us_ = 0;
    int64_t last_diag_us_ = 0;
    bool idle_hangup_armed_ = false;
    bool identity_reported_ = false;
    bool firmware_checked_ = false;
    bool was_connected_ = false;
    bool ota_marked_valid_ = false;
    int weak_ticks_ = 0;
    int64_t last_weak_cue_us_ = 0;

    MeetCharacterRuntime runtime_cache_;
    bool runtime_cache_valid_ = false;
};

}  // namespace meet
