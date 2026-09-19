#include "wifi_service.h"

#include "wifi_nvs.h"

#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace meet {
namespace {

constexpr char TAG[] = "wifi";
constexpr int kConnectTimeoutSec = 60;

int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string UrlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') {
            out.push_back(' ');
        } else if (in[i] == '%' && i + 2 < in.size()) {
            const int hi = HexVal(in[i + 1]);
            const int lo = HexVal(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            }
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

bool FormValue(const std::string& body, const char* key, std::string& out) {
    const std::string prefix = std::string(key) + "=";
    size_t pos = 0;
    while (pos < body.size()) {
        const size_t amp = body.find('&', pos);
        const std::string part = body.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        if (part.rfind(prefix, 0) == 0) {
            out = UrlDecode(part.substr(prefix.size()));
            return true;
        }
        if (amp == std::string::npos) {
            break;
        }
        pos = amp + 1;
    }
    return false;
}

}  // namespace

WifiService& WifiService::Instance() {
    static WifiService svc;
    return svc;
}

void WifiService::BuildIdentity() {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char suffix[8];
    snprintf(suffix, sizeof(suffix), "%02X%02X", mac[4], mac[5]);
    ap_ssid_ = std::string("Meet-") + suffix;
    display_name_ = std::string("太空舱-") + suffix;
}

void WifiService::SetPhase(WifiPhase next) {
    if (phase_ == next) {
        return;
    }
    phase_ = next;
    phase_dirty_ = true;
}

bool WifiService::ConsumePhaseChange() {
    if (!phase_dirty_) {
        return false;
    }
    phase_dirty_ = false;
    return true;
}

esp_err_t WifiService::InitStack() {
    if (inited_) {
        return ESP_OK;
    }
    BuildIdentity();

    sta_netif_ = esp_netif_create_default_wifi_sta();
    ap_netif_ = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, WifiEventHandler, this,
                                                        nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, WifiEventHandler, this,
                                                        nullptr));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    esp_timer_create_args_t timer = {};
    timer.callback = ConnectTimeout;
    timer.arg = this;
    timer.dispatch_method = ESP_TIMER_TASK;
    timer.name = "wifi_sta";
    ESP_ERROR_CHECK(esp_timer_create(&timer, reinterpret_cast<esp_timer_handle_t*>(&connect_timer_)));

    inited_ = true;
    return ESP_OK;
}

esp_err_t WifiService::Start() {
    ESP_ERROR_CHECK(InitStack());
    WifiCredentials cred;
    WifiNvsLoad(cred);
    if (cred.ssid.empty()) {
        StartSoftAp();
        return ESP_OK;
    }
    sta_ssid_ = cred.ssid;
    ConnectSta();
    return ESP_OK;
}

void WifiService::EnterConfigMode() {
    ESP_LOGI(TAG, "enter SoftAP config");
    if (connect_timer_) {
        esp_timer_stop(static_cast<esp_timer_handle_t>(connect_timer_));
    }
    StartSoftAp();
}

void WifiService::ConnectSta() {
    WifiCredentials cred;
    WifiNvsLoad(cred);
    if (cred.ssid.empty()) {
        StartSoftAp();
        return;
    }
    sta_ssid_ = cred.ssid;
    StopHttp();
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t cfg = {};
    strncpy(reinterpret_cast<char*>(cfg.sta.ssid), cred.ssid.c_str(), sizeof(cfg.sta.ssid) - 1);
    strncpy(reinterpret_cast<char*>(cfg.sta.password), cred.password.c_str(), sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = cred.password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
    if (connect_timer_) {
        esp_timer_stop(static_cast<esp_timer_handle_t>(connect_timer_));
        esp_timer_start_once(static_cast<esp_timer_handle_t>(connect_timer_),
                             static_cast<uint64_t>(kConnectTimeoutSec) * 1000000ULL);
    }
    SetPhase(WifiPhase::ConnectingSta);
    ESP_LOGI(TAG, "STA connecting to %s", cred.ssid.c_str());
}

void WifiService::StartSoftAp() {
    StopHttp();
    if (connect_timer_) {
        esp_timer_stop(static_cast<esp_timer_handle_t>(connect_timer_));
    }
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t cfg = {};
    strncpy(reinterpret_cast<char*>(cfg.ap.ssid), ap_ssid_.c_str(), sizeof(cfg.ap.ssid) - 1);
    cfg.ap.ssid_len = static_cast<uint8_t>(ap_ssid_.size());
    cfg.ap.channel = 1;
    cfg.ap.max_connection = 4;
    cfg.ap.authmode = WIFI_AUTH_OPEN;
    cfg.ap.ssid_hidden = 0;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    StartHttp();
    SetPhase(WifiPhase::ConfigAp);
    ESP_LOGI(TAG, "SoftAP %s  %s", ap_ssid_.c_str(), ap_url_.c_str());
}

void WifiService::StartHttp() {
    if (httpd_) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGW(TAG, "httpd start failed");
        return;
    }
    httpd_ = server;
    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = HttpRoot,
        .user_ctx = this,
    };
    httpd_uri_t save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = HttpSave,
        .user_ctx = this,
    };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &save);
}

void WifiService::StopHttp() {
    if (!httpd_) {
        return;
    }
    httpd_stop(static_cast<httpd_handle_t>(httpd_));
    httpd_ = nullptr;
}

void WifiService::WifiEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    auto* self = static_cast<WifiService*>(arg);
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (self->phase_ == WifiPhase::Connected) {
            ESP_LOGW(TAG, "STA disconnected");
            self->SetPhase(WifiPhase::Failed);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (self->connect_timer_) {
            esp_timer_stop(static_cast<esp_timer_handle_t>(self->connect_timer_));
        }
        self->SetPhase(WifiPhase::Connected);
        ESP_LOGI(TAG, "STA got IP");
    }
}

void WifiService::ConnectTimeout(void* arg) {
    auto* self = static_cast<WifiService*>(arg);
    if (self->phase_ == WifiPhase::ConnectingSta) {
        ESP_LOGW(TAG, "STA timeout → SoftAP");
        self->StartSoftAp();
    }
}

esp_err_t WifiService::HttpRoot(httpd_req_t* req) {
    auto* self = static_cast<WifiService*>(req->user_ctx);
    std::string html =
        "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' "
        "content='width=device-width,initial-scale=1'><title>Meet 配网</title></head><body>"
        "<h3>Meet 伴伴机配网</h3><p>热点 " +
        self->ap_ssid_ +
        "</p><form method='POST' action='/save'>"
        "Wi-Fi 名称<br><input name='ssid' required><br><br>"
        "密码<br><input name='password' type='password'><br><br>"
        "<button type='submit'>连接</button></form></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html.c_str(), html.size());
}

esp_err_t WifiService::HttpSave(httpd_req_t* req) {
    auto* self = static_cast<WifiService*>(req->user_ctx);
    const size_t len = req->content_len;
    if (len == 0 || len > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
        return ESP_FAIL;
    }
    std::vector<char> buf(len + 1, 0);
    int remaining = static_cast<int>(len);
    int offset = 0;
    while (remaining > 0) {
        const int n = httpd_req_recv(req, buf.data() + offset, remaining);
        if (n <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv");
            return ESP_FAIL;
        }
        remaining -= n;
        offset += n;
    }
    std::string ssid;
    std::string password;
    FormValue(buf.data(), "ssid", ssid);
    FormValue(buf.data(), "password", password);
    if (ssid.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }
    WifiCredentials cred;
    cred.ssid = ssid;
    cred.password = password;
    WifiNvsSave(cred);
    const char* ok = "<!doctype html><meta charset='utf-8'><p>已保存，设备正在连接…</p>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, ok, HTTPD_RESP_USE_STRLEN);
    self->ConnectSta();
    return ESP_OK;
}

}  // namespace meet
