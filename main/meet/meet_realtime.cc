#include "meet_realtime.h"

#include "app_event.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <mbedtls/base64.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace meet {
namespace {

constexpr char TAG[] = "meet_rt";
constexpr TickType_t kUplinkTimeout = pdMS_TO_TICKS(40);
constexpr TickType_t kCtrlTimeout = pdMS_TO_TICKS(1000);

std::string MakeEventId() {
    uint8_t b[8];
    esp_fill_random(b, sizeof(b));
    char out[32];
    snprintf(out, sizeof(out), "event_%02x%02x%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3],
             b[4], b[5], b[6], b[7]);
    return out;
}

bool Base64EncodeTo(const uint8_t* data, size_t len, std::string& out) {
    size_t olen = 0;
    mbedtls_base64_encode(nullptr, 0, &olen, data, len);
    out.resize(olen);
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out.data()), out.size(), &olen, data,
                              len) != 0) {
        out.clear();
        return false;
    }
    out.resize(olen);
    return true;
}

std::vector<uint8_t> Base64Decode(const char* b64) {
    size_t olen = 0;
    const size_t in_len = strlen(b64);
    mbedtls_base64_decode(nullptr, 0, &olen, reinterpret_cast<const unsigned char*>(b64), in_len);
    std::vector<uint8_t> out(olen);
    if (mbedtls_base64_decode(out.data(), out.size(), &olen,
                              reinterpret_cast<const unsigned char*>(b64), in_len) != 0) {
        return {};
    }
    out.resize(olen);
    return out;
}

void PostSimple(AppEventType type) {
    AppEvent ev;
    ev.type = type;
    AppEventPost(ev);
}

void PostText(AppEventType type, const char* text) {
    AppEvent ev;
    ev.type = type;
    if (text) {
        strncpy(ev.text, text, sizeof(ev.text) - 1);
    }
    AppEventPost(ev);
}

}  // namespace

MeetRealtime& MeetRealtime::Instance() {
    static MeetRealtime rt;
    return rt;
}

void MeetRealtime::SetAudioDeltaHandler(MeetRealtimeAudioDeltaCb cb) {
    on_audio_delta_ = std::move(cb);
}
void MeetRealtime::SetSpeechStartedHandler(MeetRealtimeSpeechStartedCb cb) {
    on_speech_started_ = std::move(cb);
}
void MeetRealtime::SetActivityHandler(MeetRealtimeActivityCb cb) {
    on_activity_ = std::move(cb);
}
void MeetRealtime::SetDisconnectedHandler(MeetRealtimeDisconnectedCb cb) {
    on_disconnected_ = std::move(cb);
}
void MeetRealtime::SetEmotionHandler(MeetRealtimeEmotionCb cb) {
    on_emotion_ = std::move(cb);
}
void MeetRealtime::SetCaptionHandler(MeetRealtimeCaptionCb cb) {
    on_caption_ = std::move(cb);
}
void MeetRealtime::SetReadyHandler(MeetRealtimeReadyCb cb) {
    on_ready_ = std::move(cb);
}
void MeetRealtime::SetTeachingGateHandler(MeetRealtimeTeachingGateCb cb) {
    on_teaching_gate_ = std::move(cb);
}

esp_err_t MeetRealtime::Open(const std::string& origin,
                             const std::string& bearer_token,
                             const std::string& character_id,
                             const std::string& conversation_id,
                             const MeetRealtimeSessionConfig& session) {
    Close();
    session_ = session;
    if (session_.voice.empty() || session_.instructions.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    session_.max_history_turns = kMeetMaxHistoryTurns;
    if (origin.empty()) {
        return ESP_ERR_INVALID_STATE;
    }

    std::string url = origin;
    if (url.rfind("https://", 0) == 0) {
        url.replace(0, 5, "wss");
    } else if (url.rfind("http://", 0) == 0) {
        url.replace(0, 4, "ws");
    }
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    url += "/api/characters/" + character_id +
           "/realtime/websocket?conversationId=" + conversation_id;

    auth_header_.clear();
    if (!bearer_token.empty()) {
        auth_header_ = "Authorization: Bearer " + bearer_token + "\r\n";
    }

    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = url.c_str();
    ws_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    ws_cfg.buffer_size = 8192;
    ws_cfg.task_stack = 6144;
    if (!auth_header_.empty()) {
        ws_cfg.headers = auth_header_.c_str();
    }

    client_ = esp_websocket_client_init(&ws_cfg);
    if (!client_) {
        return ESP_FAIL;
    }

    esp_websocket_register_events(static_cast<esp_websocket_client_handle_t>(client_),
                                  WEBSOCKET_EVENT_ANY, WebsocketEventHandler, this);

    esp_err_t err = esp_websocket_client_start(static_cast<esp_websocket_client_handle_t>(client_));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "websocket start failed: %s", esp_err_to_name(err));
        Close();
        return err;
    }
    ESP_LOGI(TAG, "connecting %s", url.c_str());
    return ESP_OK;
}

void MeetRealtime::AssumeRelayReady() {
    if (connected_ && !session_sent_) {
        relay_ready_ = true;
        if (!teaching_session_) {
            teaching_gate_open_.store(true);
        }
        SendSessionUpdate();
    }
}

void MeetRealtime::Close() {
    ready_ = false;
    connected_ = false;
    relay_ready_ = false;
    session_sent_ = false;
    teaching_session_ = false;
    teaching_gate_open_.store(true);
    if (client_) {
        esp_websocket_client_close(static_cast<esp_websocket_client_handle_t>(client_),
                                   portMAX_DELAY);
        esp_websocket_client_destroy(static_cast<esp_websocket_client_handle_t>(client_));
        client_ = nullptr;
    }
}

void MeetRealtime::WebsocketEventHandler(void* handler_args,
                                         esp_event_base_t /*base*/,
                                         int32_t event_id,
                                         void* event_data) {
    auto* self = static_cast<MeetRealtime*>(handler_args);
    auto* data = static_cast<esp_websocket_event_data_t*>(event_data);
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "connected; wait relay.ready");
            self->connected_ = true;
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "disconnected");
            self->connected_ = false;
            self->ready_ = false;
            self->relay_ready_ = false;
            self->session_sent_ = false;
            PostSimple(AppEventType::RtDisconnected);
            if (self->on_disconnected_) {
                self->on_disconnected_();
            }
            break;
        case WEBSOCKET_EVENT_DATA:
            if (data && data->op_code == 0x1 && data->data_ptr && data->data_len > 0) {
                self->OnMessage(data->data_ptr, data->data_len);
            }
            break;
        default:
            break;
    }
}

void MeetRealtime::OnMessage(const char* data, int len) {
    cJSON* root = cJSON_ParseWithLength(data, len);
    if (!root) {
        return;
    }
    const cJSON* type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }
    const char* t = type->valuestring;

    if (strcmp(t, "relay.ready") == 0) {
        const cJSON* teaching = cJSON_GetObjectItem(root, "teaching");
        teaching_session_ = cJSON_IsTrue(teaching);
        // Non-teaching relays open the gate immediately; teaching waits for audio_gate.
        teaching_gate_open_.store(!teaching_session_);
        relay_ready_ = true;
        SendSessionUpdate();
        cJSON_Delete(root);
        return;
    }
    if (strcmp(t, "relay.teaching.audio_gate") == 0) {
        const cJSON* revision = cJSON_GetObjectItem(root, "revision");
        const cJSON* open = cJSON_GetObjectItem(root, "open");
        if (cJSON_IsNumber(revision) && cJSON_IsBool(open)) {
            const int rev = revision->valueint;
            const bool is_open = cJSON_IsTrue(open);
            teaching_gate_open_.store(is_open);
            SendTeachingAudioGateAck(rev);
            AppEvent ev;
            ev.type = AppEventType::RtTeachingGate;
            ev.i32 = is_open ? rev : -rev;  // sign encodes open/closed; |i32| = revision
            AppEventPost(ev);
            if (on_teaching_gate_) {
                on_teaching_gate_(rev, is_open);
            }
        }
        cJSON_Delete(root);
        return;
    }
    if (strncmp(t, "relay.", 6) == 0) {
        // Ignore renewal_* / teaching.state / relay.error for now (IdleHangup covers long calls).
        cJSON_Delete(root);
        return;
    }

    if (strcmp(t, "meet.emotion") == 0) {
        const cJSON* name = cJSON_GetObjectItem(root, "name");
        if (cJSON_IsString(name)) {
            PostText(AppEventType::RtEmotion, name->valuestring);
            if (on_emotion_) on_emotion_(name->valuestring);
        }
        if (on_activity_) on_activity_();
    } else if (strcmp(t, "session.updated") == 0) {
        ready_ = true;
        ESP_LOGI(TAG, "session configured");
        PostSimple(AppEventType::RtReady);
        PostText(AppEventType::RtEmotion, "relaxed");
        if (on_ready_) on_ready_();
        if (on_emotion_) on_emotion_("relaxed");
        if (on_activity_) on_activity_();
    } else if (strcmp(t, "input_audio_buffer.speech_started") == 0) {
        // Data-plane: notify app to ClearGeneration; do not touch audio here.
        PostSimple(AppEventType::RtSpeechStarted);
        if (on_speech_started_) on_speech_started_();
        if (on_activity_) on_activity_();
    } else if (strcmp(t, "response.created") == 0) {
        PostText(AppEventType::RtEmotion, "thinking");
        if (on_emotion_) on_emotion_("thinking");
        if (on_activity_) on_activity_();
    } else if (strcmp(t, "response.audio_transcript.delta") == 0) {
        const cJSON* delta = cJSON_GetObjectItem(root, "delta");
        if (cJSON_IsString(delta) && delta->valuestring[0]) {
            PostText(AppEventType::RtCaption, delta->valuestring);
            if (on_caption_) on_caption_(delta->valuestring, false);
        }
        if (on_activity_) on_activity_();
    } else if (strcmp(t, "response.audio_transcript.done") == 0) {
        PostSimple(AppEventType::RtCaptionDone);
        if (on_caption_) on_caption_("", true);
        if (on_activity_) on_activity_();
    } else if (strcmp(t, "response.audio.delta") == 0) {
        const cJSON* response_id = cJSON_GetObjectItem(root, "response_id");
        const cJSON* delta = cJSON_GetObjectItem(root, "delta");
        if (cJSON_IsString(delta)) {
            auto pcm = Base64Decode(delta->valuestring);
            const std::string rid = cJSON_IsString(response_id) ? response_id->valuestring : "";
            if (!pcm.empty() && on_audio_delta_) {
                on_audio_delta_(rid, pcm.data(), pcm.size());
            }
            if (on_activity_) on_activity_();
        }
    } else if (strcmp(t, "response.done") == 0) {
        PostText(AppEventType::RtEmotion, "relaxed");
        if (on_emotion_) on_emotion_("relaxed");
        if (on_activity_) on_activity_();
    } else if (strcmp(t, "input_audio_buffer.speech_stopped") == 0) {
        if (on_activity_) on_activity_();
    }

    cJSON_Delete(root);
}

esp_err_t MeetRealtime::SendJson(const char* json, size_t len, TickType_t timeout) {
    if (!client_ || !connected_ || !json) {
        return ESP_ERR_INVALID_STATE;
    }
    const int n = esp_websocket_client_send_text(
        static_cast<esp_websocket_client_handle_t>(client_), json, len, timeout);
    return n < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t MeetRealtime::SendSessionUpdate() {
    if (!relay_ready_ || session_sent_) {
        return ESP_OK;
    }
    session_sent_ = true;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "event_id", MakeEventId().c_str());
    cJSON_AddStringToObject(root, "type", "session.update");
    cJSON* session = cJSON_AddObjectToObject(root, "session");
    cJSON* modalities = cJSON_AddArrayToObject(session, "modalities");
    cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));
    cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));
    cJSON_AddStringToObject(session, "voice", session_.voice.c_str());
    cJSON_AddStringToObject(session, "input_audio_format", "pcm");
    cJSON_AddStringToObject(session, "output_audio_format", "pcm");
    cJSON_AddStringToObject(session, "instructions", session_.instructions.c_str());
    cJSON_AddNumberToObject(session, "max_history_turns", kMeetMaxHistoryTurns);
    cJSON* turn = cJSON_AddObjectToObject(session, "turn_detection");
    cJSON_AddStringToObject(turn, "type", "smart_turn");
    char* raw = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!raw) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = SendJson(raw, strlen(raw), kCtrlTimeout);
    cJSON_free(raw);
    return err;
}

esp_err_t MeetRealtime::SendInputPcm(const int16_t* samples, size_t sample_count) {
    if (!ready_ || !samples || sample_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!teaching_gate_open_.load()) {
        return ESP_ERR_INVALID_STATE;
    }

    std::string b64;
    if (!Base64EncodeTo(reinterpret_cast<const uint8_t*>(samples), sample_count * sizeof(int16_t),
                        b64)) {
        return ESP_FAIL;
    }

    // Hand-built JSON to avoid per-frame cJSON malloc (50 fps).
    const std::string eid = MakeEventId();
    uplink_json_.clear();
    uplink_json_.reserve(64 + eid.size() + b64.size());
    uplink_json_ += "{\"event_id\":\"";
    uplink_json_ += eid;
    uplink_json_ += "\",\"type\":\"input_audio_buffer.append\",\"audio\":\"";
    uplink_json_ += b64;
    uplink_json_ += "\"}";
    return SendJson(uplink_json_.c_str(), uplink_json_.size(), kUplinkTimeout);
}

esp_err_t MeetRealtime::SendResponseCancel() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "event_id", MakeEventId().c_str());
    cJSON_AddStringToObject(root, "type", "response.cancel");
    char* raw = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!raw) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = SendJson(raw, strlen(raw), kCtrlTimeout);
    cJSON_free(raw);
    return err;
}

esp_err_t MeetRealtime::SendTeachingAudioGateAck(int revision) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "event_id", MakeEventId().c_str());
    cJSON_AddStringToObject(root, "type", "relay.teaching.audio_gate_ack");
    cJSON_AddNumberToObject(root, "revision", revision);
    char* raw = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!raw) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = SendJson(raw, strlen(raw), kCtrlTimeout);
    cJSON_free(raw);
    return err;
}

}  // namespace meet
