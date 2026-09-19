#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <esp_err.h>

namespace meet {

constexpr int kPcmCaptureHz = 16000;
constexpr int kPcmFrameMs = 20;
constexpr int kPcmFrameSamples = (kPcmCaptureHz * kPcmFrameMs) / 1000;  // 320

struct PcmFrame {
    std::vector<int16_t> samples;
};

struct TaggedPcmChunk {
    std::string response_id;
    uint32_t generation = 0;
    std::vector<int16_t> samples;
};

class PcmPipeline {
public:
    static PcmPipeline& Instance();

    esp_err_t Init();  // ES8388 via esp_codec_dev when ready; silence stub OK

    void StartCapture();
    void StopCapture();
    bool PopCaptureFrame(PcmFrame& out);

    /** Downlink: tag with response_id + generation; ClearGeneration on barge-in. */
    void EnqueuePlayback(const std::string& response_id, const uint8_t* pcm_bytes, size_t len);
    void ClearGeneration();
    uint32_t generation() const { return generation_; }

    /** Resample helpers 24k ↔ 16k (linear). */
    static std::vector<int16_t> Resample24kTo16k(const int16_t* in, size_t in_count);
    static std::vector<int16_t> Resample16kTo24k(const int16_t* in, size_t in_count);

    void SetDeviceAecEnabled(bool enabled);
    bool device_aec_enabled() const { return aec_enabled_; }

private:
    PcmPipeline() = default;

    static void CaptureTask(void* arg);

    bool capture_running_ = false;
    bool aec_enabled_ = false;
    uint32_t generation_ = 1;
    std::mutex mu_;
    std::deque<PcmFrame> capture_q_;
    std::deque<TaggedPcmChunk> playback_q_;
};

}  // namespace meet
