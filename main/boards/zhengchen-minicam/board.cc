#include "board.h"

#include <esp_adc/adc_cali_scheme.h>
#include <esp_check.h>
#include <esp_log.h>
#include <driver/ledc.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_vendor.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace meet {
namespace {

constexpr char TAG[] = "board";
constexpr int kBacklightLedcChannel = LEDC_CHANNEL_0;
constexpr int kBacklightLedcTimer = LEDC_TIMER_0;
constexpr int kDebounceMs = 40;
constexpr int kDoubleClickMs = 400;
constexpr int kLongPressMs = 3000;

}  // namespace

Board& Board::Instance() {
    static Board board;
    return board;
}

esp_err_t Board::Init() {
    ESP_LOGI(TAG, "zhengchen-minicam bring-up");
    ESP_RETURN_ON_ERROR(InitI2c(), TAG, "i2c");
    ESP_RETURN_ON_ERROR(InitSpiLcd(), TAG, "lcd");
    ESP_RETURN_ON_ERROR(InitBacklight(), TAG, "backlight");
    ESP_RETURN_ON_ERROR(InitBootButton(), TAG, "boot");
    ESP_RETURN_ON_ERROR(InitAdc(), TAG, "adc");
    SetBacklightPercent(80);
    return ESP_OK;
}

esp_err_t Board::InitI2c() {
    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port = I2C_NUM_0;
    cfg.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
    cfg.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = 1;
    return i2c_new_master_bus(&cfg, &i2c_bus_);
}

esp_err_t Board::InitSpiLcd() {
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
    buscfg.miso_io_num = GPIO_NUM_NC;
    buscfg.sclk_io_num = DISPLAY_CLK_PIN;
    buscfg.quadwp_io_num = GPIO_NUM_NC;
    buscfg.quadhd_io_num = GPIO_NUM_NC;
    buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "spi");

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = DISPLAY_CS_PIN;
    io_config.dc_gpio_num = DISPLAY_DC_PIN;
    io_config.spi_mode = DISPLAY_SPI_MODE;
    io_config.pclk_hz = 40 * 1000 * 1000;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &lcd_io_), TAG, "panel_io");

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = DISPLAY_RST_PIN;
    panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
    panel_config.bits_per_pixel = 16;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(lcd_io_, &panel_config, &lcd_panel_), TAG, "st7789");

    esp_lcd_panel_reset(lcd_panel_);
    esp_lcd_panel_init(lcd_panel_);
    esp_lcd_panel_invert_color(lcd_panel_, DISPLAY_INVERT_COLOR);
    ApplyOrientation(false);
    esp_lcd_panel_disp_on_off(lcd_panel_, true);
    ESP_LOGI(TAG, "ST7789 %dx%d ready", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    return ESP_OK;
}

esp_err_t Board::InitBacklight() {
    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = LEDC_TIMER_10_BIT;
    timer.timer_num = static_cast<ledc_timer_t>(kBacklightLedcTimer);
    timer.freq_hz = 25000;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc_timer");

    ledc_channel_config_t channel = {};
    channel.gpio_num = DISPLAY_BACKLIGHT_PIN;
    channel.speed_mode = LEDC_LOW_SPEED_MODE;
    channel.channel = static_cast<ledc_channel_t>(kBacklightLedcChannel);
    channel.intr_type = LEDC_INTR_DISABLE;
    channel.timer_sel = static_cast<ledc_timer_t>(kBacklightLedcTimer);
    channel.duty = 0;
    channel.hpoint = 0;
    return ledc_channel_config(&channel);
}

void Board::SetBacklightPercent(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint32_t duty = (1023 * static_cast<uint32_t>(percent)) / 100;
    if (backlight_inverted_) {
        duty = 1023 - duty;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(kBacklightLedcChannel), duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(kBacklightLedcChannel));
}

void Board::ApplyOrientation(bool landscape) {
    landscape_ = landscape;
    if (!lcd_panel_) {
        return;
    }
    if (landscape_) {
        esp_lcd_panel_swap_xy(lcd_panel_, DISPLAY_SWAP_XY_1);
        esp_lcd_panel_mirror(lcd_panel_, DISPLAY_MIRROR_X_1, DISPLAY_MIRROR_Y_1);
    } else {
        esp_lcd_panel_swap_xy(lcd_panel_, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(lcd_panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }
}

void Board::SetTalking(bool talking) {
    gpio_set_level(STATE_OUTPUT_GPIO, talking ? 0 : 1);
}

esp_err_t Board::InitBootButton() {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "boot_gpio");

    gpio_config_t state = {};
    state.pin_bit_mask = 1ULL << STATE_OUTPUT_GPIO;
    state.mode = GPIO_MODE_OUTPUT;
    state.pull_up_en = GPIO_PULLUP_DISABLE;
    state.pull_down_en = GPIO_PULLDOWN_DISABLE;
    state.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&state), TAG, "state_gpio");
    gpio_set_level(STATE_OUTPUT_GPIO, 1);

    xTaskCreate(BootButtonTask, "boot_btn", 3072, this, 5, nullptr);
    return ESP_OK;
}

void Board::BootButtonTask(void* arg) {
    auto* self = static_cast<Board*>(arg);
    int clicks = 0;
    bool holding = false;
    bool long_fired = false;
    TickType_t down_tick = 0;
    TickType_t last_down = 0;

    while (true) {
        const int level = gpio_get_level(BOOT_BUTTON_GPIO);
        if (level == 0) {
            if (!holding) {
                vTaskDelay(pdMS_TO_TICKS(kDebounceMs));
                if (gpio_get_level(BOOT_BUTTON_GPIO) != 0) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                    continue;
                }
                holding = true;
                long_fired = false;
                down_tick = xTaskGetTickCount();
            } else if (!long_fired &&
                       (xTaskGetTickCount() - down_tick) > pdMS_TO_TICKS(kLongPressMs)) {
                long_fired = true;
                clicks = 0;
                if (self->on_boot_long_press_) {
                    self->on_boot_long_press_();
                }
            }
        } else if (holding) {
            holding = false;
            if (!long_fired) {
                const TickType_t now = xTaskGetTickCount();
                if (clicks > 0 && (now - last_down) < pdMS_TO_TICKS(kDoubleClickMs)) {
                    clicks = 0;
                    if (self->on_boot_double_click_) {
                        self->on_boot_double_click_();
                    }
                } else {
                    clicks = 1;
                    last_down = now;
                }
            }
        } else if (clicks == 1 && (xTaskGetTickCount() - last_down) > pdMS_TO_TICKS(kDoubleClickMs)) {
            clicks = 0;
            if (self->on_boot_click_) {
                self->on_boot_click_();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void Board::SetBootClickHandler(BootClickCallback cb) {
    on_boot_click_ = std::move(cb);
}

void Board::SetBootDoubleClickHandler(BootDoubleClickCallback cb) {
    on_boot_double_click_ = std::move(cb);
}

void Board::SetBootLongPressHandler(BootLongPressCallback cb) {
    on_boot_long_press_ = std::move(cb);
}

void Board::SetVolumeKeyHandler(VolumeKeyCallback cb) {
    on_volume_key_ = std::move(cb);
}

void Board::HandleAdcVolumeKey(int voltage_mv) {
    AdcVolumeKeyState new_state = AdcVolumeKeyState::None;
    if (voltage_mv >= VOLUME_DOWN_KEY_MIN_MV && voltage_mv <= VOLUME_DOWN_KEY_MAX_MV) {
        new_state = AdcVolumeKeyState::VolumeDown;
    } else if (voltage_mv >= VOLUME_UP_KEY_MIN_MV && voltage_mv <= VOLUME_UP_KEY_MAX_MV) {
        new_state = AdcVolumeKeyState::VolumeUp;
    }

    if (new_state != volume_key_candidate_state_) {
        volume_key_candidate_state_ = new_state;
        volume_key_stable_count_ = 1;
        return;
    }
    if (volume_key_stable_count_ < 3) {
        ++volume_key_stable_count_;
        return;
    }
    if (new_state == volume_key_state_) {
        return;
    }

    if (new_state == AdcVolumeKeyState::VolumeDown && on_volume_key_) {
        on_volume_key_(-10);
    } else if (new_state == AdcVolumeKeyState::VolumeUp && on_volume_key_) {
        on_volume_key_(10);
    }
    volume_key_state_ = new_state;
}

void Board::UpdateBatteryState(int battery_raw, int battery_voltage, int ref_raw, int ref_voltage) {
    battery_samples_mv_[battery_sample_index_] = battery_voltage;
    battery_ref_samples_mv_[battery_sample_index_] = ref_voltage;
    battery_sample_index_ = (battery_sample_index_ + 1) % kBatteryAverageWindowSize;
    if (battery_sample_count_ < kBatteryAverageWindowSize) {
        ++battery_sample_count_;
    }

    int battery_voltage_sum = 0;
    int ref_voltage_sum = 0;
    for (size_t i = 0; i < battery_sample_count_; ++i) {
        battery_voltage_sum += battery_samples_mv_[i];
        ref_voltage_sum += battery_ref_samples_mv_[i];
    }
    battery_voltage = battery_voltage_sum / static_cast<int>(battery_sample_count_);
    ref_voltage = ref_voltage_sum / static_cast<int>(battery_sample_count_);

    is_charging_ = ref_raw > 2300;

    float battery_voltage_v = 0.0f;
    if (is_charging_) {
        battery_voltage_v = static_cast<float>(battery_voltage) * 2.0f / 1000.0f;
    } else {
        if (ref_voltage <= 0) {
            return;
        }
        battery_voltage_v = static_cast<float>(battery_voltage) *
                            static_cast<float>(BATTERY_REF_VOLTAGE_MV) * 2.0f /
                            (static_cast<float>(ref_voltage) * 1000.0f);
    }

    int percent = static_cast<int>(((battery_voltage_v - 3.4f) * 100.0f / 0.8f) + 0.5f);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    battery_percent_ = percent;
    (void)battery_raw;
}

void Board::AdcReadTask(void* arg) {
    auto* self = static_cast<Board*>(arg);
    while (true) {
        int battery_raw = 0;
        int ref_raw = 0;
        int battery_voltage = 0;
        int ref_voltage = 0;
        if (self->adc_mutex_ &&
            xSemaphoreTake(static_cast<SemaphoreHandle_t>(self->adc_mutex_), portMAX_DELAY) ==
                pdTRUE) {
            if (self->adc_handle_) {
                adc_oneshot_read(self->adc_handle_, BATTERY_ADC_CHANNEL, &battery_raw);
                adc_oneshot_read(self->adc_handle_, BATTERY_REF_ADC_CHANNEL, &ref_raw);
            }
            xSemaphoreGive(static_cast<SemaphoreHandle_t>(self->adc_mutex_));
        }
        if (self->adc_cali_handle_) {
            adc_cali_raw_to_voltage(self->adc_cali_handle_, battery_raw, &battery_voltage);
            adc_cali_raw_to_voltage(self->adc_cali_handle_, ref_raw, &ref_voltage);
            self->UpdateBatteryState(battery_raw, battery_voltage, ref_raw, ref_voltage);
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void Board::VolumeKeyTask(void* arg) {
    auto* self = static_cast<Board*>(arg);
    while (true) {
        int volume_key_raw = 0;
        int volume_key_voltage = 0;
        if (self->adc_mutex_ &&
            xSemaphoreTake(static_cast<SemaphoreHandle_t>(self->adc_mutex_), portMAX_DELAY) ==
                pdTRUE) {
            if (self->adc_handle_) {
                adc_oneshot_read(self->adc_handle_, VOLUME_KEY_ADC_CHANNEL, &volume_key_raw);
            }
            xSemaphoreGive(static_cast<SemaphoreHandle_t>(self->adc_mutex_));
        }
        if (self->adc_cali_handle_) {
            adc_cali_raw_to_voltage(self->adc_cali_handle_, volume_key_raw, &volume_key_voltage);
            self->HandleAdcVolumeKey(volume_key_voltage);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t Board::InitAdc() {
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&init_config, &adc_handle_);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC unit init failed: %s", esp_err_to_name(err));
        adc_handle_ = nullptr;
        return ESP_OK;
    }

    adc_oneshot_chan_cfg_t chan = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_oneshot_config_channel(adc_handle_, BATTERY_ADC_CHANNEL, &chan);
    adc_oneshot_config_channel(adc_handle_, BATTERY_REF_ADC_CHANNEL, &chan);
    adc_oneshot_config_channel(adc_handle_, VOLUME_KEY_ADC_CHANNEL, &chan);

    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .chan = BATTERY_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_config, &adc_cali_handle_) != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration not available");
        adc_cali_handle_ = nullptr;
    }

    adc_mutex_ = xSemaphoreCreateMutex();
    if (!adc_mutex_) {
        return ESP_ERR_NO_MEM;
    }
    xTaskCreate(AdcReadTask, "adc_bat", 3072, this, 4, nullptr);
    xTaskCreatePinnedToCore(VolumeKeyTask, "adc_vol", 3072, this, 6, nullptr, 1);
    ESP_LOGI(TAG, "ADC battery GPIO3/4 + volume GPIO9 ready");
    return ESP_OK;
}

}  // namespace meet
