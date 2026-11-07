#include "platform/AudioOutput.hpp"

#include <SDL_mixer.h>

#include <algorithm>

namespace snk::platform {

AudioOutput::AudioOutput(bool deviceOpen, Config config)
    : config_(config), ring_(std::make_unique<Ring>()) {
    if (!deviceOpen) {
        return;
    }
    int    frequency = 0;
    Uint16 format    = 0;
    int    channels  = 0;
    if (Mix_QuerySpec(&frequency, &format, &channels) == 0 || format != AUDIO_S16SYS || channels != 2) {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Unexpected mixer format; emulator audio disabled");
        return;
    }
    deviceRate_   = frequency;
    targetFrames_ = static_cast<size_t>(config_.targetLatencyMs * deviceRate_ / 1000.0);
    targetFrames_ = std::min(targetFrames_, Ring::capacity() / 2);
    scratch_.reserve(2048);
    enabled_ = true;

    // The music hook runs first in SDL_mixer's callback; sound effects played
    // through Mix_PlayChannel are mixed on top of it.
    Mix_HookMusic(&AudioOutput::mixCallback, this);
}

AudioOutput::~AudioOutput() {
    if (enabled_) {
        Mix_HookMusic(nullptr, nullptr);  // takes the audio lock: no callback runs past this
    }
}

// ---------------------------------------------------------------------------
// Emulation thread
// ---------------------------------------------------------------------------
void AudioOutput::push(std::span<const StereoFrame> frames) {
    if (!enabled_ || frames.empty()) {
        return;
    }

    // Dynamic rate control: below target -> make more output frames (smaller
    // step), above target -> fewer. Linear in the error, clamped.
    const double fill   = static_cast<double>(ring_->size());
    const double target = static_cast<double>(targetFrames_);
    const double error  = (target - fill) / target;  // +1 empty .. negative when over-full
    lastAdjust_ = std::clamp(error * config_.maxRateAdjust, -config_.maxRateAdjust, config_.maxRateAdjust);

    const double step = inputRate_ / deviceRate_ * (1.0 - lastAdjust_);
    scratch_.clear();
    resampler_.process(frames, step, [this](const StereoFrame& f) { scratch_.push_back(f); });

    const size_t pushed = ring_->push(scratch_);
    if (pushed < scratch_.size()) {
        overruns_.fetch_add(scratch_.size() - pushed, std::memory_order_relaxed);
    }
    if (!primed_.load(std::memory_order_relaxed) && ring_->size() >= targetFrames_) {
        primed_.store(true, std::memory_order_release);
    }
}

AudioOutput::Stats AudioOutput::stats() const noexcept {
    Stats s;
    s.bufferedMs = 1000.0 * static_cast<double>(ring_->size()) / deviceRate_;
    s.rateAdjust = lastAdjust_;
    s.underruns  = underruns_.load(std::memory_order_relaxed);
    s.overruns   = overruns_.load(std::memory_order_relaxed);
    return s;
}

// ---------------------------------------------------------------------------
// Audio thread
// ---------------------------------------------------------------------------
void SDLCALL AudioOutput::mixCallback(void* self, Uint8* stream, int len) {
    // AUDIO_S16SYS stereo is exactly an array of StereoFrame.
    static_cast<AudioOutput*>(self)->fill(reinterpret_cast<StereoFrame*>(stream),
                                          static_cast<size_t>(len) / sizeof(StereoFrame));
}

void AudioOutput::fill(StereoFrame* out, size_t frames) noexcept {
    if (!primed_.load(std::memory_order_acquire)) {
        std::fill_n(out, frames, StereoFrame{});  // still building the initial cushion
        return;
    }
    const size_t got = ring_->pop(std::span(out, frames));
    if (got > 0) {
        lastFrame_ = out[got - 1];
    }
    if (got < frames) {
        // Underrun: hold the last sample (no click), then re-prime.
        std::fill_n(out + got, frames - got, lastFrame_);
        underruns_.fetch_add(1, std::memory_order_relaxed);
        primed_.store(false, std::memory_order_release);
    }
}

}  // namespace snk::platform
