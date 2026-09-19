#include "wifi_nvs.h"

#include <nvs.h>
#include <nvs_flash.h>

namespace meet {
namespace {

constexpr char kNs[] = "wifi";
constexpr char kSsid[] = "ssid";
constexpr char kPass[] = "pass";

esp_err_t ReadStr(nvs_handle_t h, const char* key, std::string& out) {
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

esp_err_t WifiNvsLoad(WifiCredentials& out) {
    out = WifiCredentials{};
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNs, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    err = ReadStr(h, kSsid, out.ssid);
    if (err == ESP_OK) {
        err = ReadStr(h, kPass, out.password);
    }
    nvs_close(h);
    return err;
}

esp_err_t WifiNvsSave(const WifiCredentials& in) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNs, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, kSsid, in.ssid.c_str());
    if (err == ESP_OK) {
        err = nvs_set_str(h, kPass, in.password.c_str());
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t WifiNvsClear() {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNs, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    nvs_erase_all(h);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

}  // namespace meet
