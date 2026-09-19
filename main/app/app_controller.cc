#include "app_controller.h"

#include "board.h"
#include "meet_api.h"
#include "meet_nvs.h"
#include "meet_realtime.h"
#include "pcm_pipeline.h"
#include "ui.h"
#include "wake_word.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include <vector>

namespace meet {
namespace {

constexpr char TAG[] = "app_ctrl";

const char* kSettingsItems[] = {
    "选择角色",
    "重新配对",
    "Wi-Fi (stub)",
    "横竖屏 (stub)",
};

}  // namespace

AppController& AppController::Instance() {
    static AppController ctrl;
    return ctrl;
}

esp_err_t AppController::Start() {
    state_.AddListener([this](AppState /*from*/, AppState to) {
        switch (to) {
            case AppState::Unprovisioned:
                UiShowUnprovisioned();
                break;
            case AppState::Pairing:
                UiShowPairing(pairing_code_.c_str());
                break;
            case AppState::Ready:
                UiShowReady(character_name_.empty() ? nullptr : character_name_.c_str());
                break;
            case AppState::InCall:
                UiShowInCall("通话中");
                break;
            case AppState::Settings:
                UiShowSettings(kSettingsItems[settings_index_]);
                break;
        }
    });

    MeetRealtime::Instance().SetSpeechStartedHandler([this]() { NoteCallActivity(); });
    MeetRealtime::Instance().SetActivityHandler([this]() { NoteCallActivity(); });

    WakeWord::Instance().SetOnDetected([this]() {
        if (state_.Get() == AppState::Ready) {
            ESP_LOGI(TAG, "WakeWord detected -> InCall");
            EnterInCall();
        }
    });

    MeetConfig cfg;
    if (MeetNvsLoad(cfg) != ESP_OK) {
        ESP_LOGW(TAG, "NVS load failed; using defaults");
    }

    if (cfg.server_origin.empty()) {
        cfg.server_origin = CONFIG_MEET_SERVER_URL;
        MeetNvsSave(cfg);
    }

    MeetApi::Instance().Configure(cfg.server_origin, cfg.device_credential);

    if (!wifi_ready_ && cfg.device_credential.empty()) {
        EnterUnprovisioned();
        if (!cfg.server_origin.empty()) {
            wifi_ready_ = true;
            EnterPairing();
        }
    } else if (cfg.device_credential.empty()) {
        EnterPairing();
    } else {
        character_name_ = cfg.selected_character_name;
        EnterReady();
    }

    Board::Instance().SetBootClickHandler([this]() { OnBootClick(); });
    Board::Instance().SetBootDoubleClickHandler([this]() { OnBootDoubleClick(); });
    return ESP_OK;
}

void AppController::OnBootClick() {
    NoteCallActivity();
    switch (state_.Get()) {
        case AppState::Ready:
            EnterInCall();
            break;
        case AppState::InCall:
            LeaveInCall();
            break;
        case AppState::Settings:
            HandleSettingsActivate();
            break;
        case AppState::Pairing:
        case AppState::Unprovisioned:
            break;
    }
}

void AppController::OnBootDoubleClick() {
    NoteCallActivity();
    if (state_.Get() == AppState::Settings) {
        constexpr int kCount = static_cast<int>(sizeof(kSettingsItems) / sizeof(kSettingsItems[0]));
        settings_index_ = (settings_index_ + 1) % kCount;
        UiShowSettings(kSettingsItems[settings_index_]);
    } else if (state_.Get() == AppState::Ready || state_.Get() == AppState::InCall) {
        if (state_.Get() == AppState::InCall) {
            LeaveInCall();
        }
        EnterSettings();
    }
}

void AppController::EnterUnprovisioned() {
    state_.Set(AppState::Unprovisioned);
}

void AppController::EnterPairing() {
    pairing_code_ = "------";
    pairing_session_id_.clear();
    state_.Set(AppState::Pairing);

    MeetPairingSession session;
    esp_err_t err = MeetApi::Instance().CreatePairingSession(session);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CreatePairingSession failed (%s); placeholder code", esp_err_to_name(err));
        UiShowPairing(pairing_code_.c_str());
        return;
    }
    pairing_code_ = session.code.empty() ? "------" : session.code;
    pairing_session_id_ = session.id;
    UiShowPairing(pairing_code_.c_str());
}

void AppController::EnterReady() {
    StopIdleHangupTimer();
    MeetConfig cfg;
    MeetNvsLoad(cfg);
    if (!cfg.selected_character_id.empty()) {
        character_name_ = cfg.selected_character_name;
    } else {
        std::vector<MeetCharacter> chars;
        if (MeetApi::Instance().ListCharacters(chars) == ESP_OK && !chars.empty()) {
            character_name_ = chars.front().name;
            cfg.selected_character_id = chars.front().id;
            cfg.selected_character_name = chars.front().name;
            MeetNvsSave(cfg);
            MeetApi::Instance().UpdateSelectedCharacter(chars.front().id);
        } else {
            character_name_.clear();
        }
    }
    state_.Set(AppState::Ready);
}

void AppController::EnterSettings() {
    settings_index_ = 0;
    state_.Set(AppState::Settings);
}

void AppController::HandleSettingsActivate() {
    switch (settings_index_) {
        case 0: {  // select character
            std::vector<MeetCharacter> chars;
            if (MeetApi::Instance().ListCharacters(chars) == ESP_OK && !chars.empty()) {
                MeetConfig cfg;
                MeetNvsLoad(cfg);
                size_t idx = 0;
                for (size_t i = 0; i < chars.size(); ++i) {
                    if (chars[i].id == cfg.selected_character_id) {
                        idx = (i + 1) % chars.size();
                        break;
                    }
                }
                MeetApi::Instance().UpdateSelectedCharacter(chars[idx].id);
                cfg.selected_character_id = chars[idx].id;
                cfg.selected_character_name = chars[idx].name;
                MeetNvsSave(cfg);
                character_name_ = chars[idx].name;
                ESP_LOGI(TAG, "Selected character: %s", character_name_.c_str());
            } else {
                ESP_LOGW(TAG, "ListCharacters unavailable");
            }
            EnterReady();
            break;
        }
        case 1:  // re-pair
            {
                MeetConfig cfg;
                MeetNvsLoad(cfg);
                cfg.device_credential.clear();
                cfg.device_id.clear();
                MeetNvsSave(cfg);
                MeetApi::Instance().Configure(cfg.server_origin, "");
                EnterPairing();
            }
            break;
        case 2:
            ESP_LOGW(TAG, "Wi-Fi settings stub");
            EnterReady();
            break;
        case 3:
            ESP_LOGW(TAG, "Orientation toggle stub");
            EnterReady();
            break;
        default:
            EnterReady();
            break;
    }
}

void AppController::EnterInCall() {
    MeetConfig cfg;
    MeetNvsLoad(cfg);
    if (cfg.selected_character_id.empty()) {
        ESP_LOGW(TAG, "No SelectedCharacter; staying Ready");
        UiShowReady(nullptr);
        return;
    }

    MeetCharacterRuntime runtime;
    esp_err_t err = MeetApi::Instance().GetCharacterRuntime(cfg.selected_character_id, runtime);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GetCharacterRuntime failed: %s", esp_err_to_name(err));
        return;
    }

    MeetConversation conv;
    err = MeetApi::Instance().CreateConversation(cfg.selected_character_id, "normal", conv);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CreateConversation failed: %s", esp_err_to_name(err));
        return;
    }
    conversation_id_ = conv.id;

    MeetAuthMe me;
    if (MeetApi::Instance().GetAuthMe(me) == ESP_OK && me.account_type == "child") {
        MeetApi::Instance().TeachingPrepareChatOnly(conversation_id_);
    }

    MeetRealtimeSessionConfig session_cfg;
    session_cfg.voice = runtime.voice;
    session_cfg.instructions = runtime.instructions;
    session_cfg.max_history_turns = runtime.max_history_turns;
    err = MeetRealtime::Instance().Open(cfg.selected_character_id, conversation_id_, session_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Realtime open failed: %s", esp_err_to_name(err));
        MeetApi::Instance().CompleteConversation(conversation_id_, 0);
        conversation_id_.clear();
        return;
    }

    PcmPipeline::Instance().StartCapture();
    NoteCallActivity();
    idle_hangup_armed_ = true;
    state_.Set(AppState::InCall);
}

void AppController::LeaveInCall() {
    StopIdleHangupTimer();
    PcmPipeline::Instance().StopCapture();
    MeetRealtime::Instance().Close();
    if (!conversation_id_.empty()) {
        MeetApi::Instance().CompleteConversation(conversation_id_, 0);
        conversation_id_.clear();
    }
    EnterReady();
}

void AppController::NoteCallActivity() {
    last_activity_us_ = esp_timer_get_time();
}

void AppController::StartIdleHangupTimer() {
    idle_hangup_armed_ = true;
    NoteCallActivity();
}

void AppController::StopIdleHangupTimer() {
    idle_hangup_armed_ = false;
}

void AppController::OnIdleHangup() {
    ESP_LOGI(TAG, "IdleHangup after %d ms", CONFIG_MEET_IDLE_HANGUP_MS);
    LeaveInCall();
}

void AppController::Tick() {
    if (state_.Get() == AppState::Pairing && !pairing_session_id_.empty()) {
        MeetPairingPollResult result;
        if (MeetApi::Instance().PollPairingSession(pairing_session_id_, result) == ESP_OK &&
            result.claimed) {
            MeetConfig cfg;
            MeetNvsLoad(cfg);
            cfg.device_credential = result.device_credential;
            cfg.device_id = result.device_id;
            MeetNvsSave(cfg);
            MeetApi::Instance().Configure(cfg.server_origin, cfg.device_credential);
            ESP_LOGI(TAG, "DeviceBinding complete");
            EnterReady();
        }
    }

    if (idle_hangup_armed_ && state_.Get() == AppState::InCall) {
        const int64_t elapsed_ms = (esp_timer_get_time() - last_activity_us_) / 1000;
        if (elapsed_ms >= CONFIG_MEET_IDLE_HANGUP_MS) {
            OnIdleHangup();
        }
    }

    // Continuous uplink while InCall; silence frames do not reset IdleHangup.
    if (state_.Get() == AppState::InCall) {
        PcmFrame frame;
        if (PcmPipeline::Instance().PopCaptureFrame(frame)) {
            MeetRealtime::Instance().SendInputPcm(frame.samples.data(), frame.samples.size());
        }
    }
}

}  // namespace meet
