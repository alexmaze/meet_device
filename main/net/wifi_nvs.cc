#include "wifi_nvs.h"

#include <nvs.h>
#include <nvs_flash.h>
#include <cstdio>
#include <cstring>

namespace meet {
namespace {

constexpr char kNs[] = "wifi";
constexpr char kSsid[] = "ssid";
constexpr char kPass[] = "pass";
constexpr char kCount[] = "count";
constexpr char kLastOk[] = "last_ok";

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

void ItemKeys(int index, char* ssid_key, char* pass_key, size_t n) {
    snprintf(ssid_key, n, "ssid%d", index);
    snprintf(pass_key, n, "pass%d", index);
}

}  // namespace

esp_err_t WifiNvsLoadAll(WifiNetworkList& out) {
    out = WifiNetworkList{};
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNs, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t count = 0;
    uint8_t last_ok = 0;
    if (nvs_get_u8(h, kCount, &count) != ESP_OK) {
        count = 0;
    }
    if (nvs_get_u8(h, kLastOk, &last_ok) != ESP_OK) {
        last_ok = 0;
    }

    if (count > 0) {
        if (count > kWifiNetworkMax) count = kWifiNetworkMax;
        for (int i = 0; i < count; ++i) {
            char ssid_key[12];
            char pass_key[12];
            ItemKeys(i, ssid_key, pass_key, sizeof(ssid_key));
            ReadStr(h, ssid_key, out.items[i].ssid);
            ReadStr(h, pass_key, out.items[i].password);
            if (!out.items[i].ssid.empty()) {
                out.count = i + 1;
            }
        }
        out.last_ok = last_ok < out.count ? last_ok : 0;
    } else {
        WifiCredentials legacy;
        ReadStr(h, kSsid, legacy.ssid);
        ReadStr(h, kPass, legacy.password);
        if (!legacy.ssid.empty()) {
            out.items[0] = legacy;
            out.count = 1;
            out.last_ok = 0;
        }
    }
    nvs_close(h);
    return ESP_OK;
}

esp_err_t WifiNvsSaveAll(const WifiNetworkList& in) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNs, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    const int count = in.count < 0 ? 0 : (in.count > kWifiNetworkMax ? kWifiNetworkMax : in.count);
    err = nvs_set_u8(h, kCount, static_cast<uint8_t>(count));
    if (err == ESP_OK) {
        const int last_ok = (in.last_ok >= 0 && in.last_ok < count) ? in.last_ok : 0;
        err = nvs_set_u8(h, kLastOk, static_cast<uint8_t>(last_ok));
    }
    for (int i = 0; err == ESP_OK && i < count; ++i) {
        char ssid_key[12];
        char pass_key[12];
        ItemKeys(i, ssid_key, pass_key, sizeof(ssid_key));
        err = nvs_set_str(h, ssid_key, in.items[i].ssid.c_str());
        if (err == ESP_OK) {
            err = nvs_set_str(h, pass_key, in.items[i].password.c_str());
        }
    }
    if (err == ESP_OK && count > 0) {
        const int idx = (in.last_ok >= 0 && in.last_ok < count) ? in.last_ok : 0;
        err = nvs_set_str(h, kSsid, in.items[idx].ssid.c_str());
        if (err == ESP_OK) {
            err = nvs_set_str(h, kPass, in.items[idx].password.c_str());
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t WifiNvsLoad(WifiCredentials& out) {
    WifiNetworkList list;
    const esp_err_t err = WifiNvsLoadAll(list);
    out = WifiCredentials{};
    if (err != ESP_OK || list.count <= 0) {
        return err;
    }
    const int idx = (list.last_ok >= 0 && list.last_ok < list.count) ? list.last_ok : 0;
    out = list.items[idx];
    return ESP_OK;
}

esp_err_t WifiNvsSave(const WifiCredentials& in) {
    return WifiNvsUpsert(in);
}

esp_err_t WifiNvsUpsert(const WifiCredentials& in) {
    if (in.ssid.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    WifiNetworkList list;
    WifiNvsLoadAll(list);
    int found = -1;
    for (int i = 0; i < list.count; ++i) {
        if (list.items[i].ssid == in.ssid) {
            found = i;
            break;
        }
    }
    if (found >= 0) {
        list.items[found] = in;
        list.last_ok = found;
    } else if (list.count < kWifiNetworkMax) {
        list.items[list.count] = in;
        list.last_ok = list.count;
        list.count += 1;
    } else {
        int replace = 0;
        for (int i = 0; i < list.count; ++i) {
            if (i != list.last_ok) {
                replace = i;
                break;
            }
        }
        list.items[replace] = in;
        list.last_ok = replace;
    }
    return WifiNvsSaveAll(list);
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
