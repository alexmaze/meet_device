#include "board.h"

#include <esp_log.h>
#include <esp_check.h>
#include <driver/ledc.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_vendor.h>
#include <freertos/FreeRTOS.h>
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
    ESP_RETURN_ON_ERROR(InitAdcStubs(), TAG, "adc");
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
    esp_lcd_panel_swap_xy(lcd_panel_, DISPLAY_SWAP_XY);
    esp_lcd_panel_mirror(lcd_panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
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
    int last = 1;
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
                    last = 1;
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
        last = level;
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

esp_err_t Board::InitAdcStubs() {
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&init_config, &adc_handle_);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC unit init deferred/failed: %s", esp_err_to_name(err));
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
    ESP_LOGW(TAG, "Battery/volume ADC configured; continuous sampling TODO");
    return ESP_OK;
}

}  // namespace meet
