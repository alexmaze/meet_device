#include "app_controller.h"

#include "board.h"
#include "meet_api.h"
#include "meet_es8388.h"
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
#include <cstdio>
#include <vector>

namespace meet {
namespace {

constexpr char TAG[] = "app_ctrl";
constexpr int64_t kPairingTtlUs = 5LL * 60 * 1000000;
constexpr int64_t kPairPollUs = 1500 * 1000;
constexpr int64_t kConnectTimeoutUs = 12LL * 1000000;
constexpr int64_t kRelayFallbackUs = 4LL * 1000000;
constexpr int kSettingsCount = 6;

}  // namespace

AppController& AppController::Instance() {
    static AppController ctrl;
    return ctrl;
}

esp_err_t AppController::Start() {
    state_.AddListener([this](AppState /*from*/, AppState to) {
        Board::Instance().SetTalking(to == AppState::InCall);
        switch (to) {
            case AppState::Unprovisioned:
                PcmPipeline::Instance().StopCapture();
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
                PcmPipeline::Instance().StopCapture();
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
                RefreshSettingsUi();
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
    ApplyVolume(cfg.volume, false);
    ApplyOrientation(cfg.landscape, false);

    Board::Instance().SetBootClickHandler([this]() { OnBootClick(); });
    Board::Instance().SetBootDoubleClickHandler([this]() { OnBootDoubleClick(); });
    Board::Instance().SetBootLongPressHandler([this]() { OnBootLongPress(); });
    Board::Instance().SetVolumeKeyHandler([this](int delta) { OnVolumeKey(delta); });

    EnterUnprovisioned();
    WifiService::Instance().Start();
    HandleWifiPhase();
    return ESP_OK;
}

void AppController::ApplyVolume(int volume, bool persist) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    volume_ = volume;
    Es8388Codec::Instance().SetOutputVolume(volume_);
    if (persist) {
        MeetConfig cfg;
        MeetNvsLoad(cfg);
        cfg.volume = volume_;
        MeetNvsSave(cfg);
    }
}

void AppController::ApplyOrientation(bool landscape, bool persist) {
    landscape_ = landscape;
    UiApplyOrientation(landscape_);
    if (persist) {
        MeetConfig cfg;
        MeetNvsLoad(cfg);
        cfg.landscape = landscape_;
        MeetNvsSave(cfg);
    }
}

void AppController::RefreshSettingsUi() {
    char orient[24];
    char vol[16];
    snprintf(orient, sizeof(orient), "横竖屏 · %s", landscape_ ? "横屏" : "竖屏");
    snprintf(vol, sizeof(vol), "音量 %d", volume_);
    const char* items[kSettingsCount] = {
        "选择角色", "重新配对", "重新配网", orient, vol, "返回",
    };
    UiShowSettings(items, kSettingsCount, settings_index_);
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

void AppController::HandleUnauthorized() {
    ESP_LOGW(TAG, "401 -> clear DeviceCredential and re-pair");
    if (state_.Get() == AppState::InCall || state_.Get() == AppState::Connecting) {
        StopIdleHangupTimer();
        PcmPipeline::Instance().StopCapture();
        MeetRealtime::Instance().Close();
        conversation_id_.clear();
    }
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
        settings_index_ = (settings_index_ + 1) % kSettingsCount;
        RefreshSettingsUi();
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

void AppController::OnVolumeKey(int delta) {
    if (state_.Get() == AppState::Settings) {
        if (delta < 0) {
            settings_index_ = (settings_index_ + 1) % kSettingsCount;
        } else {
            settings_index_ = (settings_index_ + kSettingsCount - 1) % kSettingsCount;
        }
        RefreshSettingsUi();
        return;
    }
    ApplyVolume(volume_ + delta, true);
    char toast[24];
    snprintf(toast, sizeof(toast), "音量 %d", volume_);
    UiShowToast(toast);
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
    PcmPipeline::Instance().StartListen();
    state_.Set(AppState::Ready);
}

void AppController::EnterSettings() {
    settings_index_ = 0;
    PcmPipeline::Instance().StopCapture();
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
            ApplyOrientation(!landscape_, true);
            RefreshSettingsUi();
            break;
        case 4: {
            char toast[24];
            snprintf(toast, sizeof(toast), "音量 %d", volume_);
            UiShowToast(toast);
            break;
        }
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

    PcmPipeline::Instance().StopCapture();

    MeetCharacterRuntime runtime;
    esp_err_t err = MeetApi::Instance().GetCharacterRuntime(cfg.selected_character_id, runtime);
    if (MeetApi::Instance().ConsumeUnauthorized()) {
        HandleUnauthorized();
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GetCharacterRuntime failed: %s", esp_err_to_name(err));
        EnterReady();
        return;
    }

    MeetConversation conv;
    err = MeetApi::Instance().CreateConversation(cfg.selected_character_id, "normal", conv);
    if (MeetApi::Instance().ConsumeUnauthorized()) {
        HandleUnauthorized();
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CreateConversation failed: %s", esp_err_to_name(err));
        EnterReady();
        return;
    }
    conversation_id_ = conv.id;

    MeetAuthMe me;
    if (MeetApi::Instance().GetAuthMe(me) == ESP_OK && me.account_type == "child") {
        MeetApi::Instance().TeachingPrepareChatOnly(conversation_id_);
    }
    if (MeetApi::Instance().ConsumeUnauthorized()) {
        HandleUnauthorized();
        return;
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
        EnterReady();
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
    if (MeetApi::Instance().ConsumeUnauthorized()) {
        HandleUnauthorized();
        return;
    }

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
