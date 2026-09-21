#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <esp_err.h>

#include "resampler.h"

namespace meet {

constexpr int kPcmCaptureHz = 16000;
constexpr int kPcmFrameMs = 20;
constexpr int kPcmFrameSamples = (kPcmCaptureHz * kPcmFrameMs) / 1000;  // 320
constexpr int kDownlinkProviderHz = 24000;

struct TaggedPcmChunk {
    std::string response_id;
    uint32_t generation = 0;
    std::vector<int16_t> samples;  // device output rate (16 kHz)
};

enum class PcmMode {
    Idle,
    Listen,  // Ready: feed wake AFE only
    Call,    // InCall: feed VC AFE / uplink
};

class AudioPipeline {
public:
    static AudioPipeline& Instance();

    esp_err_t Init();

    void StartListen();
    void StartCapture();
    void StopCapture();
    PcmMode mode() const { return mode_.load(); }

    /** Downlink: tag with response_id + generation; ClearGeneration on barge-in. */
    void EnqueuePlayback(const std::string& response_id, const uint8_t* pcm_bytes, size_t len);
    void EnqueuePlaybackSamples(const int16_t* samples, size_t count, uint32_t generation = 0);
    void ClearGeneration();
    uint32_t generation() const { return generation_.load(); }

    void SetDeviceAecEnabled(bool enabled);
    bool device_aec_enabled() const { return aec_enabled_; }

    /** Diagnostic: uplink frames sent since last call to TakeUplinkFrameCount(). */
    uint32_t TakeUplinkFrameCount();

private:
    AudioPipeline() = default;

    void ApplyMode(PcmMode next);
    void EnsureVcAfe();
    void EnsureWakeAfe();
    void OnAfeOutput(const int16_t* data, size_t samples);
    void PushUplinkFrame(const int16_t* mono, size_t samples);

    static void CaptureTask(void* arg);
    static void PlaybackTask(void* arg);

    bool codec_ready_ = false;
    bool aec_enabled_ = false;
    std::atomic<PcmMode> mode_{PcmMode::Idle};
    std::atomic<uint32_t> generation_{1};
    std::atomic<uint32_t> uplink_frames_{0};

    std::mutex play_mu_;
    std::deque<TaggedPcmChunk> playback_q_;

    // Fixed capture buffers (no per-frame heap)
    int16_t raw_buf_[640] = {};       // 320 frames * 2 ch @ 16k / 20ms
    int16_t mono_buf_[kPcmFrameSamples] = {};
    int16_t afe_acc_[kPcmFrameSamples * 2] = {};
    size_t afe_acc_len_ = 0;

    Resampler24kTo16k downlink_resampler_;
    int16_t downsample_buf_[960] = {};  // enough for a large delta chunk slice
};

}  // namespace meet
