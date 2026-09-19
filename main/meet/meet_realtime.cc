#include "meet_realtime.h"

#include "meet_api.h"
#include "meet_nvs.h"
#include "pcm_pipeline.h"

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

std::string MakeEventId() {
    uint8_t b[8];
    esp_fill_random(b, sizeof(b));
    char out[32];
    snprintf(out, sizeof(out), "event_%02x%02x%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3],
             b[4], b[5], b[6], b[7]);
    return out;
}

std::string Base64Encode(const uint8_t* data, size_t len) {
    size_t olen = 0;
    mbedtls_base64_encode(nullptr, 0, &olen, data, len);
    std::string out(olen + 1, '\0');
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out.data()), out.size(), &olen, data,
                              len) != 0) {
        return {};
    }
    out.resize(olen);
    return out;
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

esp_err_t MeetRealtime::Open(const std::string& character_id,
                             const std::string& conversation_id,
                             const MeetRealtimeSessionConfig& session) {
    Close();
    session_ = session;
    if (session_.voice.empty() || session_.instructions.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    if (session_.max_history_turns < 1) session_.max_history_turns = 50;
    if (session_.max_history_turns > 50) session_.max_history_turns = 50;

    MeetConfig cfg;
    MeetNvsLoad(cfg);
    if (cfg.server_origin.empty()) {
        return ESP_ERR_INVALID_STATE;
    }

    std::string url = cfg.server_origin;
    if (url.rfind("https://", 0) == 0) {
        url.replace(0, 5, "wss");
    } else if (url.rfind("http://", 0) == 0) {
        url.replace(0, 4, "ws");
    }
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    url += "/api/characters/" + character_id + "/realtime/websocket?conversationId=" + conversation_id;

    auth_header_.clear();
    if (!cfg.device_credential.empty()) {
        auth_header_ = "Authorization: Bearer " + cfg.device_credential + "\r\n";
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

void MeetRealtime::Close() {
    ready_ = false;
    connected_ = false;
    relay_ready_ = false;
    session_sent_ = false;
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
            ESP_LOGI(TAG, "connected");
            self->connected_ = true;
            // If backend has no relay.ready gate, treat connect as ready-to-configure.
            self->relay_ready_ = true;
            self->SendSessionUpdate();
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "disconnected");
            self->connected_ = false;
            self->ready_ = false;
            self->relay_ready_ = false;
            self->session_sent_ = false;
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

    if (strncmp(t, "relay.", 6) == 0) {
        if (strcmp(t, "relay.ready") == 0) {
            relay_ready_ = true;
            SendSessionUpdate();
        }
        cJSON_Delete(root);
        return;
    }

    if (strcmp(t, "session.updated") == 0) {
        ready_ = true;
        ESP_LOGI(TAG, "session configured");
        if (on_activity_) on_activity_();
    } else if (strcmp(t, "input_audio_buffer.speech_started") == 0) {
        PcmPipeline::Instance().ClearGeneration();
        SendResponseCancel();
        if (on_speech_started_) {
            on_speech_started_();
        }
        if (on_activity_) on_activity_();
    } else if (strcmp(t, "response.audio.delta") == 0) {
        const cJSON* response_id = cJSON_GetObjectItem(root, "response_id");
        const cJSON* delta = cJSON_GetObjectItem(root, "delta");
        if (cJSON_IsString(delta)) {
            auto pcm = Base64Decode(delta->valuestring);
            const std::string rid = cJSON_IsString(response_id) ? response_id->valuestring : "";
            if (!pcm.empty()) {
                PcmPipeline::Instance().EnqueuePlayback(rid, pcm.data(), pcm.size());
                if (on_audio_delta_) {
                    on_audio_delta_(rid, pcm.data(), pcm.size());
                }
                if (on_activity_) on_activity_();
            }
        }
    } else if (strcmp(t, "response.done") == 0 ||
               strcmp(t, "response.audio_transcript.done") == 0 ||
               strcmp(t, "input_audio_buffer.speech_stopped") == 0) {
        if (on_activity_) on_activity_();
    }

    cJSON_Delete(root);
}

esp_err_t MeetRealtime::SendJson(const std::string& json) {
    if (!client_ || !connected_) {
        return ESP_ERR_INVALID_STATE;
    }
    const int n = esp_websocket_client_send_text(static_cast<esp_websocket_client_handle_t>(client_),
                                                 json.c_str(), json.size(), pdMS_TO_TICKS(1000));
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
    cJSON_AddNumberToObject(session, "max_history_turns", session_.max_history_turns);
    cJSON* turn = cJSON_AddObjectToObject(session, "turn_detection");
    cJSON_AddStringToObject(turn, "type", "smart_turn");
    char* raw = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!raw) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = SendJson(raw);
    cJSON_free(raw);
    return err;
}

esp_err_t MeetRealtime::SendInputPcm(const int16_t* samples, size_t sample_count) {
    if (!ready_ || !samples || sample_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    const auto b64 = Base64Encode(reinterpret_cast<const uint8_t*>(samples),
                                  sample_count * sizeof(int16_t));
    if (b64.empty()) {
        return ESP_FAIL;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "event_id", MakeEventId().c_str());
    cJSON_AddStringToObject(root, "type", "input_audio_buffer.append");
    cJSON_AddStringToObject(root, "audio", b64.c_str());
    char* raw = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!raw) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = SendJson(raw);
    cJSON_free(raw);
    return err;
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
    esp_err_t err = SendJson(raw);
    cJSON_free(raw);
    return err;
}

}  // namespace meet
