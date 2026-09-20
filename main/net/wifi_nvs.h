#pragma once

#include <esp_err.h>
#include <string>

namespace meet {

constexpr int kWifiNetworkMax = 3;

struct WifiCredentials {
    std::string ssid;
    std::string password;
};

struct WifiNetworkList {
    WifiCredentials items[kWifiNetworkMax];
    int count = 0;
    int last_ok = 0;
};

esp_err_t WifiNvsLoad(WifiCredentials& out);
esp_err_t WifiNvsSave(const WifiCredentials& in);
esp_err_t WifiNvsLoadAll(WifiNetworkList& out);
esp_err_t WifiNvsSaveAll(const WifiNetworkList& in);
esp_err_t WifiNvsUpsert(const WifiCredentials& in);
esp_err_t WifiNvsClear();

}  // namespace meet
