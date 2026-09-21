#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

#include "ui_face.h"
#include "ui_view.h"

#include <lvgl.h>

#include <cstdio>
#include <cstring>

namespace {

using meet::UiCmdType;
using meet::UiCommand;
using meet::UiEnv;

constexpr int kLandscapeW = 320;
constexpr int kLandscapeH = 240;
constexpr int kPortraitW = 240;
constexpr int kPortraitH = 320;

lv_display_t* g_disp = nullptr;
UiEnv g_env{};
bool g_landscape = true;

const char* kEmotions[] = {
    "neutral", "happy", "laughing", "funny", "winking", "sad", "crying",
    "angry",   "loving", "kissy", "embarrassed", "surprised", "shocked",
    "thinking", "confused", "cool", "confident", "relaxed", "sleepy", "delicious",
};
constexpr int kEmotionCount = static_cast<int>(sizeof(kEmotions) / sizeof(kEmotions[0]));
int g_emotion_i = 0;
bool g_quit = false;

void ApplyEnv() {
    g_env.width = g_landscape ? kLandscapeW : kPortraitW;
    g_env.height = g_landscape ? kLandscapeH : kPortraitH;
    g_env.landscape = g_landscape;
    g_env.wifi_ok = true;
    g_env.rssi = -55;
    g_env.battery_percent = 87;
    g_env.charging = false;
    meet::UiViewSetEnv(g_env);
}

void SetOrientation(bool landscape) {
    g_landscape = landscape;
    ApplyEnv();
    if (g_disp) {
        lv_sdl_window_set_size(g_disp, g_env.width, g_env.height);
    }
    meet::UiViewRelayout();
}

void Post(UiCmdType type, const char* title = nullptr, const char* body = nullptr) {
    UiCommand cmd;
    cmd.type = type;
    if (title) {
        std::strncpy(cmd.title, title, sizeof(cmd.title) - 1);
    }
    if (body) {
        std::strncpy(cmd.body, body, sizeof(cmd.body) - 1);
    }
    meet::UiViewApplyCommand(cmd);
}

void PrintHelp() {
    std::printf(
        "Meet UI preview (320x240 @2x)\n"
        "  1 unprovisioned   2 wifi config   3 pairing\n"
        "  4 ready           5 connecting    6 in-call\n"
        "  7 settings\n"
        "  e cycle emotion   t toast         c caption\n"
        "  o toggle orientation   q quit\n");
}

void HandleKey(SDL_Keycode key) {
    switch (key) {
        case SDLK_1:
            Post(UiCmdType::Unprovisioned);
            break;
        case SDLK_2:
            Post(UiCmdType::WifiConfig, nullptr,
                 "手机连接 Meet-A1B2\n浏览器打开 http://192.168.4.1");
            break;
        case SDLK_3:
            Post(UiCmdType::Pairing, "482917", "在网页输入配对码");
            break;
        case SDLK_4:
            Post(UiCmdType::Ready, "小伴");
            break;
        case SDLK_5:
            Post(UiCmdType::Connecting);
            break;
        case SDLK_6:
            Post(UiCmdType::InCall, nullptr, "通话中");
            break;
        case SDLK_7:
            Post(UiCmdType::Settings, nullptr,
                 "> 选择角色\n  重新配对\n  重新配网\n  检查更新");
            break;
        case SDLK_e:
            g_emotion_i = (g_emotion_i + 1) % kEmotionCount;
            Post(UiCmdType::Emotion, kEmotions[g_emotion_i]);
            std::printf("emotion: %s\n", kEmotions[g_emotion_i]);
            break;
        case SDLK_t:
            Post(UiCmdType::Toast, "网络较弱");
            break;
        case SDLK_c:
            Post(UiCmdType::Caption, nullptr, "你好呀，今天过得怎么样？");
            break;
        case SDLK_o:
            SetOrientation(!g_landscape);
            std::printf("orientation: %s\n", g_landscape ? "landscape" : "portrait");
            break;
        case SDLK_h:
        case SDLK_QUESTION:
            PrintHelp();
            break;
        default:
            break;
    }
}

int KeyWatch(void* /*userdata*/, SDL_Event* ev) {
    if (ev->type == SDL_QUIT) {
        g_quit = true;
        return 0;
    }
    if (ev->type == SDL_KEYDOWN && !ev->key.repeat) {
        const SDL_Keycode key = ev->key.keysym.sym;
        if (key == SDLK_q || key == SDLK_ESCAPE) {
            g_quit = true;
        } else {
            HandleKey(key);
        }
    }
    return 0;
}

}  // namespace

int main() {
    PrintHelp();

    lv_init();
    ApplyEnv();

    g_disp = lv_sdl_window_create(g_env.width, g_env.height);
    if (!g_disp) {
        std::fprintf(stderr, "lv_sdl_window_create failed\n");
        return 1;
    }
    lv_sdl_window_set_zoom(g_disp, 2.0f);
    lv_sdl_window_set_title(g_disp, "Meet UI Preview");
    lv_sdl_mouse_create();
    lv_sdl_keyboard_create();

    SDL_AddEventWatch(KeyWatch, nullptr);
    meet::UiViewShowPlain("Meet", "启动中 · 按 1-7 切页");

    uint32_t last_ms = SDL_GetTicks();
    while (!g_quit) {
        const uint32_t now = SDL_GetTicks();
        uint32_t elapsed = now - last_ms;
        if (elapsed < 1) elapsed = 1;
        last_ms = now;

        lv_tick_inc(elapsed);
        lv_timer_handler();
        meet::UiViewTick(elapsed);
        SDL_Delay(5);
    }

    SDL_DelEventWatch(KeyWatch, nullptr);
    lv_sdl_quit();
    return 0;
}
