#include "settings.h"

#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <sdkconfig.h>
#include <cstring>

namespace meet {
namespace {

constexpr char TAG[] = "settings";
constexpr char kNs[] = "meet";
constexpr char kServerOrigin[] = "srv_origin";
constexpr char kDeviceCredential[] = "dev_cred";
constexpr char kDeviceId[] = "dev_id";
constexpr char kCharacterId[] = "char_id";
constexpr char kCharacterName[] = "char_name";
constexpr char kAccountType[] = "acct_type";
constexpr char kVolume[] = "volume";

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

}  // namespace

Settings& Settings::Instance() {
    static Settings s;
    return s;
}

esp_err_t Settings::Load() {
    std::lock_guard<std::mutex> lock(mu_);
    data_ = SettingsData{};
    data_.server_origin = CONFIG_MEET_SERVER_URL;
    data_.volume = 70;

    nvs_handle_t h;
    esp_err_t err = nvs_open(kNs, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        loaded_ = true;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    ReadString(h, kServerOrigin, data_.server_origin);
    if (data_.server_origin.empty()) {
        data_.server_origin = CONFIG_MEET_SERVER_URL;
    }
    ReadString(h, kDeviceCredential, data_.device_credential);
    ReadString(h, kDeviceId, data_.device_id);
    ReadString(h, kCharacterId, data_.selected_character_id);
    ReadString(h, kCharacterName, data_.selected_character_name);
    ReadString(h, kAccountType, data_.account_type);
    uint8_t volume = 70;
    if (nvs_get_u8(h, kVolume, &volume) == ESP_OK) {
        data_.volume = volume;
    }
    nvs_close(h);
    loaded_ = true;
    ESP_LOGI(TAG, "loaded credential=%d char=%s",
             data_.device_credential.empty() ? 0 : 1,
             data_.selected_character_name.c_str());
    return ESP_OK;
}

esp_err_t Settings::PersistLocked() const {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNs, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, kServerOrigin, data_.server_origin.c_str());
    if (err == ESP_OK) err = nvs_set_str(h, kDeviceCredential, data_.device_credential.c_str());
    if (err == ESP_OK) err = nvs_set_str(h, kDeviceId, data_.device_id.c_str());
    if (err == ESP_OK) err = nvs_set_str(h, kCharacterId, data_.selected_character_id.c_str());
    if (err == ESP_OK) err = nvs_set_str(h, kCharacterName, data_.selected_character_name.c_str());
    if (err == ESP_OK) err = nvs_set_str(h, kAccountType, data_.account_type.c_str());
    if (err == ESP_OK) {
        int volume = data_.volume;
        if (volume < 0) volume = 0;
        if (volume > 100) volume = 100;
        err = nvs_set_u8(h, kVolume, static_cast<uint8_t>(volume));
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t Settings::Save() {
    std::lock_guard<std::mutex> lock(mu_);
    return PersistLocked();
}

SettingsData Settings::Snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return data_;
}

void Settings::Apply(const SettingsData& data) {
    std::lock_guard<std::mutex> lock(mu_);
    data_ = data;
    PersistLocked();
}

std::string Settings::server_origin() const {
    std::lock_guard<std::mutex> lock(mu_);
    return data_.server_origin;
}

std::string Settings::device_credential() const {
    std::lock_guard<std::mutex> lock(mu_);
    return data_.device_credential;
}

std::string Settings::device_id() const {
    std::lock_guard<std::mutex> lock(mu_);
    return data_.device_id;
}

std::string Settings::selected_character_id() const {
    std::lock_guard<std::mutex> lock(mu_);
    return data_.selected_character_id;
}

std::string Settings::selected_character_name() const {
    std::lock_guard<std::mutex> lock(mu_);
    return data_.selected_character_name;
}

std::string Settings::account_type() const {
    std::lock_guard<std::mutex> lock(mu_);
    return data_.account_type;
}

int Settings::volume() const {
    std::lock_guard<std::mutex> lock(mu_);
    return data_.volume;
}

void Settings::SetServerOrigin(const std::string& v) {
    std::lock_guard<std::mutex> lock(mu_);
    data_.server_origin = v;
    PersistLocked();
}

void Settings::SetDeviceCredential(const std::string& cred, const std::string& device_id) {
    std::lock_guard<std::mutex> lock(mu_);
    data_.device_credential = cred;
    data_.device_id = device_id;
    PersistLocked();
}

void Settings::ClearCredential() {
    std::lock_guard<std::mutex> lock(mu_);
    data_.device_credential.clear();
    data_.device_id.clear();
    data_.account_type.clear();
    PersistLocked();
}

void Settings::SetSelectedCharacter(const std::string& id, const std::string& name) {
    std::lock_guard<std::mutex> lock(mu_);
    data_.selected_character_id = id;
    data_.selected_character_name = name;
    PersistLocked();
}

void Settings::SetAccountType(const std::string& type) {
    std::lock_guard<std::mutex> lock(mu_);
    data_.account_type = type;
    PersistLocked();
}

void Settings::SetVolume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    std::lock_guard<std::mutex> lock(mu_);
    data_.volume = volume;
    PersistLocked();
}

}  // namespace meet
