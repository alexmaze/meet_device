#include "app_controller.h"

#include "board.h"
#include "meet_api.h"
#include "meet_nvs.h"
#include "meet_realtime.h"
#include "pcm_pipeline.h"
#include "ui.h"
#include "wake_word.h"
#include "wifi_service.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include <vector>

namespace meet {
namespace {

constexpr char TAG[] = "app_ctrl";
constexpr int64_t kPairingTtlUs = 5LL * 60 * 1000000;
constexpr int64_t kPairPollUs = 1500 * 1000;
constexpr int64_t kConnectTimeoutUs = 12LL * 1000000;
constexpr int64_t kRelayFallbackUs = 4LL * 1000000;

const char* kSettingsItems[] = {
    "选择角色",
    "重新配对",
    "重新配网",
    "横竖屏",
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
                if (WifiService::Instance().phase() == WifiPhase::ConfigAp) {
                    UiShowWifiConfig(WifiService::Instance().ap_ssid().c_str(),
                                     WifiService::Instance().ap_url().c_str());
                } else if (WifiService::Instance().phase() == WifiPhase::ConnectingSta) {
                    UiShowWifiConnecting(WifiService::Instance().sta_ssid().c_str());
                } else {
                    UiShowUnprovisioned();
                }
                break;
            case AppState::Pairing:
                UiShowPairing(pairing_code_.c_str(), pairing_hint_.c_str());
                break;
            case AppState::Ready:
                UiShowReady(character_name_.empty() ? nullptr : character_name_.c_str());
                break;
            case AppState::Connecting:
                UiShowConnecting();
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
    MeetRealtime::Instance().SetDisconnectedHandler([this]() { disconnect_seen_ = true; });

    WakeWord::Instance().SetOnDetected([this]() {
        if (state_.Get() == AppState::Ready) {
            ESP_LOGI(TAG, "WakeWord detected -> Connecting");
            EnterConnecting();
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
    character_name_ = cfg.selected_character_name;

    Board::Instance().SetBootClickHandler([this]() { OnBootClick(); });
    Board::Instance().SetBootDoubleClickHandler([this]() { OnBootDoubleClick(); });
    Board::Instance().SetBootLongPressHandler([this]() { OnBootLongPress(); });

    EnterUnprovisioned();
    WifiService::Instance().Start();
    HandleWifiPhase();
    return ESP_OK;
}

void AppController::ApplyOnlineState() {
    MeetConfig cfg;
    MeetNvsLoad(cfg);
    MeetApi::Instance().Configure(
        cfg.server_origin.empty() ? CONFIG_MEET_SERVER_URL : cfg.server_origin,
        cfg.device_credential);
    if (cfg.device_credential.empty()) {
        EnterPairing();
    } else {
        character_name_ = cfg.selected_character_name;
        EnterReady();
    }
}

void AppController::HandleWifiPhase() {
    switch (WifiService::Instance().phase()) {
        case WifiPhase::ConfigAp:
            if (state_.Get() == AppState::InCall || state_.Get() == AppState::Connecting) {
                LeaveInCall();
            }
            EnterUnprovisioned();
            UiShowWifiConfig(WifiService::Instance().ap_ssid().c_str(),
                             WifiService::Instance().ap_url().c_str());
            break;
        case WifiPhase::ConnectingSta:
            EnterUnprovisioned();
            UiShowWifiConnecting(WifiService::Instance().sta_ssid().c_str());
            break;
        case WifiPhase::Connected:
            ApplyOnlineState();
            break;
        case WifiPhase::Failed:
            EnterUnprovisioned();
            UiShowUnprovisioned();
            break;
        case WifiPhase::Idle:
            break;
    }
}

void AppController::OnBootClick() {
    NoteCallActivity();
    switch (state_.Get()) {
        case AppState::Ready:
            EnterConnecting();
            break;
        case AppState::Connecting:
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
    } else if (state_.Get() == AppState::Ready || state_.Get() == AppState::InCall ||
               state_.Get() == AppState::Connecting) {
        if (state_.Get() == AppState::InCall || state_.Get() == AppState::Connecting) {
            LeaveInCall();
        }
        EnterSettings();
    }
}

void AppController::OnBootLongPress() {
    ESP_LOGI(TAG, "long press → SoftAP");
    if (state_.Get() == AppState::InCall || state_.Get() == AppState::Connecting) {
        LeaveInCall();
    }
    WifiService::Instance().EnterConfigMode();
    HandleWifiPhase();
}

void AppController::EnterUnprovisioned() {
    state_.Set(AppState::Unprovisioned);
}

void AppController::EnterPairing() {
    pairing_code_ = "------";
    pairing_hint_ = "正在申请配对码";
    pairing_session_id_.clear();
    pairing_deadline_us_ = 0;
    last_pair_poll_us_ = 0;
    state_.Set(AppState::Pairing);

    MeetPairingSession session;
    esp_err_t err =
        MeetApi::Instance().CreatePairingSession(session, WifiService::Instance().display_name());
    if (err != ESP_OK) {
        pairing_hint_ = "申请失败，稍后重试";
        UiShowPairing(pairing_code_.c_str(), pairing_hint_.c_str());
        pairing_deadline_us_ = esp_timer_get_time() + 8 * 1000000LL;
        return;
    }
    pairing_code_ = session.code.empty() ? "------" : session.code;
    pairing_session_id_ = session.id;
    pairing_hint_ = "在网页输入配对码";
    pairing_deadline_us_ = esp_timer_get_time() + kPairingTtlUs;
    UiShowPairing(pairing_code_.c_str(), pairing_hint_.c_str());
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
        case 0: {
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
            }
            EnterReady();
            break;
        }
        case 1: {
            MeetConfig cfg;
            MeetNvsLoad(cfg);
            cfg.device_credential.clear();
            cfg.device_id.clear();
            MeetNvsSave(cfg);
            MeetApi::Instance().Configure(cfg.server_origin, "");
            if (WifiService::Instance().phase() == WifiPhase::Connected) {
                EnterPairing();
            } else {
                EnterUnprovisioned();
            }
            break;
        }
        case 2:
            WifiService::Instance().EnterConfigMode();
            HandleWifiPhase();
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

void AppController::EnterConnecting() {
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
    disconnect_seen_ = false;
    err = MeetRealtime::Instance().Open(cfg.selected_character_id, conversation_id_, session_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Realtime open failed: %s", esp_err_to_name(err));
        MeetApi::Instance().CompleteConversation(conversation_id_, 0);
        conversation_id_.clear();
        return;
    }

    connecting_started_us_ = esp_timer_get_time();
    state_.Set(AppState::Connecting);
}

void AppController::FinishConnectingToInCall() {
    PcmPipeline::Instance().StartCapture();
    NoteCallActivity();
    idle_hangup_armed_ = true;
    state_.Set(AppState::InCall);
}

void AppController::FailConnecting() {
    ESP_LOGW(TAG, "Connecting failed / timeout");
    PcmPipeline::Instance().StopCapture();
    MeetRealtime::Instance().Close();
    if (!conversation_id_.empty()) {
        MeetApi::Instance().CompleteConversation(conversation_id_, 0);
        conversation_id_.clear();
    }
    EnterReady();
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
    if (WifiService::Instance().ConsumePhaseChange()) {
        HandleWifiPhase();
    }

    if (state_.Get() == AppState::Pairing) {
        const int64_t now = esp_timer_get_time();
        if (pairing_deadline_us_ > 0 && now >= pairing_deadline_us_) {
            ESP_LOGW(TAG, "pairing expired or retry");
            EnterPairing();
            return;
        }
        if (!pairing_session_id_.empty() && now - last_pair_poll_us_ >= kPairPollUs) {
            last_pair_poll_us_ = now;
            MeetPairingPollResult result;
            if (MeetApi::Instance().PollPairingSession(pairing_session_id_, result) == ESP_OK) {
                if (result.expired) {
                    pairing_hint_ = "配对码过期，正在重申";
                    UiShowPairing(pairing_code_.c_str(), pairing_hint_.c_str());
                    EnterPairing();
                    return;
                }
                if (result.claimed) {
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
        }
    }

    if (state_.Get() == AppState::Connecting) {
        if (disconnect_seen_) {
            disconnect_seen_ = false;
            FailConnecting();
            return;
        }
        const int64_t elapsed = esp_timer_get_time() - connecting_started_us_;
        if (MeetRealtime::Instance().IsReady()) {
            FinishConnectingToInCall();
        } else if (elapsed >= kConnectTimeoutUs) {
            FailConnecting();
        } else if (elapsed >= kRelayFallbackUs) {
            MeetRealtime::Instance().AssumeRelayReady();
        }
    }

    if (disconnect_seen_ && state_.Get() == AppState::InCall) {
        disconnect_seen_ = false;
        LeaveInCall();
        return;
    }

    if (idle_hangup_armed_ && state_.Get() == AppState::InCall) {
        const int64_t elapsed_ms = (esp_timer_get_time() - last_activity_us_) / 1000;
        if (elapsed_ms >= CONFIG_MEET_IDLE_HANGUP_MS) {
            OnIdleHangup();
        }
    }

    if (state_.Get() == AppState::InCall) {
        PcmFrame frame;
        if (PcmPipeline::Instance().PopCaptureFrame(frame)) {
            MeetRealtime::Instance().SendInputPcm(frame.samples.data(), frame.samples.size());
        }
    }
}

}  // namespace meet
