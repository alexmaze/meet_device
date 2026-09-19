#include "ui.h"

#include "board.h"
#include "config.h"

#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>
#include <cstring>

namespace meet {
namespace {

constexpr char TAG[] = "ui";

lv_display_t* display_ = nullptr;
lv_obj_t* screen_ = nullptr;
lv_obj_t* title_label_ = nullptr;
lv_obj_t* body_label_ = nullptr;

void FlushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    auto panel = Board::Instance().lcd_panel();
    if (panel) {
        esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
    }
    lv_display_flush_ready(disp);
}

void EnsureWidgets() {
    if (screen_) {
        return;
    }
    screen_ = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, 0);
    lv_screen_load(screen_);

    title_label_ = lv_label_create(screen_);
    lv_obj_set_style_text_color(title_label_, lv_color_white(), 0);
    lv_obj_set_style_text_font(title_label_, &lv_font_montserrat_28, 0);
    lv_obj_align(title_label_, LV_ALIGN_TOP_MID, 0, 40);
    lv_label_set_text(title_label_, "");

    body_label_ = lv_label_create(screen_);
    lv_obj_set_style_text_color(body_label_, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(body_label_, &lv_font_montserrat_20, 0);
    lv_obj_set_width(body_label_, DISPLAY_WIDTH - 24);
    lv_label_set_long_mode(body_label_, LV_LABEL_LONG_WRAP);
    lv_obj_align(body_label_, LV_ALIGN_CENTER, 0, 20);
    lv_label_set_text(body_label_, "");
}

void SetTexts(const char* title, const char* body) {
    EnsureWidgets();
    lv_label_set_text(title_label_, title ? title : "");
    lv_label_set_text(body_label_, body ? body : "");
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

    const size_t buf_pixels = DISPLAY_WIDTH * 40;
    static lv_color_t buf1[DISPLAY_WIDTH * 40];
    static lv_color_t buf2[DISPLAY_WIDTH * 40];
    lv_display_set_buffers(display_, buf1, buf2, buf_pixels * sizeof(lv_color_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_color_format(display_, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display_, FlushCb);

    EnsureWidgets();
    SetTexts("Meet", "启动中…");
    return ESP_OK;
}

void UiShowPairing(const char* code) {
    SetTexts("配对码", code ? code : "------");
}

void UiShowReady(const char* character_name) {
    const char* name =
        (character_name && character_name[0] != '\0') ? character_name : "未选择角色";
    SetTexts(name, "等待通话");
}

void UiShowUnprovisioned() {
    SetTexts("未配置", "请配置 Wi-Fi 与 Meet 服务器");
}

void UiShowInCall(const char* subtitle) {
    SetTexts("通话中", subtitle ? subtitle : "");
}

void UiShowSettings(const char* item_label) {
    SetTexts("设置", item_label ? item_label : "");
}

void UiTick(uint32_t elapsed_ms) {
    lv_tick_inc(elapsed_ms);
    lv_timer_handler();
}

}  // namespace meet
