#include "meet_api.h"

#include "app_event.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_random.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace meet {
namespace {

constexpr char TAG[] = "meet_api";
constexpr int kMaxResponseBytes = 64 * 1024;

struct HttpBuffer {
    std::vector<char> data;
};

esp_err_t HttpEventHandler(esp_http_client_event_t* evt) {
    auto* buf = static_cast<HttpBuffer*>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        if (buf->data.size() + evt->data_len > static_cast<size_t>(kMaxResponseBytes)) {
            return ESP_FAIL;
        }
        const char* p = static_cast<const char*>(evt->data);
        buf->data.insert(buf->data.end(), p, p + evt->data_len);
    }
    return ESP_OK;
}

std::string MakeUuidV4() {
    uint8_t b[16];
    esp_fill_random(b, sizeof(b));
    b[6] = static_cast<uint8_t>((b[6] & 0x0f) | 0x40);
    b[8] = static_cast<uint8_t>((b[8] & 0x3f) | 0x80);
    char out[37];
    snprintf(out, sizeof(out),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
             b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14],
             b[15]);
    return out;
}

void NotifyUnauthorized() {
    AppEvent ev;
    ev.type = AppEventType::Unauthorized;
    AppEventPost(ev);
}

}  // namespace

MeetApi& MeetApi::Instance() {
    static MeetApi api;
    return api;
}

void MeetApi::Configure(const std::string& server_origin, const std::string& bearer_token) {
    origin_ = server_origin;
    while (!origin_.empty() && origin_.back() == '/') {
        origin_.pop_back();
    }
    bearer_ = bearer_token;
}

esp_err_t MeetApi::HttpJson(const char* method,
                            const std::string& path,
                            const char* body_json,
                            std::string& response_body,
                            int* status_out) {
    if (origin_.empty()) {
        return ESP_ERR_INVALID_STATE;
    }

    const std::string url = origin_ + path;
    HttpBuffer buf;

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 15000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.event_handler = HttpEventHandler;
    config.user_data = &buf;

    if (strcmp(method, "POST") == 0) {
        config.method = HTTP_METHOD_POST;
    } else if (strcmp(method, "PATCH") == 0) {
        config.method = HTTP_METHOD_PATCH;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (!bearer_.empty()) {
        const std::string auth = "Bearer " + bearer_;
        esp_http_client_set_header(client, "Authorization", auth.c_str());
    }
    if (body_json) {
        esp_http_client_set_post_field(client, body_json, strlen(body_json));
    }

    esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    if (status_out) {
        *status_out = status;
    }
    if (err == ESP_OK) {
        response_body.assign(buf.data.begin(), buf.data.end());
        if (status == 401 && !bearer_.empty()) {
            ESP_LOGW(TAG, "%s %s -> 401", method, path.c_str());
            NotifyUnauthorized();
        }
        if (status < 200 || status >= 300) {
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGW(TAG, "%s %s failed: %s", method, path.c_str(), esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t MeetApi::CreatePairingSession(MeetPairingSession& out, const std::string& display_name) {
    out = MeetPairingSession{};
    cJSON* req = cJSON_CreateObject();
    if (!display_name.empty()) {
        cJSON_AddStringToObject(req, "displayName", display_name.c_str());
    }
    char* raw = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!raw) {
        return ESP_ERR_NO_MEM;
    }
    std::string body;
    int status = 0;
    esp_err_t err = HttpJson("POST", "/api/devices/pairing-sessions", raw, body, &status);
    cJSON_free(raw);
    if (err != ESP_OK) {
        return err;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON* id = cJSON_GetObjectItem(root, "pairingSessionId");
    const cJSON* code = cJSON_GetObjectItem(root, "code");
    const cJSON* expires = cJSON_GetObjectItem(root, "expiresAt");
    if (cJSON_IsString(id)) out.id = id->valuestring;
    if (cJSON_IsString(code)) out.code = code->valuestring;
    if (cJSON_IsString(expires)) out.expires_at = expires->valuestring;
    cJSON_Delete(root);
    return (out.id.empty() || out.code.empty()) ? ESP_ERR_INVALID_RESPONSE : ESP_OK;
}

esp_err_t MeetApi::PollPairingSession(const std::string& session_id, MeetPairingPollResult& out) {
    out = MeetPairingPollResult{};
    std::string body;
    int status = 0;
    const std::string path = "/api/devices/pairing-sessions/" + session_id;
    esp_err_t err = HttpJson("GET", path, nullptr, body, &status);
    // Backend: 410 = expired, 404 = not found, 409 = credential already delivered.
    if (status == 410 || status == 404 || status == 409) {
        out.expired = true;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON* status_field = cJSON_GetObjectItem(root, "status");
    out.claimed = cJSON_IsString(status_field) && strcmp(status_field->valuestring, "claimed") == 0;
    if (out.claimed) {
        const cJSON* cred = cJSON_GetObjectItem(root, "deviceCredential");
        if (cJSON_IsString(cred)) out.device_credential = cred->valuestring;
        const cJSON* device_id = cJSON_GetObjectItem(root, "deviceId");
        if (cJSON_IsString(device_id)) out.device_id = device_id->valuestring;
        if (out.device_credential.empty()) {
            out.claimed = false;
            out.expired = true;  // already delivered once
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t MeetApi::ListCharacters(std::vector<MeetCharacter>& out) {
    out.clear();
    std::string body;
    int status = 0;
    esp_err_t err = HttpJson("GET", "/api/characters", nullptr, body, &status);
    if (err != ESP_OK) {
        return err;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON* arr = cJSON_GetObjectItem(root, "characters");
    if (cJSON_IsArray(arr)) {
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, arr) {
            MeetCharacter ch;
            const cJSON* id = cJSON_GetObjectItem(item, "id");
            const cJSON* name = cJSON_GetObjectItem(item, "name");
            if (cJSON_IsString(id)) ch.id = id->valuestring;
            if (cJSON_IsString(name)) ch.name = name->valuestring;
            if (!ch.id.empty()) {
                out.push_back(std::move(ch));
            }
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t MeetApi::GetCharacterRuntime(const std::string& character_id, MeetCharacterRuntime& out) {
    out = MeetCharacterRuntime{};
    out.character_id = character_id;
    out.max_history_turns = kMeetMaxHistoryTurns;
    std::string body;
    int status = 0;
    const std::string path = "/api/characters/" + character_id + "/runtime";
    esp_err_t err = HttpJson("GET", path, nullptr, body, &status);
    if (err != ESP_OK) {
        return err;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON* realtime = cJSON_GetObjectItem(root, "realtime");
    const cJSON* src = realtime ? realtime : root;
    const cJSON* voice = cJSON_GetObjectItem(src, "voice");
    const cJSON* instructions = cJSON_GetObjectItem(src, "instructions");
    const cJSON* provider = cJSON_GetObjectItem(src, "provider");
    if (!voice) voice = cJSON_GetObjectItem(root, "voice");
    if (!instructions) instructions = cJSON_GetObjectItem(root, "instructions");
    if (!provider) provider = cJSON_GetObjectItem(root, "provider");
    if (cJSON_IsString(voice)) out.voice = voice->valuestring;
    if (cJSON_IsString(instructions)) out.instructions = instructions->valuestring;
    if (cJSON_IsString(provider)) out.provider = provider->valuestring;
    // HTTP runtime does not return maxHistoryTurns; always use kMeetMaxHistoryTurns.
    cJSON_Delete(root);
    if (out.voice.empty() || out.instructions.empty()) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t MeetApi::UpdateSelectedCharacter(const std::string& character_id) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "selectedCharacterId", character_id.c_str());
    char* raw = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!raw) {
        return ESP_ERR_NO_MEM;
    }
    std::string body;
    int status = 0;
    esp_err_t err = HttpJson("PATCH", "/api/devices/me", raw, body, &status);
    cJSON_free(raw);
    return err;
}

esp_err_t MeetApi::CreateConversation(const std::string& character_id,
                                      const std::string& mode,
                                      MeetConversation& out) {
    cJSON* root = cJSON_CreateObject();
    const std::string id = MakeUuidV4();
    cJSON_AddStringToObject(root, "id", id.c_str());
    cJSON_AddStringToObject(root, "characterId", character_id.c_str());
    cJSON_AddStringToObject(root, "mode", mode.c_str());
    char* raw = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!raw) {
        return ESP_ERR_NO_MEM;
    }
    std::string body;
    int status = 0;
    esp_err_t err = HttpJson("POST", "/api/conversations", raw, body, &status);
    cJSON_free(raw);
    if (err != ESP_OK) {
        return err;
    }
    cJSON* resp = cJSON_Parse(body.c_str());
    if (!resp) {
        out.id = id;
        return ESP_OK;
    }
    const cJSON* conv = cJSON_GetObjectItem(resp, "conversation");
    const cJSON* cid = conv ? cJSON_GetObjectItem(conv, "id") : nullptr;
    out.id = cJSON_IsString(cid) ? cid->valuestring : id;
    cJSON_Delete(resp);
    return ESP_OK;
}

esp_err_t MeetApi::TeachingPrepareChatOnly(const std::string& conversation_id) {
    const std::string path = "/api/conversations/" + conversation_id + "/teaching/prepare";
    std::string body;
    int status = 0;
    return HttpJson("POST", path, "{\"choice\":\"chat_only\"}", body, &status);
}

esp_err_t MeetApi::CompleteConversation(const std::string& conversation_id, int last_sequence) {
    const std::string path = "/api/conversations/" + conversation_id + "/complete";
    char payload[64];
    snprintf(payload, sizeof(payload), "{\"lastSequence\":%d}", last_sequence);
    std::string body;
    int status = 0;
    return HttpJson("POST", path, payload, body, &status);
}

esp_err_t MeetApi::ReportIdentity(const std::string& serial, const std::string& firmware_version) {
    cJSON* root = cJSON_CreateObject();
    if (!serial.empty()) {
        cJSON_AddStringToObject(root, "serial", serial.c_str());
    }
    if (!firmware_version.empty()) {
        cJSON_AddStringToObject(root, "firmwareVersion", firmware_version.c_str());
    }
    char* raw = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!raw) {
        return ESP_ERR_NO_MEM;
    }
    std::string body;
    int status = 0;
    esp_err_t err = HttpJson("PATCH", "/api/devices/me", raw, body, &status);
    cJSON_free(raw);
    return err;
}

esp_err_t MeetApi::CheckFirmware(const std::string& current,
                                 const std::string& serial,
                                 MeetFirmwareInfo& out) {
    out = MeetFirmwareInfo{};
    std::string path = "/api/devices/firmware?current=";
    path += current.empty() ? "0.0.0" : current;
    if (!serial.empty()) {
        path += "&serial=";
        path += serial;
    }
    std::string body;
    int status = 0;
    esp_err_t err = HttpJson("GET", path, nullptr, body, &status);
    if (err != ESP_OK) {
        return err;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON* available = cJSON_GetObjectItem(root, "available");
    const cJSON* version = cJSON_GetObjectItem(root, "version");
    const cJSON* url = cJSON_GetObjectItem(root, "url");
    const cJSON* sha = cJSON_GetObjectItem(root, "sha256");
    const cJSON* size = cJSON_GetObjectItem(root, "size");
    const cJSON* force = cJSON_GetObjectItem(root, "force");
    out.available = cJSON_IsTrue(available);
    if (cJSON_IsString(version)) out.version = version->valuestring;
    if (cJSON_IsString(url)) out.url = url->valuestring;
    if (cJSON_IsString(sha)) out.sha256 = sha->valuestring;
    if (cJSON_IsNumber(size)) out.size = static_cast<size_t>(size->valuedouble);
    out.force = cJSON_IsTrue(force);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t MeetApi::GetAuthMe(MeetAuthMe& out) {
    out = MeetAuthMe{};
    std::string body;
    int status = 0;
    esp_err_t err = HttpJson("GET", "/api/auth/me", nullptr, body, &status);
    if (err != ESP_OK) {
        return err;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON* user = cJSON_GetObjectItem(root, "user");
    const cJSON* src = user ? user : root;
    const cJSON* type = cJSON_GetObjectItem(src, "accountType");
    const cJSON* id = cJSON_GetObjectItem(src, "id");
    if (cJSON_IsString(type)) out.account_type = type->valuestring;
    if (cJSON_IsString(id)) out.user_id = id->valuestring;
    cJSON_Delete(root);
    return ESP_OK;
}

}  // namespace meet
