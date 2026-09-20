#pragma once

#include <cstddef>
#include <cstdint>

namespace meet {

/**
 * Fixed-state 24 kHz mono PCM16 → 16 kHz downsample with a short FIR low-pass
 * (cutoff ~7.2 kHz) then 3:2 decimation. Zero heap allocation.
 */
class Resampler24kTo16k {
public:
    void Reset();
    /** Returns number of output samples written to `out` (capacity must be >= in_count * 2 / 3 + 8). */
    size_t Process(const int16_t* in, size_t in_count, int16_t* out, size_t out_cap);

private:
    static constexpr int kTaps = 15;
    int32_t hist_[kTaps] = {};
    int hist_pos_ = 0;
    int phase_ = 0;  // 0..2 input phase within 3-sample group
};

}  // namespace meet
