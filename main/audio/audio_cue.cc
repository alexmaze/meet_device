#include "audio_cue.h"

#include "audio_pipeline.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace meet {
namespace {

constexpr int kRate = 16000;

void EnqueueTone(int freq_hz, int duration_ms, int amplitude) {
    const int samples = (kRate * duration_ms) / 1000;
    std::vector<int16_t> buf(static_cast<size_t>(samples));
    for (int i = 0; i < samples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kRate);
        buf[static_cast<size_t>(i)] = static_cast<int16_t>(
            std::sin(2.0f * 3.1415926f * static_cast<float>(freq_hz) * t) *
            static_cast<float>(amplitude));
    }
    AudioPipeline::Instance().EnqueuePlaybackSamples(buf.data(), buf.size());
}

}  // namespace

void PlayAudioCue(AudioCue cue) {
    switch (cue) {
        case AudioCue::Connected:
            EnqueueTone(880, 90, 9000);
            EnqueueTone(1175, 110, 9000);
            break;
        case AudioCue::Weak:
            EnqueueTone(660, 80, 7000);
            break;
        case AudioCue::Lost:
            EnqueueTone(440, 160, 8000);
            EnqueueTone(330, 200, 8000);
            break;
    }
}

}  // namespace meet
