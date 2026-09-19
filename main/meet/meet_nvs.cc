#include "meet_nvs.h"

#include <esp_log.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <cstring>

namespace meet {
namespace {

constexpr char TAG[] = "meet_nvs";

esp_err_t ReadString(nvs_handle_t h, const char* key, std::string& out) {
    size_t len = 0;
    esp_err_t err = nvs_get_str(h, key, nullptr, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        out.clear();
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    out.resize(len);
    err = nvs_get_str(h, key, out.data(), &len);
    if (err == ESP_OK && !out.empty() && out.back() == '\0') {
        out.pop_back();
    }
    return err;
}

esp_err_t WriteString(nvs_handle_t h, const char* key, const std::string& value) {
    return nvs_set_str(h, key, value.c_str());
}

}  // namespace

esp_err_t MeetNvsLoad(MeetConfig& out) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(MeetNvsKeys::kNamespace, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        out = MeetConfig{};
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    err = ReadString(h, MeetNvsKeys::kServerOrigin, out.server_origin);
    if (err == ESP_OK) err = ReadString(h, MeetNvsKeys::kDeviceCredential, out.device_credential);
    if (err == ESP_OK) err = ReadString(h, MeetNvsKeys::kDeviceId, out.device_id);
    if (err == ESP_OK) err = ReadString(h, MeetNvsKeys::kCharacterId, out.selected_character_id);
    if (err == ESP_OK) err = ReadString(h, MeetNvsKeys::kCharacterName, out.selected_character_name);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "load partial failure: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t MeetNvsSave(const MeetConfig& in) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(MeetNvsKeys::kNamespace, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = WriteString(h, MeetNvsKeys::kServerOrigin, in.server_origin);
    if (err == ESP_OK) err = WriteString(h, MeetNvsKeys::kDeviceCredential, in.device_credential);
    if (err == ESP_OK) err = WriteString(h, MeetNvsKeys::kDeviceId, in.device_id);
    if (err == ESP_OK) err = WriteString(h, MeetNvsKeys::kCharacterId, in.selected_character_id);
    if (err == ESP_OK) err = WriteString(h, MeetNvsKeys::kCharacterName, in.selected_character_name);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

}  // namespace meet
