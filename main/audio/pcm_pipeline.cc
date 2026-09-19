#include "pcm_pipeline.h"

#include "board.h"
#include "config.h"
#include "meet_es8388.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace meet {
namespace {

constexpr char TAG[] = "pcm";
constexpr int kCodecFrameSamples = (AUDIO_INPUT_SAMPLE_RATE * kPcmFrameMs) / 1000;  // 480 @ 24k

}  // namespace

PcmPipeline& PcmPipeline::Instance() {
    static PcmPipeline pipe;
    return pipe;
}

esp_err_t PcmPipeline::Init() {
#if CONFIG_MEET_USE_DEVICE_AEC
    aec_enabled_ = true;
#else
    aec_enabled_ = false;
#endif
    const esp_err_t err = Es8388Codec::Instance().Init(Board::Instance().i2c_bus());
    codec_ready_ = (err == ESP_OK);
    if (!codec_ready_) {
        ESP_LOGW(TAG, "ES8388 init failed (%s); capture falls back to tone", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "ES8388 capture/playback ready");
    }
    return ESP_OK;
}

void PcmPipeline::StartCapture() {
    if (capture_running_) {
        return;
    }
    capture_running_ = true;
    if (codec_ready_) {
        Es8388Codec::Instance().EnableInput(true);
        Es8388Codec::Instance().EnableOutput(true);
        playback_running_ = true;
        xTaskCreate(PlaybackTask, "pcm_play", 4096, this, 6, nullptr);
    }
    xTaskCreate(CaptureTask, "pcm_cap", 4096, this, 6, nullptr);
}

void PcmPipeline::StopCapture() {
    capture_running_ = false;
    playback_running_ = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        capture_q_.clear();
        playback_q_.clear();
    }
    if (codec_ready_) {
        Es8388Codec::Instance().EnableInput(false);
        Es8388Codec::Instance().EnableOutput(false);
    }
}

void PcmPipeline::CaptureTask(void* arg) {
    auto* self = static_cast<PcmPipeline*>(arg);
    auto& codec = Es8388Codec::Instance();
    std::vector<int16_t> raw;
    uint32_t phase = 0;
    while (self->capture_running_) {
        PcmFrame frame;
        if (self->codec_ready_) {
            const int channels = codec.input_channels();
            raw.resize(static_cast<size_t>(kCodecFrameSamples * channels));
            codec.Read(raw.data(), static_cast<int>(raw.size()));
            std::vector<int16_t> mono(kCodecFrameSamples);
            for (int i = 0; i < kCodecFrameSamples; ++i) {
                mono[i] = raw[static_cast<size_t>(i * channels)];
            }
            frame.samples = Resample24kTo16k(mono.data(), mono.size());
            if (frame.samples.size() != static_cast<size_t>(kPcmFrameSamples) && !mono.empty()) {
                frame.samples.resize(kPcmFrameSamples, 0);
                const size_t n = std::min(mono.size(), frame.samples.size());
                for (size_t i = 0; i < n; ++i) {
                    const size_t src = (i * mono.size()) / n;
                    frame.samples[i] = mono[src];
                }
            }
        } else {
            frame.samples.resize(kPcmFrameSamples);
            for (int i = 0; i < kPcmFrameSamples; ++i) {
                frame.samples[i] = static_cast<int16_t>(80 * sinf(phase * 0.02f));
                ++phase;
            }
        }
        {
            std::lock_guard<std::mutex> lock(self->mu_);
            if (self->capture_q_.size() > 10) {
                self->capture_q_.pop_front();
            }
            self->capture_q_.push_back(std::move(frame));
        }
        vTaskDelay(pdMS_TO_TICKS(kPcmFrameMs));
    }
    vTaskDelete(nullptr);
}

void PcmPipeline::PlaybackTask(void* arg) {
    auto* self = static_cast<PcmPipeline*>(arg);
    auto& codec = Es8388Codec::Instance();
    std::vector<int16_t> silence(kCodecFrameSamples, 0);
    while (self->playback_running_) {
        TaggedPcmChunk chunk;
        bool have = false;
        {
            std::lock_guard<std::mutex> lock(self->mu_);
            if (!self->playback_q_.empty()) {
                chunk = std::move(self->playback_q_.front());
                self->playback_q_.pop_front();
                have = true;
            }
        }
        if (have && chunk.generation == self->generation_ && !chunk.samples.empty()) {
            codec.Write(chunk.samples.data(), static_cast<int>(chunk.samples.size()));
        } else {
            codec.Write(silence.data(), static_cast<int>(silence.size()));
            vTaskDelay(pdMS_TO_TICKS(kPcmFrameMs));
        }
    }
    vTaskDelete(nullptr);
}

bool PcmPipeline::PopCaptureFrame(PcmFrame& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (capture_q_.empty()) {
        return false;
    }
    out = std::move(capture_q_.front());
    capture_q_.pop_front();
    return true;
}

void PcmPipeline::EnqueuePlayback(const std::string& response_id,
                                  const uint8_t* pcm_bytes,
                                  size_t len) {
    if (!pcm_bytes || len < 2) {
        return;
    }
    TaggedPcmChunk chunk;
    chunk.response_id = response_id;
    chunk.generation = generation_;
    const size_t samples = len / sizeof(int16_t);
    chunk.samples.resize(samples);
    memcpy(chunk.samples.data(), pcm_bytes, samples * sizeof(int16_t));

    std::lock_guard<std::mutex> lock(mu_);
    if (playback_q_.size() > 64) {
        playback_q_.pop_front();
    }
    playback_q_.push_back(std::move(chunk));
}

void PcmPipeline::ClearGeneration() {
    std::lock_guard<std::mutex> lock(mu_);
    ++generation_;
    playback_q_.clear();
    ESP_LOGI(TAG, "ClearGeneration -> %u", static_cast<unsigned>(generation_));
}

std::vector<int16_t> PcmPipeline::Resample24kTo16k(const int16_t* in, size_t in_count) {
    std::vector<int16_t> out;
    if (!in || in_count == 0) {
        return out;
    }
    out.reserve((in_count * 2) / 3 + 1);
    for (size_t i = 0; i + 2 < in_count; i += 3) {
        out.push_back(in[i]);
        const int32_t mid = (static_cast<int32_t>(in[i + 1]) + in[i + 2]) / 2;
        out.push_back(static_cast<int16_t>(mid));
    }
    return out;
}

std::vector<int16_t> PcmPipeline::Resample16kTo24k(const int16_t* in, size_t in_count) {
    std::vector<int16_t> out;
    if (!in || in_count == 0) {
        return out;
    }
    out.reserve((in_count * 3) / 2 + 1);
    for (size_t i = 0; i + 1 < in_count; i += 2) {
        out.push_back(in[i]);
        const int32_t mid = (static_cast<int32_t>(in[i]) + in[i + 1]) / 2;
        out.push_back(static_cast<int16_t>(mid));
        out.push_back(in[i + 1]);
    }
    if (in_count % 2) {
        out.push_back(in[in_count - 1]);
    }
    return out;
}

void PcmPipeline::SetDeviceAecEnabled(bool enabled) {
    aec_enabled_ = enabled;
}

}  // namespace meet
