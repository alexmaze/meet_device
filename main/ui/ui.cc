#include "ui.h"

#include "board.h"
#include "config.h"
#include "ui_view.h"
#include "wifi_service.h"

#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <lvgl.h>
#include "src/draw/sw/lv_draw_sw_utils.h"
#include <cstdio>
#include <cstring>

namespace meet {
namespace {

constexpr char TAG[] = "ui";
constexpr int kMaxBufW = DISPLAY_WIDTH_1 > DISPLAY_WIDTH ? DISPLAY_WIDTH_1 : DISPLAY_WIDTH;
constexpr int kQueueDepth = 16;

QueueHandle_t g_ui_q = nullptr;
lv_display_t* display_ = nullptr;

void PostCmd(const UiCommand& cmd) {
    if (!g_ui_q) {
        return;
    }
    if (xQueueSend(g_ui_q, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "ui queue full, drop type=%d", static_cast<int>(cmd.type));
    }
}

void FillEnvFromBoard() {
    UiEnv env;
    env.width = Board::Instance().display_width();
    env.height = Board::Instance().display_height();
    env.landscape = Board::Instance().landscape();
    env.wifi_ok = WifiService::Instance().phase() == WifiPhase::Connected;
    env.rssi = WifiService::Instance().rssi();
    env.battery_percent = Board::Instance().battery_percent();
    env.charging = Board::Instance().is_charging();
    UiViewSetEnv(env);
}

void FlushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    auto panel = Board::Instance().lcd_panel();
    if (panel) {
        const uint32_t px = static_cast<uint32_t>(area->x2 - area->x1 + 1) *
                            static_cast<uint32_t>(area->y2 - area->y1 + 1);
        // ST7789 SPI wants RGB565 big-endian; LVGL renders native LE.
        lv_draw_sw_rgb565_swap(px_map, px);
        esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
    } else {
        lv_display_flush_ready(disp);
    }
}

}  // namespace

void UiNotifyFlushDone() {
    if (display_) {
        lv_display_flush_ready(display_);
    }
}

esp_err_t UiInit() {
    const int w = Board::Instance().display_width();
    const int h = Board::Instance().display_height();
    ESP_LOGI(TAG, "LVGL init for ST7789 %dx%d", w, h);
    g_ui_q = xQueueCreate(kQueueDepth, sizeof(UiCommand));
    if (!g_ui_q) {
        return ESP_ERR_NO_MEM;
    }

    lv_init();
    display_ = lv_display_create(w, h);
    if (!display_) {
        ESP_LOGE(TAG, "lv_display_create failed");
        return ESP_FAIL;
    }

    constexpr size_t kBufBytes = kMaxBufW * 40 * 2;
    static uint8_t buf1[kBufBytes];
    static uint8_t buf2[kBufBytes];
    lv_display_set_buffers(display_, buf1, buf2, kBufBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_color_format(display_, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display_, FlushCb);

    FillEnvFromBoard();
    UiViewShowPlain("Meet", "启动中");
    return ESP_OK;
}

void UiShowPairing(const char* code, const char* hint) {
    UiCommand cmd;
    cmd.type = UiCmdType::Pairing;
    strncpy(cmd.title, code && code[0] ? code : "------", sizeof(cmd.title) - 1);
    if (hint) strncpy(cmd.body, hint, sizeof(cmd.body) - 1);
    PostCmd(cmd);
}

void UiShowReady(const char* character_name) {
    UiCommand cmd;
    cmd.type = UiCmdType::Ready;
    if (character_name) strncpy(cmd.title, character_name, sizeof(cmd.title) - 1);
    PostCmd(cmd);
}

void UiShowUnprovisioned() {
    UiCommand cmd;
    cmd.type = UiCmdType::Unprovisioned;
    PostCmd(cmd);
}

void UiShowWifiConfig(const char* ap_ssid, const char* url) {
    UiCommand cmd;
    cmd.type = UiCmdType::WifiConfig;
    snprintf(cmd.body, sizeof(cmd.body), "手机连接 %s\n浏览器打开 %s",
             ap_ssid ? ap_ssid : "Meet", url ? url : "http://192.168.4.1");
    PostCmd(cmd);
}

void UiShowWifiConnecting(const char* ssid) {
    UiCommand cmd;
    cmd.type = UiCmdType::WifiConnecting;
    if (ssid) strncpy(cmd.title, ssid, sizeof(cmd.title) - 1);
    PostCmd(cmd);
}

void UiShowConnecting() {
    UiCommand cmd;
    cmd.type = UiCmdType::Connecting;
    PostCmd(cmd);
}

void UiShowInCall(const char* subtitle) {
    UiCommand cmd;
    cmd.type = UiCmdType::InCall;
    if (subtitle) strncpy(cmd.body, subtitle, sizeof(cmd.body) - 1);
    PostCmd(cmd);
}

void UiShowSettings(const char* body) {
    UiCommand cmd;
    cmd.type = UiCmdType::Settings;
    if (body) strncpy(cmd.body, body, sizeof(cmd.body) - 1);
    PostCmd(cmd);
}

void UiSetEmotion(const char* name) {
    UiCommand cmd;
    cmd.type = UiCmdType::Emotion;
    strncpy(cmd.title, name && name[0] ? name : "neutral", sizeof(cmd.title) - 1);
    PostCmd(cmd);
}

void UiSetCaption(const char* text) {
    UiCommand cmd;
    cmd.type = UiCmdType::Caption;
    if (text) strncpy(cmd.body, text, sizeof(cmd.body) - 1);
    PostCmd(cmd);
}

void UiClearCaption() {
    UiCommand cmd;
    cmd.type = UiCmdType::ClearCaption;
    PostCmd(cmd);
}

void UiShowToast(const char* text) {
    UiCommand cmd;
    cmd.type = UiCmdType::Toast;
    if (text) strncpy(cmd.title, text, sizeof(cmd.title) - 1);
    PostCmd(cmd);
}

void UiTick(uint32_t elapsed_ms) {
    FillEnvFromBoard();
    UiCommand cmd;
    while (g_ui_q && xQueueReceive(g_ui_q, &cmd, 0) == pdTRUE) {
        UiViewApplyCommand(cmd);
    }
    lv_tick_inc(elapsed_ms);
    lv_timer_handler();
    UiViewTick(elapsed_ms);
}

}  // namespace meet
