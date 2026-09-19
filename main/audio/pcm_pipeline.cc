#include "pcm_pipeline.h"

#include "config.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include <cmath>
#include <cstring>

namespace meet {
namespace {

constexpr char TAG[] = "pcm";

}  // namespace

PcmPipeline& PcmPipeline::Instance() {
    static PcmPipeline pipe;
    return pipe;
}

esp_err_t PcmPipeline::Init() {
#if CONFIG_MEET_USE_DEVICE_AEC
    aec_enabled_ = true;
    ESP_LOGI(TAG, "Device AEC hook enabled (esp-sr AFE TODO)");
#else
    aec_enabled_ = false;
#endif
    // ES8388 / esp_codec_dev bring-up deferred; capture emits silence/tone frames.
    ESP_LOGW(TAG, "ES8388 codec init deferred; using silence capture stub @ %d Hz", kPcmCaptureHz);
    return ESP_OK;
}

void PcmPipeline::StartCapture() {
    if (capture_running_) {
        return;
    }
    capture_running_ = true;
    xTaskCreate(CaptureTask, "pcm_cap", 4096, this, 6, nullptr);
}

void PcmPipeline::StopCapture() {
    capture_running_ = false;
    std::lock_guard<std::mutex> lock(mu_);
    capture_q_.clear();
}

void PcmPipeline::CaptureTask(void* arg) {
    auto* self = static_cast<PcmPipeline*>(arg);
    uint32_t phase = 0;
    while (self->capture_running_) {
        PcmFrame frame;
        frame.samples.resize(kPcmFrameSamples);
        // Soft near-silence stub (tiny tone keeps path alive for debug).
        for (int i = 0; i < kPcmFrameSamples; ++i) {
            frame.samples[i] = static_cast<int16_t>(80 * sinf(phase * 0.02f));
            ++phase;
        }
        if (self->aec_enabled_) {
            // Hook: run AFE AEC against playback reference when wired.
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

    // Provider often 24 kHz; capture path is 16 kHz — resample for local play later.
    if (AUDIO_OUTPUT_SAMPLE_RATE == 24000 && kPcmCaptureHz == 16000) {
        // Keep provider rate in queue; playback task will drive codec @ 24k.
    }

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
    // 3:2 decimation with linear interpolation
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
