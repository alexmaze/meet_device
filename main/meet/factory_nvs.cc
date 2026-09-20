#include "factory_nvs.h"

#include <esp_mac.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <cstdio>

namespace meet {
namespace {

constexpr char kNs[] = "factory";
constexpr char kSerial[] = "serial";

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

std::string MakeSerialFromMac() {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[20];
    snprintf(buf, sizeof(buf), "MEET-%02X%02X%02X", mac[3], mac[4], mac[5]);
    return buf;
}

}  // namespace

esp_err_t FactorySerialLoad(std::string& out) {
    out.clear();
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNs, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = ReadStr(h, kSerial, out);
    if (err == ESP_OK && out.empty()) {
        out = MakeSerialFromMac();
        err = nvs_set_str(h, kSerial, out.c_str());
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
    }
    nvs_close(h);
    return err;
}

}  // namespace meet
