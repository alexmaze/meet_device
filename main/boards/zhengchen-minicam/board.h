#pragma once

#include "config.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <cstdint>

namespace meet {

/**
 * zhengchen-minicam board bring-up.
 * Input handlers post AppEvents — no heavy callbacks.
 */
class Board {
public:
    static Board& Instance();

    esp_err_t Init();

    i2c_master_bus_handle_t i2c_bus() const { return i2c_bus_; }
    esp_lcd_panel_handle_t lcd_panel() const { return lcd_panel_; }
    esp_lcd_panel_io_handle_t lcd_io() const { return lcd_io_; }
    int display_width() const { return landscape_ ? DISPLAY_WIDTH_1 : DISPLAY_WIDTH; }
    int display_height() const { return landscape_ ? DISPLAY_HEIGHT_1 : DISPLAY_HEIGHT; }
    bool landscape() const { return landscape_; }

    void SetBacklightPercent(int percent);
    void ApplyOrientation(bool landscape);
    void SetTalking(bool talking);

    int battery_percent() const { return battery_percent_; }
    bool is_charging() const { return is_charging_; }

private:
    Board() = default;

    enum class AdcVolumeKeyState : uint8_t {
        None,
        VolumeDown,
        VolumeUp,
    };

    esp_err_t InitI2c();
    esp_err_t InitSpiLcd();
    esp_err_t InitBacklight();
    esp_err_t InitBootButton();
    esp_err_t InitAdc();

    void HandleAdcVolumeKey(int voltage_mv);
    void UpdateBatteryState(int battery_raw, int battery_voltage, int ref_raw, int ref_voltage);

    static void BootButtonTask(void* arg);
    static void AdcReadTask(void* arg);
    static void VolumeKeyTask(void* arg);
    static bool OnColorTransDone(esp_lcd_panel_io_handle_t panel_io,
                                 esp_lcd_panel_io_event_data_t* edata,
                                 void* user_ctx);

    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t lcd_io_ = nullptr;
    esp_lcd_panel_handle_t lcd_panel_ = nullptr;
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    adc_cali_handle_t adc_cali_handle_ = nullptr;
    void* adc_mutex_ = nullptr;

    static constexpr size_t kBatteryAverageWindowSize = 8;
    int battery_samples_mv_[kBatteryAverageWindowSize] = {};
    int battery_ref_samples_mv_[kBatteryAverageWindowSize] = {};
    size_t battery_sample_count_ = 0;
    size_t battery_sample_index_ = 0;
    int battery_percent_ = 100;
    bool is_charging_ = false;
    bool landscape_ = true;
    bool backlight_inverted_ = DISPLAY_BACKLIGHT_OUTPUT_INVERT;

    AdcVolumeKeyState volume_key_state_ = AdcVolumeKeyState::None;
    AdcVolumeKeyState volume_key_candidate_state_ = AdcVolumeKeyState::None;
    uint8_t volume_key_stable_count_ = 0;
};

}  // namespace meet
