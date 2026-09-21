#include "wifi_service.h"

#include "app_event.h"
#include "wifi_nvs.h"

#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace meet {
namespace {

constexpr char TAG[] = "wifi";
constexpr int kConnectTimeoutSec = 60;
constexpr int kStaRetryMax = 8;

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
        const std::string part =
            body.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
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
    display_name_ = std::string("Meet-") + suffix;
}

esp_err_t WifiService::WifiOp(esp_err_t err, const char* what) {
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: %s", what, esp_err_to_name(err));
    }
    return err;
}

void WifiService::SetPhase(WifiPhase next) {
    if (phase_ == next) {
        return;
    }
    phase_ = next;
    AppEvent ev;
    ev.type = AppEventType::WifiPhaseChanged;
    ev.i32 = static_cast<int32_t>(next);
    AppEventPost(ev);
}

int WifiService::rssi() const {
    if (phase_ != WifiPhase::Connected) {
        return 0;
    }
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}

esp_err_t WifiService::Init() {
    return InitStack();
}

esp_err_t WifiService::InitStack() {
    if (inited_) {
        return ESP_OK;
    }
    BuildIdentity();

    ESP_LOGI(TAG, "init, internal free=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));

    sta_netif_ = esp_netif_create_default_wifi_sta();
    ap_netif_ = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s (internal free=%u)", esp_err_to_name(err),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        return err;
    }
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, WifiEventHandler, this,
                                              nullptr);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, WifiEventHandler, this,
                                              nullptr);
    if (err != ESP_OK) {
        return err;
    }
    (void)esp_wifi_set_storage(WIFI_STORAGE_RAM);

    esp_timer_create_args_t timer = {};
    timer.callback = ConnectTimeout;
    timer.arg = this;
    timer.dispatch_method = ESP_TIMER_TASK;
    timer.name = "wifi_sta";
    err = esp_timer_create(&timer, reinterpret_cast<esp_timer_handle_t*>(&connect_timer_));
    if (err != ESP_OK) {
        return err;
    }

    inited_ = true;
    return ESP_OK;
}

esp_err_t WifiService::Start() {
    const esp_err_t err = InitStack();
    if (err != ESP_OK) {
        SetPhase(WifiPhase::Failed);
        return err;
    }
    WifiNetworkList list;
    WifiNvsLoadAll(list);
    if (list.count <= 0) {
        StartSoftAp();
        return ESP_OK;
    }
    try_count_ = 0;
    ConnectStaIndex(list.last_ok);
    return ESP_OK;
}

void WifiService::EnterConfigMode() {
    ESP_LOGI(TAG, "enter SoftAP config");
    if (connect_timer_) {
        esp_timer_stop(static_cast<esp_timer_handle_t>(connect_timer_));
    }
    StartSoftAp();
}

void WifiService::ConnectAfterProvision() {
    ConnectSta();
}

void WifiService::ConnectSta() {
    WifiNetworkList list;
    WifiNvsLoadAll(list);
    if (list.count <= 0) {
        StartSoftAp();
        return;
    }
    try_count_ = 0;
    ConnectStaIndex(list.last_ok);
}

void WifiService::ConnectStaIndex(int index) {
    WifiNetworkList list;
    WifiNvsLoadAll(list);
    if (list.count <= 0) {
        StartSoftAp();
        return;
    }
    if (index < 0 || index >= list.count) {
        index = 0;
    }
    try_index_ = index;
    sta_retry_ = 0;
    const WifiCredentials& cred = list.items[index];
    sta_ssid_ = cred.ssid;
    StopHttp();
    WifiOp(esp_wifi_stop(), "wifi_stop");
    if (WifiOp(esp_wifi_set_mode(WIFI_MODE_STA), "set_mode STA") != ESP_OK) {
        StartSoftAp();
        return;
    }

    wifi_config_t cfg = {};
    strncpy(reinterpret_cast<char*>(cfg.sta.ssid), cred.ssid.c_str(), sizeof(cfg.sta.ssid) - 1);
    strncpy(reinterpret_cast<char*>(cfg.sta.password), cred.password.c_str(),
            sizeof(cfg.sta.password) - 1);
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.failure_retry_cnt = 5;
    cfg.sta.threshold.authmode = cred.password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK;
    if (WifiOp(esp_wifi_set_config(WIFI_IF_STA, &cfg), "set_config") != ESP_OK) {
        StartSoftAp();
        return;
    }
    // Phase must be ConnectingSta before esp_wifi_start(): STA_START is handled on the
    // higher-priority event loop and can run before this function continues.
    if (connect_timer_) {
        esp_timer_stop(static_cast<esp_timer_handle_t>(connect_timer_));
        esp_timer_start_once(static_cast<esp_timer_handle_t>(connect_timer_),
                             static_cast<uint64_t>(kConnectTimeoutSec) * 1000000ULL);
    }
    SetPhase(WifiPhase::ConnectingSta);
    ESP_LOGI(TAG, "STA starting, will connect %s (%d/%d)", cred.ssid.c_str(), index + 1, list.count);
    if (WifiOp(esp_wifi_start(), "wifi_start") != ESP_OK) {
        StartSoftAp();
        return;
    }
}

void WifiService::TryNextOrAp() {
    WifiNetworkList list;
    WifiNvsLoadAll(list);
    try_count_ += 1;
    if (list.count <= 0 || try_count_ >= list.count) {
        ESP_LOGW(TAG, "all SSIDs failed → SoftAP");
        StartSoftAp();
        return;
    }
    const int next = (try_index_ + 1) % list.count;
    ESP_LOGW(TAG, "STA timeout, try next SSID");
    ConnectStaIndex(next);
}

void WifiService::MarkLastOk() {
    WifiNetworkList list;
    WifiNvsLoadAll(list);
    if (try_index_ >= 0 && try_index_ < list.count) {
        list.last_ok = try_index_;
        WifiNvsSaveAll(list);
    }
}

void WifiService::StartSoftAp() {
    StopHttp();
    if (connect_timer_) {
        esp_timer_stop(static_cast<esp_timer_handle_t>(connect_timer_));
    }
    WifiOp(esp_wifi_stop(), "wifi_stop");
    if (WifiOp(esp_wifi_set_mode(WIFI_MODE_AP), "set_mode AP") != ESP_OK) {
        return;
    }

    wifi_config_t cfg = {};
    strncpy(reinterpret_cast<char*>(cfg.ap.ssid), ap_ssid_.c_str(), sizeof(cfg.ap.ssid) - 1);
    cfg.ap.ssid_len = static_cast<uint8_t>(ap_ssid_.size());
    cfg.ap.channel = 1;
    cfg.ap.max_connection = 4;
    cfg.ap.authmode = WIFI_AUTH_OPEN;
    cfg.ap.ssid_hidden = 0;
    if (WifiOp(esp_wifi_set_config(WIFI_IF_AP, &cfg), "set_config AP") != ESP_OK ||
        WifiOp(esp_wifi_start(), "wifi_start AP") != ESP_OK) {
        return;
    }
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
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (self->phase_ == WifiPhase::ConnectingSta) {
            ESP_LOGI(TAG, "STA start, connecting to %s", self->sta_ssid_.c_str());
            WifiOp(esp_wifi_connect(), "wifi_connect");
        } else {
            ESP_LOGW(TAG, "STA start ignored, phase=%d", static_cast<int>(self->phase_));
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto* disc = static_cast<const wifi_event_sta_disconnected_t*>(data);
        const int reason = disc ? disc->reason : 0;
        if (self->phase_ == WifiPhase::ConnectingSta) {
            self->sta_retry_ += 1;
            ESP_LOGW(TAG, "STA disconnect reason=%d retry=%d/%d", reason, self->sta_retry_,
                     kStaRetryMax);
            if (self->sta_retry_ < kStaRetryMax) {
                WifiOp(esp_wifi_connect(), "wifi_reconnect");
            } else if (self->connect_timer_) {
                // Don't esp_wifi_stop() on the wifi event task; let the timer hop off it.
                esp_timer_stop(static_cast<esp_timer_handle_t>(self->connect_timer_));
                esp_timer_start_once(static_cast<esp_timer_handle_t>(self->connect_timer_), 100000);
            }
        } else if (self->phase_ == WifiPhase::Connected) {
            ESP_LOGW(TAG, "STA dropped reason=%d", reason);
            AppEvent drop;
            drop.type = AppEventType::WifiDropped;
            AppEventPost(drop);
            self->SetPhase(WifiPhase::Failed);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (self->connect_timer_) {
            esp_timer_stop(static_cast<esp_timer_handle_t>(self->connect_timer_));
        }
        self->sta_retry_ = 0;
        self->MarkLastOk();
        self->SetPhase(WifiPhase::Connected);
        ESP_LOGI(TAG, "STA got IP");
    }
}

void WifiService::ConnectTimeout(void* arg) {
    auto* self = static_cast<WifiService*>(arg);
    if (self->phase_ == WifiPhase::ConnectingSta) {
        self->TryNextOrAp();
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
        "<p>最多保存 3 组，连接失败会自动试下一组。</p>"
        "<button type='submit'>连接</button></form></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html.c_str(), html.size());
}

esp_err_t WifiService::HttpSave(httpd_req_t* req) {
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
    WifiNvsUpsert(cred);
    const char* ok = "<!doctype html><meta charset='utf-8'><p>已保存，设备正在连接…</p>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, ok, HTTPD_RESP_USE_STRLEN);
    // Do NOT call ConnectSta/StopHttp here — would deadlock httpd joining itself.
    AppEvent ev;
    ev.type = AppEventType::WifiConnectRequested;
    AppEventPost(ev);
    return ESP_OK;
}

}  // namespace meet
