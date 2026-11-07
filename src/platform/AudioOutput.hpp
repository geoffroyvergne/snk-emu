#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <SDL.h>

#include "audio/AudioTypes.hpp"
#include "audio/Resampler.hpp"
#include "audio/SpscRingBuffer.hpp"

namespace snk::platform {

struct AudioOutputConfig {
    double targetLatencyMs = 50.0;   // buffered audio to aim for
    double maxRateAdjust   = 0.005;  // dynamic rate control range (+/-)
};

// Streams emulator audio to the device opened by SdlPlatform (SDL_mixer,
// 44.1 kHz S16 stereo).
//
// Threading: push() runs on the emulation thread once per frame; SDL_mixer's
// audio thread pulls through a music hook (an SDL audio callback). The two
// only share a lock-free SPSC ring buffer, so the callback never blocks.
//
// Rate control: the emulator is paced by the frame limiter, not the sound
// card. push() resamples with a nominal ratio (input rate / device rate)
// nudged by up to +/-0.5% according to how full the ring is, so the buffered
// latency stays near its target without audible pitch change.
class AudioOutput {
public:
    using Config = AudioOutputConfig;

    struct Stats {
        double   bufferedMs  = 0.0;
        double   rateAdjust  = 0.0;  // currently applied correction, e.g. +0.002
        uint64_t underruns   = 0;    // callbacks that ran out of data
        uint64_t overruns    = 0;    // frames dropped because the ring was full
    };

    // Hooks into the already-open SDL_mixer device. If audio is unavailable
    // (no device), the object is inert and push() discards data.
    AudioOutput(bool deviceOpen, Config config = {});
    ~AudioOutput();

    AudioOutput(const AudioOutput&)            = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    // Frames pushed per second of wall-clock time, e.g. the YM2610 rate times
    // (emulated FPS / native refresh) when running at 60 instead of 59.19 FPS.
    void setInputRate(double framesPerSecond) noexcept { inputRate_ = framesPerSecond; }

    void push(std::span<const StereoFrame> frames);

    [[nodiscard]] bool  enabled() const noexcept { return enabled_; }
    [[nodiscard]] int   deviceRate() const noexcept { return deviceRate_; }
    [[nodiscard]] Stats stats() const noexcept;

private:
    static void SDLCALL mixCallback(void* self, Uint8* stream, int len);
    void fill(StereoFrame* out, size_t frames) noexcept;

    using Ring = SpscRingBuffer<StereoFrame, 16384>;

    Config                   config_;
    bool                     enabled_    = false;
    int                      deviceRate_ = 44'100;
    double                   inputRate_  = 44'100.0;
    size_t                   targetFrames_ = 0;
    LinearResampler          resampler_;
    std::vector<StereoFrame> scratch_;
    std::unique_ptr<Ring>    ring_;

    // Written by one thread, read by the other.
    std::atomic<bool>     primed_{false};   // false: callback outputs silence until the ring fills
    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint64_t> overruns_{0};
    double                lastAdjust_ = 0.0;
    StereoFrame           lastFrame_{};      // callback thread only: held on underrun to avoid clicks
};

}  // namespace snk::platform
