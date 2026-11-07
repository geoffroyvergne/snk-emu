#pragma once

#include <cstdint>

namespace snk {

// One stereo sample frame, signed 16-bit. Layout matches interleaved
// AUDIO_S16SYS stereo, so a span of frames can be copied to SDL as-is.
struct StereoFrame {
    int16_t left  = 0;
    int16_t right = 0;
};
static_assert(sizeof(StereoFrame) == 4);

}  // namespace snk
