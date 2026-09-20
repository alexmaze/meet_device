#include "ui.h"

#include "board.h"
#include "config.h"
#include "ui_face.h"
#include "wifi_service.h"

#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <lvgl.h>
#include <cstdio>
#include <cstring>

namespace meet {
namespace {

constexpr char TAG[] = "ui";
constexpr int kMaxBufW = DISPLAY_WIDTH;
constexpr int kQueueDepth = 16;

enum class UiCmdType : uint8_t {
    Pairing,
    Ready,
    Unprovisioned,
    WifiConfig,
    WifiConnecting,
    Connecting,
    InCall,
    Settings,
    Toast,
    Emotion,
    Caption,
    ClearCaption,
};

struct UiCommand {
    UiCmdType type = UiCmdType::Ready;
    char title[48] = {};
    char body[320] = {};
};

QueueHandle_t g_ui_q = nullptr;
lv_display_t* display_ = nullptr;
lv_obj_t* screen_ = nullptr;
lv_obj_t* status_label_ = nullptr;
lv_obj_t* title_label_ = nullptr;
lv_obj_t* body_label_ = nullptr;
lv_obj_t* toast_label_ = nullptr;
lv_obj_t* face_canvas_ = nullptr;
int toast_ms_left_ = 0;
uint32_t status_accum_ms_ = 0;
bool face_visible_ = false;
char emotion_name_[24] = "neutral";
char caption_[160] = {};
uint16_t face_buf_[kFaceSize * kFaceSize];

#if !LV_FONT_SOURCE_HAN_SANS_SC_16_CJK
#error "CONFIG_LV_FONT_SOURCE_HAN_SANS_SC_16_CJK must be enabled — Meet UI is Chinese-only"
#endif

const lv_font_t* UiFont() {
    return &lv_font_source_han_sans_sc_16_cjk;
}

void PostCmd(const UiCommand& cmd) {
    if (!g_ui_q) {
        return;
    }
    if (xQueueSend(g_ui_q, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "ui queue full, drop type=%d", static_cast<int>(cmd.type));
    }
}

void Relayout() {
    if (!screen_) return;
    const int w = DISPLAY_WIDTH;
    if (status_label_) {
        lv_obj_set_width(status_label_, w - 16);
        lv_obj_align(status_label_, LV_ALIGN_TOP_MID, 0, 4);
    }
    if (face_canvas_) {
        lv_obj_align(face_canvas_, LV_ALIGN_TOP_MID, 0, 22);
        if (face_visible_) lv_obj_clear_flag(face_canvas_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(face_canvas_, LV_OBJ_FLAG_HIDDEN);
    }
    if (title_label_) {
        lv_obj_set_width(title_label_, w - 24);
        lv_obj_align(title_label_, LV_ALIGN_TOP_MID, 0, face_visible_ ? 90 : 28);
    }
    if (body_label_) {
        lv_obj_set_width(body_label_, w - 24);
        lv_obj_align(body_label_, LV_ALIGN_CENTER, 0, face_visible_ ? 36 : 20);
    }
    if (toast_label_) {
        lv_obj_set_width(toast_label_, w - 32);
        lv_obj_align(toast_label_, LV_ALIGN_BOTTOM_MID, 0, -8);
    }
}

void RefreshStatus() {
    if (!status_label_) return;
    char line[48];
    const bool wifi_ok = WifiService::Instance().phase() == WifiPhase::Connected;
    const int rssi = WifiService::Instance().rssi();
    const char* wifi =
        !wifi_ok ? "WiFi--" : (rssi >= -60 ? "WiFi强" : (rssi >= -70 ? "WiFi中" : "WiFi弱"));
    const int bat = Board::Instance().battery_percent();
    const char* chg = Board::Instance().is_charging() ? "充" : "";
    snprintf(line, sizeof(line), "%s  %d%%%s", wifi, bat, chg);
    lv_label_set_text(status_label_, line);
}

void EnsureWidgets() {
    if (screen_) return;
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

    face_canvas_ = lv_canvas_create(screen_);
    lv_canvas_set_buffer(face_canvas_, face_buf_, kFaceSize, kFaceSize, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(face_canvas_, kFaceSize, kFaceSize);
    FaceDrawEmotion(emotion_name_, face_buf_, kFaceSize);
    Relayout();
}

void ApplyTexts(const char* title, const char* body, bool show_face) {
    EnsureWidgets();
    face_visible_ = show_face;
    if (show_face) {
        FaceDrawEmotion(emotion_name_, face_buf_, kFaceSize);
        if (face_canvas_) lv_obj_invalidate(face_canvas_);
    }
    Relayout();
    lv_label_set_text(title_label_, title ? title : "");
    const char* shown = body;
    if (show_face && caption_[0]) {
        shown = caption_;
    }
    lv_label_set_text(body_label_, shown ? shown : "");
    RefreshStatus();
}

void ApplyCommand(const UiCommand& cmd) {
    switch (cmd.type) {
        case UiCmdType::Pairing:
            ApplyTexts(cmd.title, cmd.body[0] ? cmd.body : "在网页输入配对码", false);
            break;
        case UiCmdType::Ready:
            if (!emotion_name_[0] || strcmp(emotion_name_, "thinking") == 0) {
                strncpy(emotion_name_, "neutral", sizeof(emotion_name_) - 1);
            }
            ApplyTexts(cmd.title[0] ? cmd.title : "未选择角色", "单击通话 · 说唤醒词", true);
            break;
        case UiCmdType::Unprovisioned:
            ApplyTexts("未配网", "请配置家庭 Wi-Fi", false);
            break;
        case UiCmdType::WifiConfig:
            ApplyTexts("配网", cmd.body, false);
            break;
        case UiCmdType::WifiConnecting:
            ApplyTexts("连接 Wi-Fi", cmd.title, false);
            break;
        case UiCmdType::Connecting:
            strncpy(emotion_name_, "thinking", sizeof(emotion_name_) - 1);
            ApplyTexts("连接中", "正在接通角色", true);
            break;
        case UiCmdType::InCall:
            ApplyTexts("通话中", cmd.body, true);
            break;
        case UiCmdType::Settings:
            ApplyTexts("设置", cmd.body, false);
            break;
        case UiCmdType::Toast:
            EnsureWidgets();
            lv_label_set_text(toast_label_, cmd.title);
            toast_ms_left_ = 1500;
            break;
        case UiCmdType::Emotion:
            EnsureWidgets();
            strncpy(emotion_name_, cmd.title[0] ? cmd.title : "neutral", sizeof(emotion_name_) - 1);
            emotion_name_[sizeof(emotion_name_) - 1] = 0;
            if (face_visible_) {
                FaceDrawEmotion(emotion_name_, face_buf_, kFaceSize);
                if (face_canvas_) lv_obj_invalidate(face_canvas_);
                Relayout();
            }
            break;
        case UiCmdType::Caption:
            EnsureWidgets();
            strncpy(caption_, cmd.body, sizeof(caption_) - 1);
            caption_[sizeof(caption_) - 1] = 0;
            if (body_label_ && face_visible_) {
                lv_label_set_text(body_label_, caption_[0] ? caption_ : "");
            }
            break;
        case UiCmdType::ClearCaption:
            caption_[0] = 0;
            if (body_label_ && face_visible_) {
                lv_label_set_text(body_label_, "");
            }
            break;
    }
}

void FlushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    auto panel = Board::Instance().lcd_panel();
    if (panel) {
        esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
        // flush_ready is called from on_color_trans_done (UiNotifyFlushDone)
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
    ESP_LOGI(TAG, "LVGL init for ST7789 %dx%d", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    g_ui_q = xQueueCreate(kQueueDepth, sizeof(UiCommand));
    if (!g_ui_q) {
        return ESP_ERR_NO_MEM;
    }

    lv_init();
    display_ = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
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

    EnsureWidgets();
    ApplyTexts("Meet", "启动中", false);
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
    UiCommand cmd;
    while (g_ui_q && xQueueReceive(g_ui_q, &cmd, 0) == pdTRUE) {
        ApplyCommand(cmd);
    }
    lv_tick_inc(elapsed_ms);
    lv_timer_handler();
    if (toast_ms_left_ > 0) {
        toast_ms_left_ -= static_cast<int>(elapsed_ms);
        if (toast_ms_left_ <= 0) {
            toast_ms_left_ = 0;
            if (toast_label_) lv_label_set_text(toast_label_, "");
        }
    }
    status_accum_ms_ += elapsed_ms;
    if (status_accum_ms_ >= 1000) {
        status_accum_ms_ = 0;
        RefreshStatus();
    }
}

}  // namespace meet
