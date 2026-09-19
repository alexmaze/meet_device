#include "ui.h"

#include "board.h"
#include "config.h"
#include "wifi_service.h"

#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <lvgl.h>
#include <cstdio>
#include <cstring>

namespace meet {
namespace {

constexpr char TAG[] = "ui";
constexpr int kMaxBufW = DISPLAY_HEIGHT;  // 320, covers landscape width

lv_display_t* display_ = nullptr;
lv_obj_t* screen_ = nullptr;
lv_obj_t* status_label_ = nullptr;
lv_obj_t* title_label_ = nullptr;
lv_obj_t* body_label_ = nullptr;
lv_obj_t* toast_label_ = nullptr;
int toast_ms_left_ = 0;
uint32_t status_accum_ms_ = 0;
bool landscape_ = false;

const lv_font_t* UiFont() {
#if LV_FONT_SOURCE_HAN_SANS_SC_16_CJK
    return &lv_font_source_han_sans_sc_16_cjk;
#else
    return &lv_font_montserrat_20;
#endif
}

int HorRes() {
    return landscape_ ? DISPLAY_HEIGHT : DISPLAY_WIDTH;
}

void FlushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    auto panel = Board::Instance().lcd_panel();
    if (panel) {
        esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
    }
    lv_display_flush_ready(disp);
}

void Relayout() {
    if (!screen_) {
        return;
    }
    const int w = HorRes();
    if (status_label_) {
        lv_obj_set_width(status_label_, w - 16);
        lv_obj_align(status_label_, LV_ALIGN_TOP_MID, 0, 4);
    }
    if (title_label_) {
        lv_obj_set_width(title_label_, w - 24);
        lv_obj_align(title_label_, LV_ALIGN_TOP_MID, 0, 28);
    }
    if (body_label_) {
        lv_obj_set_width(body_label_, w - 24);
        lv_obj_align(body_label_, LV_ALIGN_CENTER, 0, 20);
    }
    if (toast_label_) {
        lv_obj_set_width(toast_label_, w - 32);
        lv_obj_align(toast_label_, LV_ALIGN_BOTTOM_MID, 0, -8);
    }
}

void RefreshStatus() {
    if (!status_label_) {
        return;
    }
    char line[48];
    const bool wifi_ok = WifiService::Instance().phase() == WifiPhase::Connected;
    const int rssi = WifiService::Instance().rssi();
    const char* wifi = !wifi_ok ? "WiFi--" : (rssi >= -60 ? "WiFi强" : (rssi >= -70 ? "WiFi中" : "WiFi弱"));
    const int bat = Board::Instance().battery_percent();
    const char* chg = Board::Instance().is_charging() ? "充" : "";
    snprintf(line, sizeof(line), "%s  %d%%%s", wifi, bat, chg);
    lv_label_set_text(status_label_, line);
}

void EnsureWidgets() {
    if (screen_) {
        return;
    }
    screen_ = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, 0);
    lv_screen_load(screen_);

    const lv_font_t* font = UiFont();
    status_label_ = lv_label_create(screen_);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(0x88CC88), 0);
    lv_obj_set_style_text_font(status_label_, font, 0);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_CLIP);
    lv_label_set_text(status_label_, "");

    title_label_ = lv_label_create(screen_);
    lv_obj_set_style_text_color(title_label_, lv_color_white(), 0);
    lv_obj_set_style_text_font(title_label_, font, 0);
    lv_label_set_long_mode(title_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(title_label_, "");

    body_label_ = lv_label_create(screen_);
    lv_obj_set_style_text_color(body_label_, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(body_label_, font, 0);
    lv_label_set_long_mode(body_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(body_label_, "");

    toast_label_ = lv_label_create(screen_);
    lv_obj_set_style_text_color(toast_label_, lv_color_hex(0xFFEE88), 0);
    lv_obj_set_style_text_font(toast_label_, font, 0);
    lv_label_set_long_mode(toast_label_, LV_LABEL_LONG_CLIP);
    lv_label_set_text(toast_label_, "");
    Relayout();
}

void SetTexts(const char* title, const char* body) {
    EnsureWidgets();
    lv_label_set_text(title_label_, title ? title : "");
    lv_label_set_text(body_label_, body ? body : "");
    RefreshStatus();
}

}  // namespace

esp_err_t UiInit() {
    ESP_LOGI(TAG, "LVGL init for ST7789 %dx%d", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_init();

    display_ = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (!display_) {
        ESP_LOGE(TAG, "lv_display_create failed");
        return ESP_FAIL;
    }

    const size_t buf_pixels = kMaxBufW * 40;
    static lv_color_t buf1[kMaxBufW * 40];
    static lv_color_t buf2[kMaxBufW * 40];
    lv_display_set_buffers(display_, buf1, buf2, buf_pixels * sizeof(lv_color_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_color_format(display_, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display_, FlushCb);

    EnsureWidgets();
    SetTexts("Meet", "启动中");
    return ESP_OK;
}

void UiShowPairing(const char* code, const char* hint) {
    SetTexts(code && code[0] ? code : "------", hint && hint[0] ? hint : "在网页输入配对码");
}

void UiShowReady(const char* character_name) {
    const char* name =
        (character_name && character_name[0] != '\0') ? character_name : "未选择角色";
    SetTexts(name, "等待通话  说嗨乐鑫");
}

void UiShowUnprovisioned() {
    SetTexts("未配网", "请配置家庭 Wi-Fi");
}

void UiShowWifiConfig(const char* ap_ssid, const char* url) {
    char body[160];
    snprintf(body, sizeof(body), "手机连接 %s\n浏览器打开 %s", ap_ssid ? ap_ssid : "Meet",
             url ? url : "http://192.168.4.1");
    SetTexts("配网", body);
}

void UiShowWifiConnecting(const char* ssid) {
    SetTexts("连接 Wi-Fi", ssid ? ssid : "");
}

void UiShowConnecting() {
    SetTexts("连接中", "正在接通角色");
}

void UiShowInCall(const char* subtitle) {
    SetTexts("通话中", subtitle ? subtitle : "");
}

void UiShowSettings(const char* const* items, int count, int index) {
    char body[320] = {};
    size_t used = 0;
    for (int i = 0; i < count; ++i) {
        const char* prefix = (i == index) ? "> " : "  ";
        const char* label = items[i] ? items[i] : "";
        const int n = snprintf(body + used, sizeof(body) - used, "%s%s%s", prefix, label,
                               (i + 1 < count) ? "\n" : "");
        if (n < 0) {
            break;
        }
        used += static_cast<size_t>(n);
        if (used >= sizeof(body)) {
            break;
        }
    }
    SetTexts("设置", body);
}

void UiShowToast(const char* text) {
    EnsureWidgets();
    lv_label_set_text(toast_label_, text ? text : "");
    toast_ms_left_ = 1500;
}

void UiApplyOrientation(bool landscape) {
    landscape_ = landscape;
    Board::Instance().ApplyOrientation(landscape);
    if (display_) {
        lv_display_set_rotation(display_,
                                landscape ? LV_DISPLAY_ROTATION_90 : LV_DISPLAY_ROTATION_0);
    }
    Relayout();
}

void UiTick(uint32_t elapsed_ms) {
    lv_tick_inc(elapsed_ms);
    lv_timer_handler();
    if (toast_ms_left_ > 0) {
        toast_ms_left_ -= static_cast<int>(elapsed_ms);
        if (toast_ms_left_ <= 0) {
            toast_ms_left_ = 0;
            if (toast_label_) {
                lv_label_set_text(toast_label_, "");
            }
        }
    }
    status_accum_ms_ += elapsed_ms;
    if (status_accum_ms_ >= 1000) {
        status_accum_ms_ = 0;
        RefreshStatus();
    }
}

}  // namespace meet
