#include "audio_pipeline.h"

#include "afe_unit.h"
#include "app_event.h"
#include "board.h"
#include "config.h"
#include "meet_es8388.h"
#include "meet_realtime.h"
#include "resampler.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include <algorithm>
#include <cstring>

namespace meet {
namespace {

constexpr char TAG[] = "audio";
constexpr int kCodecFrameSamples = (AUDIO_INPUT_SAMPLE_RATE * kPcmFrameMs) / 1000;

AfeUnit g_afe_vc;
AfeUnit g_afe_wake;

}  // namespace

AudioPipeline& AudioPipeline::Instance() {
    static AudioPipeline pipe;
    return pipe;
}

esp_err_t AudioPipeline::Init() {
#if CONFIG_MEET_USE_DEVICE_AEC
    aec_enabled_ = true;
#else
    aec_enabled_ = false;
#endif
    downlink_resampler_.Reset();

    const esp_err_t err = Es8388Codec::Instance().Init(Board::Instance().i2c_bus());
    codec_ready_ = (err == ESP_OK);
    if (!codec_ready_) {
        ESP_LOGW(TAG, "ES8388 init failed (%s)", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "ES8388 ready @ %d Hz", AUDIO_INPUT_SAMPLE_RATE);
    }

    const int channels = codec_ready_ ? Es8388Codec::Instance().input_channels() : 2;
    const bool has_ref = AUDIO_INPUT_REFERENCE;

    if (g_afe_vc.Init(AfeUnitType::VoiceComm, channels, has_ref, aec_enabled_, "afe_vc", 6144,
                      5) == ESP_OK) {
        g_afe_vc.SetOutputHandler(
            [this](const int16_t* data, size_t samples) { OnAfeOutput(data, samples); });
    } else {
        ESP_LOGW(TAG, "AFE VC unavailable; InCall uses mic channel 0");
    }

    if (g_afe_wake.Init(AfeUnitType::WakeWord, channels, has_ref, false, "afe_sr", 4096, 3) ==
        ESP_OK) {
        g_afe_wake.SetWakeHandler([]() {
            AppEvent ev;
            ev.type = AppEventType::WakeDetected;
            AppEventPost(ev);
        });
    } else {
        ESP_LOGW(TAG, "wake AFE unavailable; Boot remains the call trigger");
    }

    // Resident tasks — never destroyed.
    xTaskCreate(CaptureTask, "pcm_cap", 4096, this, 6, nullptr);
    xTaskCreate(PlaybackTask, "pcm_play", 4096, this, 6, nullptr);

    if (codec_ready_) {
        Es8388Codec::Instance().EnableOutput(true);
    }
    return ESP_OK;
}

void AudioPipeline::ApplyMode(PcmMode next) {
    const PcmMode prev = mode_.exchange(next);
    if (prev == next) {
        return;
    }

    g_afe_wake.Stop();
    g_afe_vc.Stop();
    afe_acc_len_ = 0;

    if (!codec_ready_) {
        return;
    }

    if (next == PcmMode::Idle) {
        Es8388Codec::Instance().EnableInput(false);
        return;
    }

    Es8388Codec::Instance().EnableInput(true);
    Es8388Codec::Instance().EnableOutput(true);

    if (next == PcmMode::Listen) {
        if (g_afe_wake.ready()) {
            g_afe_wake.Start();
        }
        ESP_LOGI(TAG, "mode=Listen");
    } else if (next == PcmMode::Call) {
        if (aec_enabled_ && g_afe_vc.ready()) {
            g_afe_vc.Start();
        }
        ESP_LOGI(TAG, "mode=Call aec=%d", (aec_enabled_ && g_afe_vc.ready()) ? 1 : 0);
    }
}

void AudioPipeline::StartListen() {
    ApplyMode(PcmMode::Listen);
}

void AudioPipeline::StartCapture() {
    ApplyMode(PcmMode::Call);
}

void AudioPipeline::StopCapture() {
    ApplyMode(PcmMode::Idle);
}

void AudioPipeline::PushUplinkFrame(const int16_t* mono, size_t samples) {
    if (!mono || samples == 0 || mode_.load() != PcmMode::Call) {
        return;
    }
    int16_t frame[kPcmFrameSamples];
    if (samples >= static_cast<size_t>(kPcmFrameSamples)) {
        std::memcpy(frame, mono, sizeof(frame));
    } else {
        std::memcpy(frame, mono, samples * sizeof(int16_t));
        std::memset(frame + samples, 0, (kPcmFrameSamples - samples) * sizeof(int16_t));
    }
    if (MeetRealtime::Instance().SendInputPcm(frame, kPcmFrameSamples) == ESP_OK) {
        uplink_frames_.fetch_add(1);
    }
}

void AudioPipeline::OnAfeOutput(const int16_t* data, size_t samples) {
    if (!data || samples == 0 || mode_.load() != PcmMode::Call) {
        return;
    }
    size_t off = 0;
    while (off < samples) {
        const size_t space = kPcmFrameSamples - afe_acc_len_;
        const size_t n = std::min(space, samples - off);
        std::memcpy(afe_acc_ + afe_acc_len_, data + off, n * sizeof(int16_t));
        afe_acc_len_ += n;
        off += n;
        if (afe_acc_len_ >= static_cast<size_t>(kPcmFrameSamples)) {
            PushUplinkFrame(afe_acc_, kPcmFrameSamples);
            afe_acc_len_ = 0;
        }
    }
}

void AudioPipeline::CaptureTask(void* arg) {
    auto* self = static_cast<AudioPipeline*>(arg);
    auto& codec = Es8388Codec::Instance();
    while (true) {
        const PcmMode mode = self->mode_.load();
        if (mode == PcmMode::Idle || !self->codec_ready_) {
            vTaskDelay(pdMS_TO_TICKS(kPcmFrameMs));
            continue;
        }

        const int channels = codec.input_channels();
        const int need = kCodecFrameSamples * channels;
        if (need <= 0 || need > static_cast<int>(sizeof(self->raw_buf_) / sizeof(int16_t))) {
            vTaskDelay(pdMS_TO_TICKS(kPcmFrameMs));
            continue;
        }
        const int got = codec.Read(self->raw_buf_, need);
        if (got <= 0) {
            vTaskDelay(pdMS_TO_TICKS(kPcmFrameMs));
            continue;
        }

        // Native 16 kHz path: extract mono or feed interleaved to AFE.
        if (mode == PcmMode::Listen) {
            if (g_afe_wake.ready() && g_afe_wake.running()) {
                g_afe_wake.FeedInterleaved16k(self->raw_buf_,
                                              static_cast<size_t>(kCodecFrameSamples * channels));
            }
        } else if (mode == PcmMode::Call) {
            if (self->aec_enabled_ && g_afe_vc.ready() && g_afe_vc.running()) {
                g_afe_vc.FeedInterleaved16k(self->raw_buf_,
                                            static_cast<size_t>(kCodecFrameSamples * channels));
            } else {
                for (int i = 0; i < kCodecFrameSamples && i < kPcmFrameSamples; ++i) {
                    self->mono_buf_[i] = self->raw_buf_[i * channels];
                }
                self->PushUplinkFrame(self->mono_buf_, kPcmFrameSamples);
            }
        }
    }
}

void AudioPipeline::PlaybackTask(void* arg) {
    auto* self = static_cast<AudioPipeline*>(arg);
    auto& codec = Es8388Codec::Instance();
    std::vector<int16_t> silence(kCodecFrameSamples, 0);
    while (true) {
        TaggedPcmChunk chunk;
        bool have = false;
        {
            std::lock_guard<std::mutex> lock(self->play_mu_);
            if (!self->playback_q_.empty()) {
                chunk = std::move(self->playback_q_.front());
                self->playback_q_.pop_front();
                have = true;
            }
        }
        if (have && chunk.generation == self->generation_.load() && !chunk.samples.empty()) {
            if (self->codec_ready_) {
                codec.Write(chunk.samples.data(), static_cast<int>(chunk.samples.size()));
            }
        } else {
            if (self->codec_ready_) {
                codec.Write(silence.data(), static_cast<int>(silence.size()));
            }
            vTaskDelay(pdMS_TO_TICKS(kPcmFrameMs));
        }
    }
}

void AudioPipeline::EnqueuePlayback(const std::string& response_id,
                                    const uint8_t* pcm_bytes,
                                    size_t len) {
    if (!pcm_bytes || len < 2) {
        return;
    }
    const size_t in_samples = len / sizeof(int16_t);
    const int16_t* in = reinterpret_cast<const int16_t*>(pcm_bytes);

    // Provider downlink is 24 kHz; device plays at 16 kHz.
    size_t produced = 0;
    size_t offset = 0;
    TaggedPcmChunk chunk;
    chunk.response_id = response_id;
    chunk.generation = generation_.load();
    chunk.samples.reserve((in_samples * 2) / 3 + 8);

    while (offset < in_samples) {
        const size_t slice = std::min(in_samples - offset, static_cast<size_t>(480));
        produced = downlink_resampler_.Process(in + offset, slice, downsample_buf_,
                                               sizeof(downsample_buf_) / sizeof(int16_t));
        chunk.samples.insert(chunk.samples.end(), downsample_buf_, downsample_buf_ + produced);
        offset += slice;
    }
    if (chunk.samples.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(play_mu_);
    if (playback_q_.size() > 64) {
        playback_q_.pop_front();
    }
    playback_q_.push_back(std::move(chunk));
}

void AudioPipeline::EnqueuePlaybackSamples(const int16_t* samples, size_t count,
                                           uint32_t generation) {
    if (!samples || count == 0) {
        return;
    }
    TaggedPcmChunk chunk;
    chunk.generation = generation ? generation : generation_.load();
    chunk.samples.assign(samples, samples + count);
    std::lock_guard<std::mutex> lock(play_mu_);
    if (playback_q_.size() > 64) {
        playback_q_.pop_front();
    }
    playback_q_.push_back(std::move(chunk));
}

void AudioPipeline::ClearGeneration() {
    std::lock_guard<std::mutex> lock(play_mu_);
    generation_.fetch_add(1);
    playback_q_.clear();
    downlink_resampler_.Reset();
    ESP_LOGI(TAG, "ClearGeneration -> %u", static_cast<unsigned>(generation_.load()));
}

void AudioPipeline::SetDeviceAecEnabled(bool enabled) {
    aec_enabled_ = enabled;
}

uint32_t AudioPipeline::TakeUplinkFrameCount() {
    return uplink_frames_.exchange(0);
}

}  // namespace meet
