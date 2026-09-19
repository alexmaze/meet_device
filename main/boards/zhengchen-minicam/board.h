#pragma once

#include "config.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <functional>
#include <cstdint>

namespace meet {

using BootClickCallback = std::function<void()>;
using BootDoubleClickCallback = std::function<void()>;

/**
 * Simplified zhengchen-minicam board bring-up.
 * Self-contained — no xiaozhi Application / Display stack.
 */
class Board {
public:
    static Board& Instance();

    esp_err_t Init();

    i2c_master_bus_handle_t i2c_bus() const { return i2c_bus_; }
    esp_lcd_panel_handle_t lcd_panel() const { return lcd_panel_; }
    esp_lcd_panel_io_handle_t lcd_io() const { return lcd_io_; }
    int display_width() const { return DISPLAY_WIDTH; }
    int display_height() const { return DISPLAY_HEIGHT; }

    void SetBacklightPercent(int percent);
    void SetBootClickHandler(BootClickCallback cb);
    void SetBootDoubleClickHandler(BootDoubleClickCallback cb);

    /** Battery / volume stubs — filled by ADC tasks when ready. */
    int battery_percent() const { return battery_percent_; }
    bool is_charging() const { return is_charging_; }

private:
    Board() = default;

    esp_err_t InitI2c();
    esp_err_t InitSpiLcd();
    esp_err_t InitBacklight();
    esp_err_t InitBootButton();
    esp_err_t InitAdcStubs();

    static void BootButtonTask(void* arg);

    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t lcd_io_ = nullptr;
    esp_lcd_panel_handle_t lcd_panel_ = nullptr;
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;

    BootClickCallback on_boot_click_;
    BootDoubleClickCallback on_boot_double_click_;

    int battery_percent_ = 100;
    bool is_charging_ = false;
    bool backlight_inverted_ = DISPLAY_BACKLIGHT_OUTPUT_INVERT;
};

}  // namespace meet
