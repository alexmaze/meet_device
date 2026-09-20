#include "resampler.h"

#include <algorithm>
#include <cstring>

namespace meet {
namespace {

// 15-tap FIR low-pass, ~7.2 kHz cutoff @ 24 kHz, Q15 coefficients.
// Symmetric; designed for anti-alias before 3:2 downsample.
constexpr int16_t kFirQ15[15] = {
    -98, -212, 0, 687, 0, -1966, 0, 8106, 0, -1966, 0, 687, 0, -212, -98,
};

}  // namespace

void Resampler24kTo16k::Reset() {
    std::memset(hist_, 0, sizeof(hist_));
    hist_pos_ = 0;
    phase_ = 0;
}

size_t Resampler24kTo16k::Process(const int16_t* in, size_t in_count, int16_t* out, size_t out_cap) {
    if (!in || !out || in_count == 0 || out_cap == 0) {
        return 0;
    }
    size_t produced = 0;
    for (size_t i = 0; i < in_count; ++i) {
        hist_[hist_pos_] = in[i];
        hist_pos_ = (hist_pos_ + 1) % kTaps;

        int32_t acc = 0;
        int idx = hist_pos_;
        for (int t = 0; t < kTaps; ++t) {
            idx = (idx + kTaps - 1) % kTaps;
            acc += hist_[idx] * static_cast<int32_t>(kFirQ15[t]);
        }
        const int16_t filtered = static_cast<int16_t>(std::max(
            -32768, std::min(32767, static_cast<int>(acc >> 15))));

        // Keep samples at phases 0 and 1 of every 3 (≈ 2/3 rate).
        if (phase_ == 0 || phase_ == 1) {
            if (produced < out_cap) {
                out[produced++] = filtered;
            }
        }
        phase_ = (phase_ + 1) % 3;
    }
    return produced;
}

}  // namespace meet
