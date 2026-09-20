#include "ui_face.h"

#include <cstring>

namespace meet {
namespace {

uint16_t Rgb(int r, int g, int b) {
    return static_cast<uint16_t>(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

void Pixel(uint16_t* buf, int size, int x, int y, uint16_t color) {
    if (x < 0 || y < 0 || x >= size || y >= size) return;
    buf[y * size + x] = color;
}

void Fill(uint16_t* buf, int size, uint16_t color) {
    for (int i = 0; i < size * size; ++i) buf[i] = color;
}

void Circle(uint16_t* buf, int size, int cx, int cy, int r, uint16_t color, bool fill) {
    const int r2 = r * r;
    const int inner = (r - 1) * (r - 1);
    for (int y = -r; y <= r; ++y) {
        for (int x = -r; x <= r; ++x) {
            const int d = x * x + y * y;
            if (fill) {
                if (d <= r2) Pixel(buf, size, cx + x, cy + y, color);
            } else if (d <= r2 && d >= inner) {
                Pixel(buf, size, cx + x, cy + y, color);
            }
        }
    }
}

void Rect(uint16_t* buf, int size, int x, int y, int w, int h, uint16_t color) {
    for (int yy = 0; yy < h; ++yy) {
        for (int xx = 0; xx < w; ++xx) Pixel(buf, size, x + xx, y + yy, color);
    }
}

}  // namespace

void FaceDrawEmotion(const char* name, uint16_t* buf, int size) {
    if (!buf || size < kFaceSize) return;
    const uint16_t bg = Rgb(0, 0, 0);
    const uint16_t yellow = Rgb(255, 210, 50);
    const uint16_t ink = Rgb(40, 30, 20);
    const uint16_t white = Rgb(255, 255, 255);
    const uint16_t blush = Rgb(255, 140, 140);
    Fill(buf, size, bg);
    Circle(buf, size, 32, 32, 28, yellow, true);

    const char* e = name && name[0] ? name : "neutral";
    auto eyes = [&](int ly, int ry, int h) {
        Rect(buf, size, 18, ly, 8, h, ink);
        Rect(buf, size, 38, ry, 8, h, ink);
    };
    auto smile = [&](int y, int w) {
        Rect(buf, size, 32 - w / 2, y, w, 2, ink);
        Rect(buf, size, 32 - w / 2, y - 3, 2, 4, ink);
        Rect(buf, size, 32 + w / 2 - 2, y - 3, 2, 4, ink);
    };

    if (strcmp(e, "happy") == 0 || strcmp(e, "laughing") == 0) {
        Rect(buf, size, 18, 24, 8, 3, ink);
        Rect(buf, size, 38, 24, 8, 3, ink);
        smile(42, 22);
        if (e[0] == 'l') Circle(buf, size, 16, 36, 4, blush, true);
    } else if (strcmp(e, "funny") == 0 || strcmp(e, "winking") == 0) {
        Rect(buf, size, 18, 24, 8, 3, ink);
        Rect(buf, size, 38, 26, 8, 2, ink);
        smile(42, 18);
    } else if (strcmp(e, "sad") == 0 || strcmp(e, "crying") == 0) {
        eyes(22, 22, 6);
        Rect(buf, size, 22, 42, 20, 2, ink);
        if (e[0] == 'c') {
            Rect(buf, size, 20, 30, 2, 8, Rgb(80, 160, 255));
            Rect(buf, size, 42, 30, 2, 8, Rgb(80, 160, 255));
        }
    } else if (strcmp(e, "angry") == 0) {
        Rect(buf, size, 16, 20, 10, 3, ink);
        Rect(buf, size, 38, 20, 10, 3, ink);
        Rect(buf, size, 22, 42, 20, 3, ink);
    } else if (strcmp(e, "loving") == 0 || strcmp(e, "kissy") == 0) {
        Circle(buf, size, 20, 24, 5, Rgb(230, 60, 80), true);
        Circle(buf, size, 44, 24, 5, Rgb(230, 60, 80), true);
        Circle(buf, size, 32, 42, 4, Rgb(200, 40, 60), true);
    } else if (strcmp(e, "embarrassed") == 0) {
        eyes(24, 24, 5);
        Circle(buf, size, 16, 36, 4, blush, true);
        Circle(buf, size, 48, 36, 4, blush, true);
        Rect(buf, size, 26, 42, 12, 2, ink);
    } else if (strcmp(e, "surprised") == 0 || strcmp(e, "shocked") == 0) {
        Circle(buf, size, 20, 24, 6, white, true);
        Circle(buf, size, 44, 24, 6, white, true);
        Circle(buf, size, 20, 24, 3, ink, true);
        Circle(buf, size, 44, 24, 3, ink, true);
        Circle(buf, size, 32, 44, 6, ink, true);
    } else if (strcmp(e, "thinking") == 0 || strcmp(e, "confused") == 0) {
        Rect(buf, size, 18, 22, 8, 5, ink);
        Rect(buf, size, 40, 24, 6, 3, ink);
        Rect(buf, size, 30, 42, 8, 2, ink);
    } else if (strcmp(e, "cool") == 0 || strcmp(e, "confident") == 0) {
        Rect(buf, size, 14, 22, 16, 6, ink);
        Rect(buf, size, 34, 22, 16, 6, ink);
        smile(42, 16);
    } else if (strcmp(e, "relaxed") == 0 || strcmp(e, "sleepy") == 0) {
        Rect(buf, size, 18, 26, 8, 2, ink);
        Rect(buf, size, 38, 26, 8, 2, ink);
        Rect(buf, size, 24, 42, 16, 2, ink);
    } else if (strcmp(e, "delicious") == 0) {
        Rect(buf, size, 18, 24, 8, 3, ink);
        Rect(buf, size, 38, 24, 8, 3, ink);
        Circle(buf, size, 32, 42, 5, Rgb(220, 80, 80), true);
    } else {
        eyes(22, 22, 6);
        Rect(buf, size, 22, 42, 20, 2, ink);
    }
}

}  // namespace meet
