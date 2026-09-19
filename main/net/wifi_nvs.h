#pragma once

#include <esp_err.h>
#include <string>

namespace meet {

struct WifiCredentials {
    std::string ssid;
    std::string password;
};

esp_err_t WifiNvsLoad(WifiCredentials& out);
esp_err_t WifiNvsSave(const WifiCredentials& in);
esp_err_t WifiNvsClear();

}  // namespace meet
