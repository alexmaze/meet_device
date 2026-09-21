#include "app_controller.h"

#include "audio_cue.h"
#include "audio_pipeline.h"
#include "board.h"
#include "factory_nvs.h"
#include "meet_api.h"
#include "meet_es8388.h"
#include "meet_ota.h"
#include "meet_realtime.h"
#include "settings.h"
#include "ui.h"
#include "wifi_service.h"

#include <sdkconfig.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdio>
#include <cstring>

namespace meet {
namespace {

constexpr char TAG[] = "app_ctrl";
constexpr int64_t kPairingTtlUs = 5LL * 60 * 1000000;
constexpr int64_t kPairPollUs = 1500 * 1000;
constexpr int64_t kConnectTimeoutUs = 12LL * 1000000;
constexpr int64_t kRelayFallbackUs = 4LL * 1000000;
constexpr int kSettingsCount = 4;
constexpr int kWeakRssi = -75;
constexpr int kWeakTicks = 80;
constexpr int64_t kWeakCueCooldownUs = 30LL * 1000000;
constexpr int64_t kDiagIntervalUs = 30LL * 1000000;

const char* kSettingsLabels[kSettingsCount] = {
    "选择角色",
    "重新配对",
    "重新配网",
    "检查更新",
};

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
                AudioPipeline::Instance().StopCapture();
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
                AudioPipeline::Instance().StopCapture();
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

    // Data-plane: realtime → audio (control-plane events are posted inside MeetRealtime).
    MeetRealtime::Instance().SetAudioDeltaHandler(
        [](const std::string& rid, const uint8_t* pcm, size_t bytes) {
            AudioPipeline::Instance().EnqueuePlayback(rid, pcm, bytes);
        });
    MeetRealtime::Instance().SetSpeechStartedHandler([]() {
        AudioPipeline::Instance().ClearGeneration();
        MeetRealtime::Instance().SendResponseCancel();
    });
    MeetRealtime::Instance().SetActivityHandler([this]() { NoteCallActivity(); });

    Settings::Instance().Load();
    volume_ = Settings::Instance().volume();
    Es8388Codec::Instance().SetOutputVolume(volume_);
    MeetApi::Instance().Configure(Settings::Instance().server_origin(),
                                  Settings::Instance().device_credential());
    character_name_ = Settings::Instance().selected_character_name();

    EnterUnprovisioned();
    WifiService::Instance().Start();
    return ESP_OK;
}

void AppController::RunLoop() {
    auto* q = AppEventQueue();
    while (true) {
        AppEvent ev;
        if (q && xQueueReceive(q, &ev, pdMS_TO_TICKS(100)) == pdTRUE) {
            HandleEvent(ev);
        }
        PeriodicWork();
    }
}

void AppController::HandleEvent(const AppEvent& ev) {
    switch (ev.type) {
        case AppEventType::BootClick:
            OnBootClick();
            break;
        case AppEventType::BootDoubleClick:
            OnBootDoubleClick();
            break;
        case AppEventType::VolumeKey:
            OnVolumeKey(static_cast<int>(ev.i32));
            break;
        case AppEventType::WakeDetected:
            if (state_.Get() == AppState::Ready) {
                EnterConnecting();
            }
            break;
        case AppEventType::WifiPhaseChanged:
            HandleWifiPhase(static_cast<WifiPhase>(ev.i32));
            break;
        case AppEventType::WifiDropped:
            PlayAudioCue(AudioCue::Lost);
            UiShowToast("网络已断开");
            break;
        case AppEventType::WifiConnectRequested:
            WifiService::Instance().ConnectAfterProvision();
            break;
        case AppEventType::RtReady:
            if (state_.Get() == AppState::Connecting) {
                FinishConnectingToInCall();
            }
            break;
        case AppEventType::RtDisconnected:
            if (state_.Get() == AppState::Connecting) {
                FailConnecting();
            } else if (state_.Get() == AppState::InCall) {
                LeaveInCall();
            }
            break;
        case AppEventType::RtSpeechStarted:
            caption_.clear();
            caption_sentence_done_ = true;
            UiClearCaption();
            NoteCallActivity();
            break;
        case AppEventType::RtEmotion:
            UiSetEmotion(ev.text);
            NoteCallActivity();
            break;
        case AppEventType::RtCaption:
            if (caption_sentence_done_) {
                caption_.clear();
                caption_sentence_done_ = false;
            }
            caption_ += ev.text;
            if (caption_.size() > 120) {
                caption_ = caption_.substr(caption_.size() - 120);
            }
            UiSetCaption(caption_.c_str());
            NoteCallActivity();
            break;
        case AppEventType::RtCaptionDone:
            caption_sentence_done_ = true;
            NoteCallActivity();
            break;
        case AppEventType::RtTeachingGate:
            NoteCallActivity();
            break;
        case AppEventType::Unauthorized:
            HandleUnauthorized();
            break;
    }
}

void AppController::ApplyVolume(int volume, bool persist) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    volume_ = volume;
    Es8388Codec::Instance().SetOutputVolume(volume_);
    if (persist) {
        Settings::Instance().SetVolume(volume_);
    }
}

void AppController::RefreshSettingsUi() {
    if (char_browse_mode_ && !char_list_.empty()) {
        char body[320] = {};
        size_t used = 0;
        for (size_t i = 0; i < char_list_.size() && used + 40 < sizeof(body); ++i) {
            const char* prefix = (i == char_browse_index_) ? "> " : "  ";
            used += snprintf(body + used, sizeof(body) - used, "%s%s%s", prefix,
                             char_list_[i].name.c_str(),
                             (i + 1 < char_list_.size()) ? "\n" : "");
        }
        UiShowSettings(body);
        return;
    }
    char body[320] = {};
    size_t used = 0;
    for (int i = 0; i < kSettingsCount; ++i) {
        const char* prefix = (i == settings_index_) ? "> " : "  ";
        used += snprintf(body + used, sizeof(body) - used, "%s%s%s", prefix, kSettingsLabels[i],
                         (i + 1 < kSettingsCount) ? "\n" : "");
    }
    UiShowSettings(body);
}

void AppController::ApplyOnlineState() {
    MeetApi::Instance().Configure(Settings::Instance().server_origin(),
                                  Settings::Instance().device_credential());
    if (Settings::Instance().device_credential().empty()) {
        EnterPairing();
    } else {
        character_name_ = Settings::Instance().selected_character_name();
        EnterReady();
    }
}

void AppController::HandleWifiPhase(WifiPhase phase) {
    switch (phase) {
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
            if (!was_connected_) {
                PlayAudioCue(AudioCue::Connected);
            }
            was_connected_ = true;
            ApplyOnlineState();
            break;
        case WifiPhase::Failed:
            was_connected_ = false;
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
        AudioPipeline::Instance().StopCapture();
        MeetRealtime::Instance().Close();
        conversation_id_.clear();
    }
    Settings::Instance().ClearCredential();
    InvalidateRuntimeCache();
    MeetApi::Instance().Configure(Settings::Instance().server_origin(), "");
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
        default:
            break;
    }
}

void AppController::OnBootDoubleClick() {
    NoteCallActivity();
    if (state_.Get() == AppState::Settings) {
        char_browse_mode_ = false;
        EnterReady();
        return;
    }
    if (state_.Get() == AppState::InCall || state_.Get() == AppState::Connecting) {
        LeaveInCall();
    }
    if (state_.Get() == AppState::Ready || state_.Get() == AppState::Pairing) {
        EnterSettings();
    }
}

void AppController::OnVolumeKey(int delta) {
    if (state_.Get() == AppState::Settings) {
        if (char_browse_mode_ && !char_list_.empty()) {
            if (delta < 0) {
                char_browse_index_ = (char_browse_index_ + 1) % char_list_.size();
            } else {
                char_browse_index_ =
                    (char_browse_index_ + char_list_.size() - 1) % char_list_.size();
            }
        } else {
            if (delta < 0) {
                settings_index_ = (settings_index_ + 1) % kSettingsCount;
            } else {
                settings_index_ = (settings_index_ + kSettingsCount - 1) % kSettingsCount;
            }
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

void AppController::InvalidateRuntimeCache() {
    runtime_cache_valid_ = false;
    runtime_cache_ = MeetCharacterRuntime{};
}

void AppController::PrefetchRuntime() {
    const std::string id = Settings::Instance().selected_character_id();
    if (id.empty()) {
        InvalidateRuntimeCache();
        return;
    }
    MeetCharacterRuntime runtime;
    if (MeetApi::Instance().GetCharacterRuntime(id, runtime) == ESP_OK) {
        runtime_cache_ = std::move(runtime);
        runtime_cache_valid_ = true;
        ESP_LOGI(TAG, "runtime cached for %s", id.c_str());
    } else {
        InvalidateRuntimeCache();
    }
}

void AppController::EnterReady() {
    StopIdleHangupTimer();
    char_browse_mode_ = false;
    if (!Settings::Instance().selected_character_id().empty()) {
        character_name_ = Settings::Instance().selected_character_name();
    } else {
        std::vector<MeetCharacter> chars;
        if (MeetApi::Instance().ListCharacters(chars) == ESP_OK && !chars.empty()) {
            Settings::Instance().SetSelectedCharacter(chars.front().id, chars.front().name);
            MeetApi::Instance().UpdateSelectedCharacter(chars.front().id);
            character_name_ = chars.front().name;
            InvalidateRuntimeCache();
        } else {
            character_name_.clear();
        }
    }
    PrefetchRuntime();
    AudioPipeline::Instance().StartListen();
    UiSetEmotion("neutral");
    caption_.clear();
    caption_sentence_done_ = true;
    UiClearCaption();
    state_.Set(AppState::Ready);
    MaybeReportIdentity();
    MaybeCheckFirmware(false);
    MarkOtaValidIfNeeded();
}

void AppController::EnterSettings() {
    settings_index_ = 0;
    char_browse_mode_ = false;
    AudioPipeline::Instance().StopCapture();
    state_.Set(AppState::Settings);
}

void AppController::HandleSettingsActivate() {
    if (char_browse_mode_) {
        if (!char_list_.empty() && char_browse_index_ < char_list_.size()) {
            const auto& ch = char_list_[char_browse_index_];
            MeetApi::Instance().UpdateSelectedCharacter(ch.id);
            Settings::Instance().SetSelectedCharacter(ch.id, ch.name);
            character_name_ = ch.name;
            InvalidateRuntimeCache();
        }
        char_browse_mode_ = false;
        EnterReady();
        return;
    }

    switch (settings_index_) {
        case 0: {  // 选择角色 — browse list
            char_list_.clear();
            if (MeetApi::Instance().ListCharacters(char_list_) == ESP_OK && !char_list_.empty()) {
                char_browse_mode_ = true;
                char_browse_index_ = 0;
                const std::string cur = Settings::Instance().selected_character_id();
                for (size_t i = 0; i < char_list_.size(); ++i) {
                    if (char_list_[i].id == cur) {
                        char_browse_index_ = i;
                        break;
                    }
                }
                RefreshSettingsUi();
            } else {
                UiShowToast("无法获取角色");
            }
            break;
        }
        case 1: {  // 重新配对
            Settings::Instance().ClearCredential();
            InvalidateRuntimeCache();
            MeetApi::Instance().Configure(Settings::Instance().server_origin(), "");
            if (WifiService::Instance().phase() == WifiPhase::Connected) {
                EnterPairing();
            } else {
                EnterUnprovisioned();
            }
            break;
        }
        case 2:  // 重新配网
            WifiService::Instance().EnterConfigMode();
            break;
        case 3:  // 检查更新
            MaybeCheckFirmware(true);
            RefreshSettingsUi();
            break;
        default:
            EnterReady();
            break;
    }
}

void AppController::EnterConnecting() {
    // Feedback first — before slow HTTP.
    state_.Set(AppState::Connecting);
    AudioPipeline::Instance().StopCapture();

    const std::string char_id = Settings::Instance().selected_character_id();
    if (char_id.empty()) {
        ESP_LOGW(TAG, "No SelectedCharacter");
        UiShowToast("请先选择角色");
        EnterReady();
        return;
    }

    if (!runtime_cache_valid_ || runtime_cache_.character_id != char_id) {
        PrefetchRuntime();
    }
    if (!runtime_cache_valid_) {
        ESP_LOGW(TAG, "GetCharacterRuntime failed");
        EnterReady();
        return;
    }

    MeetConversation conv;
    esp_err_t err = MeetApi::Instance().CreateConversation(char_id, "normal", conv);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CreateConversation failed: %s", esp_err_to_name(err));
        EnterReady();
        return;
    }
    conversation_id_ = conv.id;

    if (Settings::Instance().account_type() == "child") {
        MeetApi::Instance().TeachingPrepareChatOnly(conversation_id_);
    }

    MeetRealtimeSessionConfig session_cfg;
    session_cfg.voice = runtime_cache_.voice;
    session_cfg.instructions = runtime_cache_.instructions;
    session_cfg.max_history_turns = kMeetMaxHistoryTurns;
    err = MeetRealtime::Instance().Open(Settings::Instance().server_origin(),
                                        Settings::Instance().device_credential(), char_id,
                                        conversation_id_, session_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Realtime open failed: %s", esp_err_to_name(err));
        MeetApi::Instance().CompleteConversation(conversation_id_, 0);
        conversation_id_.clear();
        EnterReady();
        return;
    }

    connecting_started_us_ = esp_timer_get_time();
}

void AppController::FinishConnectingToInCall() {
    AudioPipeline::Instance().StartCapture();
    NoteCallActivity();
    idle_hangup_armed_ = true;
    state_.Set(AppState::InCall);
}

void AppController::FailConnecting() {
    ESP_LOGW(TAG, "Connecting failed / timeout");
    AudioPipeline::Instance().StopCapture();
    MeetRealtime::Instance().Close();
    if (!conversation_id_.empty()) {
        MeetApi::Instance().CompleteConversation(conversation_id_, 0);
        conversation_id_.clear();
    }
    EnterReady();
}

void AppController::LeaveInCall() {
    StopIdleHangupTimer();
    AudioPipeline::Instance().StopCapture();
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

void AppController::StopIdleHangupTimer() {
    idle_hangup_armed_ = false;
}

void AppController::OnIdleHangup() {
    ESP_LOGI(TAG, "IdleHangup after %d ms", CONFIG_MEET_IDLE_HANGUP_MS);
    LeaveInCall();
}

void AppController::MaybeReportIdentity() {
    if (identity_reported_) {
        return;
    }
    std::string serial;
    FactorySerialLoad(serial);
    if (MeetApi::Instance().ReportIdentity(serial, CONFIG_MEET_FIRMWARE_VERSION) == ESP_OK) {
        identity_reported_ = true;
        ESP_LOGI(TAG, "reported %s fw %s", serial.c_str(), CONFIG_MEET_FIRMWARE_VERSION);
    }
}

void AppController::MarkOtaValidIfNeeded() {
    if (ota_marked_valid_) {
        return;
    }
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "OTA image marked valid");
    }
#endif
    ota_marked_valid_ = true;
}

void AppController::MaybeCheckFirmware(bool from_settings) {
    if (MeetOtaBusy()) {
        UiShowToast("正在更新");
        return;
    }
    if (!from_settings && firmware_checked_) {
        return;
    }
    firmware_checked_ = true;
    std::string serial;
    FactorySerialLoad(serial);
    MeetFirmwareInfo info;
    if (MeetApi::Instance().CheckFirmware(CONFIG_MEET_FIRMWARE_VERSION, serial, info) != ESP_OK) {
        if (from_settings) {
            UiShowToast("检查更新失败");
        }
        return;
    }
    if (!info.available || info.url.empty()) {
        if (from_settings) {
            UiShowToast("已是最新版本");
        }
        return;
    }
    char toast[48];
    snprintf(toast, sizeof(toast), "新版本 %s", info.version.c_str());
    UiShowToast(toast);
    if (from_settings || info.force) {
        UiShowToast("开始更新…");
        if (MeetOtaStart(info) != ESP_OK) {
            UiShowToast("更新启动失败");
        }
    }
}

void AppController::HandleWeakNet() {
    if (WifiService::Instance().phase() != WifiPhase::Connected) {
        weak_ticks_ = 0;
        return;
    }
    const int rssi = WifiService::Instance().rssi();
    if (rssi >= 0 || rssi > kWeakRssi) {
        weak_ticks_ = 0;
        return;
    }
    weak_ticks_ += 1;
    const int64_t now = esp_timer_get_time();
    if (weak_ticks_ >= kWeakTicks && now - last_weak_cue_us_ >= kWeakCueCooldownUs) {
        last_weak_cue_us_ = now;
        weak_ticks_ = 0;
        PlayAudioCue(AudioCue::Weak);
        UiShowToast("网络较弱");
    }
}

void AppController::PrintDiagnostics() {
    const uint32_t uplink = AudioPipeline::Instance().TakeUplinkFrameCount();
    const size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "diag uplink_fps~%u heap_int=%u spiram=%u state=%s",
             static_cast<unsigned>(uplink / 30), static_cast<unsigned>(free_int),
             static_cast<unsigned>(free_spiram), AppStateName(state_.Get()));
}

void AppController::PeriodicWork() {
    HandleWeakNet();

    const int64_t now = esp_timer_get_time();
    if (now - last_diag_us_ >= kDiagIntervalUs) {
        last_diag_us_ = now;
        PrintDiagnostics();
    }

    if (state_.Get() == AppState::Ready &&
        WifiService::Instance().phase() == WifiPhase::Connected) {
        MaybeReportIdentity();
    }

    if (state_.Get() == AppState::Pairing) {
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
                    Settings::Instance().SetDeviceCredential(result.device_credential,
                                                             result.device_id);
                    MeetApi::Instance().Configure(Settings::Instance().server_origin(),
                                                  result.device_credential);
                    MeetAuthMe me;
                    if (MeetApi::Instance().GetAuthMe(me) == ESP_OK) {
                        Settings::Instance().SetAccountType(me.account_type);
                    }
                    ESP_LOGI(TAG, "DeviceBinding complete");
                    EnterReady();
                }
            }
        }
    }

    if (state_.Get() == AppState::Connecting) {
        const int64_t elapsed = now - connecting_started_us_;
        if (MeetRealtime::Instance().IsReady()) {
            FinishConnectingToInCall();
        } else if (elapsed >= kConnectTimeoutUs) {
            FailConnecting();
        } else if (elapsed >= kRelayFallbackUs) {
            MeetRealtime::Instance().AssumeRelayReady();
        }
    }

    if (idle_hangup_armed_ && state_.Get() == AppState::InCall) {
        const int64_t elapsed_ms = (now - last_activity_us_) / 1000;
        if (elapsed_ms >= CONFIG_MEET_IDLE_HANGUP_MS) {
            OnIdleHangup();
        }
    }
}

}  // namespace meet
