#include "meet_es8388.h"

#include "config.h"

#include <driver/i2s_std.h>
#include <es8388_codec.h>
#include <esp_codec_dev_defaults.h>
#include <esp_log.h>

namespace meet {
namespace {

constexpr char TAG[] = "es8388";
constexpr int kDmaDescNum = 6;
constexpr int kDmaFrameNum = 240;

}  // namespace

Es8388Codec& Es8388Codec::Instance() {
    static Es8388Codec codec;
    return codec;
}

esp_err_t Es8388Codec::Init(i2c_master_bus_handle_t i2c) {
    if (ready_) {
        return ESP_OK;
    }
    if (!i2c) {
        return ESP_ERR_INVALID_ARG;
    }

    input_reference_ = AUDIO_INPUT_REFERENCE;
    input_channels_ = input_reference_ ? 2 : 1;
    input_rate_ = AUDIO_INPUT_SAMPLE_RATE;
    output_rate_ = AUDIO_OUTPUT_SAMPLE_RATE;

    i2s_chan_config_t chan_cfg = {};
    chan_cfg.id = I2S_NUM_0;
    chan_cfg.role = I2S_ROLE_MASTER;
    chan_cfg.dma_desc_num = kDmaDescNum;
    chan_cfg.dma_frame_num = kDmaFrameNum;
    chan_cfg.auto_clear_after_cb = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg.sample_rate_hz = static_cast<uint32_t>(output_rate_);
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    std_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_16BIT;
    std_cfg.slot_cfg.ws_pol = false;
    std_cfg.slot_cfg.bit_shift = true;
    std_cfg.slot_cfg.left_align = true;
    std_cfg.gpio_cfg.mclk = AUDIO_I2S_GPIO_MCLK;
    std_cfg.gpio_cfg.bclk = AUDIO_I2S_GPIO_BCLK;
    std_cfg.gpio_cfg.ws = AUDIO_I2S_GPIO_WS;
    std_cfg.gpio_cfg.dout = AUDIO_I2S_GPIO_DOUT;
    std_cfg.gpio_cfg.din = AUDIO_I2S_GPIO_DIN;

    err = i2s_channel_init_std_mode(tx_handle_, &std_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_init_std_mode(rx_handle_, &std_cfg);
    }
    if (err == ESP_OK) {
        err = i2s_channel_enable(tx_handle_);
    }
    if (err == ESP_OK) {
        err = i2s_channel_enable(rx_handle_);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s std: %s", esp_err_to_name(err));
        return err;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {};
    i2s_cfg.port = I2S_NUM_0;
    i2s_cfg.rx_handle = rx_handle_;
    i2s_cfg.tx_handle = tx_handle_;
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = {};
    i2c_cfg.port = I2C_NUM_0;
    i2c_cfg.addr = AUDIO_CODEC_ES8388_ADDR;
    i2c_cfg.bus_handle = i2c;
    ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    gpio_if_ = audio_codec_new_gpio();
    if (!data_if_ || !ctrl_if_ || !gpio_if_) {
        ESP_LOGE(TAG, "codec if create failed");
        return ESP_FAIL;
    }

    es8388_codec_cfg_t es8388_cfg = {};
    es8388_cfg.ctrl_if = static_cast<const audio_codec_ctrl_if_t*>(ctrl_if_);
    es8388_cfg.gpio_if = static_cast<const audio_codec_gpio_if_t*>(gpio_if_);
    es8388_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    es8388_cfg.master_mode = true;
    es8388_cfg.pa_pin = AUDIO_CODEC_PA_PIN;
    es8388_cfg.hw_gain.pa_voltage = 5.0;
    es8388_cfg.hw_gain.codec_dac_voltage = 3.3;
    codec_if_ = es8388_codec_new(&es8388_cfg);
    if (!codec_if_) {
        ESP_LOGE(TAG, "es8388_codec_new failed");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t out_cfg = {};
    out_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    out_cfg.codec_if = static_cast<const audio_codec_if_t*>(codec_if_);
    out_cfg.data_if = static_cast<const audio_codec_data_if_t*>(data_if_);
    output_dev_ = esp_codec_dev_new(&out_cfg);
    esp_codec_dev_cfg_t in_cfg = {};
    in_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    in_cfg.codec_if = static_cast<const audio_codec_if_t*>(codec_if_);
    in_cfg.data_if = static_cast<const audio_codec_data_if_t*>(data_if_);
    input_dev_ = esp_codec_dev_new(&in_cfg);
    if (!output_dev_ || !input_dev_) {
        ESP_LOGE(TAG, "esp_codec_dev_new failed");
        return ESP_FAIL;
    }
    esp_codec_set_disable_when_closed(output_dev_, false);
    esp_codec_set_disable_when_closed(input_dev_, false);
    ready_ = true;
    ESP_LOGI(TAG, "ready @ %d Hz, ch=%d", input_rate_, input_channels_);
    return ESP_OK;
}

void Es8388Codec::EnableInput(bool enable) {
    if (!ready_ || enable == input_on_) {
        return;
    }
    if (enable) {
        esp_codec_dev_sample_info_t fs = {};
        fs.bits_per_sample = 16;
        fs.channel = static_cast<uint8_t>(input_channels_);
        fs.channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0);
        fs.sample_rate = static_cast<uint32_t>(input_rate_);
        if (input_reference_) {
            fs.channel_mask |= ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
        }
        if (esp_codec_dev_open(input_dev_, &fs) != ESP_OK) {
            ESP_LOGW(TAG, "input open failed");
            return;
        }
        auto* ctrl = static_cast<const audio_codec_ctrl_if_t*>(ctrl_if_);
        if (input_reference_ && ctrl && ctrl->write_reg) {
            uint8_t gain = (11 << 4);
            ctrl->write_reg(ctrl, 0x09, 1, &gain, 1);
        } else {
            esp_codec_dev_set_in_gain(input_dev_, 24);
        }
    } else {
        esp_codec_dev_close(input_dev_);
    }
    input_on_ = enable;
}

void Es8388Codec::EnableOutput(bool enable) {
    if (!ready_ || enable == output_on_) {
        return;
    }
    if (enable) {
        esp_codec_dev_sample_info_t fs = {};
        fs.bits_per_sample = 16;
        fs.channel = 1;
        fs.sample_rate = static_cast<uint32_t>(output_rate_);
        if (esp_codec_dev_open(output_dev_, &fs) != ESP_OK) {
            ESP_LOGW(TAG, "output open failed");
            return;
        }
        esp_codec_dev_set_out_vol(output_dev_, volume_);
        auto* ctrl = static_cast<const audio_codec_ctrl_if_t*>(ctrl_if_);
        if (ctrl && ctrl->write_reg) {
            uint8_t reg_val = 30;
            const uint8_t regs[] = {46, 47, 48, 49};
            for (uint8_t reg : regs) {
                ctrl->write_reg(ctrl, reg, 1, &reg_val, 1);
            }
        }
    } else {
        esp_codec_dev_close(output_dev_);
    }
    output_on_ = enable;
}

void Es8388Codec::SetOutputVolume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    volume_ = volume;
    if (output_on_ && output_dev_) {
        esp_codec_dev_set_out_vol(output_dev_, volume_);
    }
}

int Es8388Codec::Read(int16_t* dest, int samples) {
    if (!input_on_ || !dest || samples <= 0) {
        return 0;
    }
    const esp_err_t err =
        esp_codec_dev_read(input_dev_, dest, samples * static_cast<int>(sizeof(int16_t)));
    if (err != ESP_OK) {
        return 0;
    }
    return samples;
}

int Es8388Codec::Write(const int16_t* data, int samples) {
    if (!output_on_ || !data || samples <= 0) {
        return 0;
    }
    const esp_err_t err = esp_codec_dev_write(output_dev_, const_cast<int16_t*>(data),
                                              samples * static_cast<int>(sizeof(int16_t)));
    if (err != ESP_OK) {
        return 0;
    }
    return samples;
}

}  // namespace meet
