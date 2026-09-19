#pragma once

#include <esp_err.h>
#include <esp_event.h>
#include <esp_http_server.h>
#include <string>

namespace meet {

enum class WifiPhase {
    Idle,
    ConnectingSta,
    Connected,
    ConfigAp,
    Failed,
};

class WifiService {
public:
    static WifiService& Instance();

    esp_err_t Start();
    void EnterConfigMode();

    WifiPhase phase() const { return phase_; }
    const std::string& sta_ssid() const { return sta_ssid_; }
    const std::string& ap_ssid() const { return ap_ssid_; }
    const std::string& ap_url() const { return ap_url_; }
    const std::string& display_name() const { return display_name_; }
    bool ConsumePhaseChange();
    int rssi() const;

private:
    WifiService() = default;

    esp_err_t InitStack();
    void ConnectSta();
    void StartSoftAp();
    void StopHttp();
    void StartHttp();
    void BuildIdentity();
    void SetPhase(WifiPhase next);

    static void WifiEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);
    static esp_err_t HttpRoot(httpd_req_t* req);
    static esp_err_t HttpSave(httpd_req_t* req);
    static void ConnectTimeout(void* arg);

    bool inited_ = false;
    bool phase_dirty_ = false;
    WifiPhase phase_ = WifiPhase::Idle;
    std::string sta_ssid_;
    std::string ap_ssid_;
    std::string ap_url_ = "http://192.168.4.1";
    std::string display_name_;
    void* httpd_ = nullptr;
    void* connect_timer_ = nullptr;
    void* sta_netif_ = nullptr;
    void* ap_netif_ = nullptr;
};

}  // namespace meet
