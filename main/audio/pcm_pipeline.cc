#include "pcm_pipeline.h"

#include "afe_processor.h"
#include "board.h"
#include "config.h"
#include "meet_es8388.h"
#include "wake_word.h"

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

    const int channels = codec_ready_ ? Es8388Codec::Instance().input_channels() : 2;
    const bool has_ref = AUDIO_INPUT_REFERENCE;
    if (AfeProcessor::Instance().Init(channels, has_ref) == ESP_OK) {
        AfeProcessor::Instance().SetOutputHandler(
            [this](const int16_t* data, size_t samples) { OnAfeOutput(data, samples); });
    } else {
        ESP_LOGW(TAG, "AFE VC unavailable; InCall uses mic channel 0");
    }
    if (WakeWord::Instance().Init(channels, has_ref) != ESP_OK) {
        ESP_LOGW(TAG, "wake AFE unavailable; Boot remains the call trigger");
    }
    return ESP_OK;
}

void PcmPipeline::StopTasks() {
    capture_running_ = false;
    playback_running_ = false;
    WakeWord::Instance().Stop();
    AfeProcessor::Instance().Stop();
    {
        std::lock_guard<std::mutex> lock(mu_);
        capture_q_.clear();
        playback_q_.clear();
        afe_acc_.clear();
    }
    if (codec_ready_) {
        Es8388Codec::Instance().EnableInput(false);
        Es8388Codec::Instance().EnableOutput(false);
    }
    vTaskDelay(pdMS_TO_TICKS(40));
    mode_ = PcmMode::Idle;
}

void PcmPipeline::EnsureCaptureTask() {
    if (capture_running_) {
        return;
    }
    capture_running_ = true;
    xTaskCreate(CaptureTask, "pcm_cap", 4096, this, 6, nullptr);
}

void PcmPipeline::StartListen() {
    if (mode_ == PcmMode::Listen) {
        if (WakeWord::Instance().ready() && !WakeWord::Instance().running()) {
            WakeWord::Instance().Start();
        }
        return;
    }
    StopTasks();
    mode_ = PcmMode::Listen;
    if (codec_ready_) {
        Es8388Codec::Instance().EnableInput(true);
    }
    if (WakeWord::Instance().ready()) {
        WakeWord::Instance().Start();
    }
    EnsureCaptureTask();
    ESP_LOGI(TAG, "listen (wake) started");
}

void PcmPipeline::StartCapture() {
    if (mode_ == PcmMode::Call) {
        return;
    }
    StopTasks();
    mode_ = PcmMode::Call;
    if (codec_ready_) {
        Es8388Codec::Instance().EnableInput(true);
        Es8388Codec::Instance().EnableOutput(true);
        playback_running_ = true;
        xTaskCreate(PlaybackTask, "pcm_play", 4096, this, 6, nullptr);
    }
    if (aec_enabled_ && AfeProcessor::Instance().ready()) {
        AfeProcessor::Instance().Start();
    }
    EnsureCaptureTask();
    ESP_LOGI(TAG, "call capture started aec=%d",
             (aec_enabled_ && AfeProcessor::Instance().ready()) ? 1 : 0);
}

void PcmPipeline::StopCapture() {
    StopTasks();
}

void PcmPipeline::PushUplink(const int16_t* data, size_t samples) {
    if (!data || samples == 0) {
        return;
    }
    PcmFrame frame;
    frame.samples.assign(data, data + samples);
    if (frame.samples.size() != static_cast<size_t>(kPcmFrameSamples)) {
        frame.samples.resize(kPcmFrameSamples, 0);
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (capture_q_.size() > 10) {
        capture_q_.pop_front();
    }
    capture_q_.push_back(std::move(frame));
}

void PcmPipeline::OnAfeOutput(const int16_t* data, size_t samples) {
    if (!data || samples == 0 || mode_ != PcmMode::Call) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    afe_acc_.insert(afe_acc_.end(), data, data + samples);
    while (afe_acc_.size() >= static_cast<size_t>(kPcmFrameSamples)) {
        PcmFrame frame;
        frame.samples.assign(afe_acc_.begin(), afe_acc_.begin() + kPcmFrameSamples);
        afe_acc_.erase(afe_acc_.begin(), afe_acc_.begin() + kPcmFrameSamples);
        if (capture_q_.size() > 10) {
            capture_q_.pop_front();
        }
        capture_q_.push_back(std::move(frame));
    }
}

void PcmPipeline::CaptureTask(void* arg) {
    auto* self = static_cast<PcmPipeline*>(arg);
    auto& codec = Es8388Codec::Instance();
    std::vector<int16_t> raw;
    uint32_t phase = 0;
    while (self->capture_running_) {
        if (self->codec_ready_) {
            const int channels = codec.input_channels();
            raw.resize(static_cast<size_t>(kCodecFrameSamples * channels));
            codec.Read(raw.data(), static_cast<int>(raw.size()));
            const auto interleaved16k =
                Resample24kTo16kInterleaved(raw.data(), raw.size(), channels);

            if (self->mode_ == PcmMode::Listen) {
                WakeWord::Instance().FeedInterleaved16k(interleaved16k.data(), interleaved16k.size());
            } else if (self->mode_ == PcmMode::Call) {
                if (self->aec_enabled_ && AfeProcessor::Instance().ready() &&
                    AfeProcessor::Instance().running()) {
                    AfeProcessor::Instance().FeedInterleaved16k(interleaved16k.data(),
                                                                interleaved16k.size());
                } else {
                    const size_t frames = interleaved16k.size() / static_cast<size_t>(channels);
                    std::vector<int16_t> mono(frames);
                    for (size_t i = 0; i < frames; ++i) {
                        mono[i] = interleaved16k[i * static_cast<size_t>(channels)];
                    }
                    if (mono.size() != static_cast<size_t>(kPcmFrameSamples)) {
                        mono.resize(kPcmFrameSamples, 0);
                    }
                    self->PushUplink(mono.data(), mono.size());
                }
            }
        } else if (self->mode_ == PcmMode::Call) {
            PcmFrame frame;
            frame.samples.resize(kPcmFrameSamples);
            for (int i = 0; i < kPcmFrameSamples; ++i) {
                frame.samples[i] = static_cast<int16_t>(80 * sinf(phase * 0.02f));
                ++phase;
            }
            self->PushUplink(frame.samples.data(), frame.samples.size());
            vTaskDelay(pdMS_TO_TICKS(kPcmFrameMs));
        } else {
            vTaskDelay(pdMS_TO_TICKS(kPcmFrameMs));
        }
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

std::vector<int16_t> PcmPipeline::Resample24kTo16kInterleaved(const int16_t* in,
                                                             size_t in_count,
                                                             int channels) {
    if (!in || in_count == 0 || channels <= 0) {
        return {};
    }
    if (channels == 1) {
        return Resample24kTo16k(in, in_count);
    }
    const size_t frames = in_count / static_cast<size_t>(channels);
    std::vector<int16_t> out;
    const size_t out_frames = (frames * 2) / 3;
    out.resize(out_frames * static_cast<size_t>(channels), 0);
    size_t o = 0;
    for (size_t i = 0; i + 2 < frames; i += 3) {
        for (int ch = 0; ch < channels; ++ch) {
            out[o * static_cast<size_t>(channels) + static_cast<size_t>(ch)] =
                in[i * static_cast<size_t>(channels) + static_cast<size_t>(ch)];
        }
        ++o;
        if (o >= out_frames) {
            break;
        }
        for (int ch = 0; ch < channels; ++ch) {
            const int32_t a = in[(i + 1) * static_cast<size_t>(channels) + static_cast<size_t>(ch)];
            const int32_t b = in[(i + 2) * static_cast<size_t>(channels) + static_cast<size_t>(ch)];
            out[o * static_cast<size_t>(channels) + static_cast<size_t>(ch)] =
                static_cast<int16_t>((a + b) / 2);
        }
        ++o;
        if (o >= out_frames) {
            break;
        }
    }
    out.resize(o * static_cast<size_t>(channels));
    return out;
}

void PcmPipeline::SetDeviceAecEnabled(bool enabled) {
    aec_enabled_ = enabled;
}

}  // namespace meet
