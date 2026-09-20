#pragma once

#include <driver/i2c_master.h>
#include <driver/i2s_std.h>
#include <esp_codec_dev.h>
#include <esp_err.h>
#include <cstdint>

namespace meet {

class Es8388Codec {
public:
    static Es8388Codec& Instance();

    esp_err_t Init(i2c_master_bus_handle_t i2c);
    bool ready() const { return ready_; }

    void EnableInput(bool enable);
    void EnableOutput(bool enable);
    void SetOutputVolume(int volume);
    int output_volume() const { return volume_; }

    int input_channels() const { return input_channels_; }
    int input_rate() const { return input_rate_; }
    int output_rate() const { return output_rate_; }

    int Read(int16_t* dest, int samples);
    int Write(const int16_t* data, int samples);

private:
    Es8388Codec() = default;

    bool ready_ = false;
    bool input_on_ = false;
    bool output_on_ = false;
    bool input_reference_ = true;
    int input_channels_ = 2;
    int input_rate_ = 16000;
    int output_rate_ = 16000;
    int volume_ = 70;

    const void* data_if_ = nullptr;
    const void* ctrl_if_ = nullptr;
    const void* codec_if_ = nullptr;
    const void* gpio_if_ = nullptr;
    esp_codec_dev_handle_t output_dev_ = nullptr;
    esp_codec_dev_handle_t input_dev_ = nullptr;
    i2s_chan_handle_t tx_handle_ = nullptr;
    i2s_chan_handle_t rx_handle_ = nullptr;
};

}  // namespace meet
