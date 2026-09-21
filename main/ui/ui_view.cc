#include "ui_view.h"

#include "ui_face.h"

#include <lvgl.h>
#include <cstdio>
#include <cstring>

extern "C" {
LV_FONT_DECLARE(font_meet_cjk_16_4);
}

namespace meet {
namespace {

UiEnv env_;
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

const lv_font_t* UiFont() {
    return &font_meet_cjk_16_4;
}

void RefreshStatus() {
    if (!status_label_) return;
    char line[48];
    const char* wifi = !env_.wifi_ok
                           ? "WiFi--"
                           : (env_.rssi >= -60 ? "WiFi强"
                                              : (env_.rssi >= -70 ? "WiFi中" : "WiFi弱"));
    const char* chg = env_.charging ? "充" : "";
    snprintf(line, sizeof(line), "%s  %d%%%s", wifi, env_.battery_percent, chg);
    lv_label_set_text(status_label_, line);
}

void ApplyTexts(const char* title, const char* body, bool show_face) {
    UiViewEnsureWidgets();
    face_visible_ = show_face;
    if (show_face) {
        FaceDrawEmotion(emotion_name_, face_buf_, kFaceSize);
        if (face_canvas_) lv_obj_invalidate(face_canvas_);
    }
    UiViewRelayout();
    lv_label_set_text(title_label_, title ? title : "");
    const char* shown = body;
    if (show_face && caption_[0]) {
        shown = caption_;
    }
    lv_label_set_text(body_label_, shown ? shown : "");
    RefreshStatus();
}

}  // namespace

void UiViewSetEnv(const UiEnv& env) {
    env_ = env;
}

void UiViewRelayout() {
    if (!screen_) return;
    const int w = env_.width;
    const bool landscape = env_.landscape;
    if (status_label_) {
        lv_obj_set_width(status_label_, w - 16);
        lv_obj_align(status_label_, LV_ALIGN_TOP_MID, 0, landscape ? 2 : 4);
    }
    if (face_canvas_) {
        lv_obj_align(face_canvas_, LV_ALIGN_TOP_MID, 0, landscape ? 18 : 22);
        if (face_visible_) lv_obj_clear_flag(face_canvas_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(face_canvas_, LV_OBJ_FLAG_HIDDEN);
    }
    if (title_label_) {
        lv_obj_set_width(title_label_, w - 24);
        const int title_y = face_visible_ ? (landscape ? 84 : 90) : (landscape ? 22 : 28);
        lv_obj_align(title_label_, LV_ALIGN_TOP_MID, 0, title_y);
    }
    if (body_label_) {
        lv_obj_set_width(body_label_, w - 24);
        lv_obj_align(body_label_, LV_ALIGN_CENTER, 0,
                     face_visible_ ? (landscape ? 28 : 36) : (landscape ? 12 : 20));
    }
    if (toast_label_) {
        lv_obj_set_width(toast_label_, w - 32);
        lv_obj_align(toast_label_, LV_ALIGN_BOTTOM_MID, 0, landscape ? -4 : -8);
    }
}

void UiViewShowPlain(const char* title, const char* body) {
    ApplyTexts(title, body, false);
}

void UiViewEnsureWidgets() {
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
    UiViewRelayout();
}

void UiViewApplyCommand(const UiCommand& cmd) {
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
            UiViewEnsureWidgets();
            lv_label_set_text(toast_label_, cmd.title);
            toast_ms_left_ = 1500;
            break;
        case UiCmdType::Emotion:
            UiViewEnsureWidgets();
            strncpy(emotion_name_, cmd.title[0] ? cmd.title : "neutral", sizeof(emotion_name_) - 1);
            emotion_name_[sizeof(emotion_name_) - 1] = 0;
            if (face_visible_) {
                FaceDrawEmotion(emotion_name_, face_buf_, kFaceSize);
                if (face_canvas_) lv_obj_invalidate(face_canvas_);
                UiViewRelayout();
            }
            break;
        case UiCmdType::Caption:
            UiViewEnsureWidgets();
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

void UiViewTick(uint32_t elapsed_ms) {
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
