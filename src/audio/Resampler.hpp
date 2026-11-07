#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

#include "audio/AudioTypes.hpp"

namespace snk {

// Streaming linear-interpolation resampler. The ratio can change between
// calls (used for dynamic rate control); the fractional read position and the
// last input frame carry over, so consecutive blocks join without clicks.
//
// Linear interpolation is cheap and fine for a first pass; a windowed-sinc or
// polyphase filter would reduce aliasing from the YM2610's 55.5 kHz output.
class LinearResampler {
public:
    void reset() noexcept {
        position_ = 0.0;
        previous_ = {};
    }

    // step = input frames consumed per output frame (inputRate / outputRate).
    // Calls sink(StereoFrame) once per output frame.
    template <typename Sink>
    void process(std::span<const StereoFrame> input, double step, Sink&& sink) {
        const size_t n = input.size();
        if (n == 0 || step <= 0.0) {
            return;
        }
        // Virtual sequence: index 0 = previous_, index k = input[k - 1].
        const auto at = [&](size_t i) -> const StereoFrame& { return i == 0 ? previous_ : input[i - 1]; };

        while (true) {
            const auto   i    = static_cast<size_t>(position_);
            if (i + 1 > n) {
                break;  // need frames i and i+1; the last virtual index is n
            }
            const double frac = position_ - static_cast<double>(i);
            const StereoFrame& a = at(i);
            const StereoFrame& b = at(i + 1);
            sink(StereoFrame{lerp(a.left, b.left, frac), lerp(a.right, b.right, frac)});
            position_ += step;
        }
        position_ -= static_cast<double>(n);  // virtual index n becomes the new index 0
        previous_ = input[n - 1];
    }

private:
    static int16_t lerp(int16_t a, int16_t b, double t) noexcept {
        const double v = a + (b - a) * t;
        return static_cast<int16_t>(std::clamp(std::lround(v), -32768L, 32767L));
    }

    double      position_ = 0.0;
    StereoFrame previous_{};
};

}  // namespace snk
