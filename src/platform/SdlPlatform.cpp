#include "platform/SdlPlatform.hpp"

#include <SDL_mixer.h>

#include <algorithm>
#include <stdexcept>

namespace snk::platform {

namespace {

[[noreturn]] void throwSdlError(const char* what) {
    throw std::runtime_error(std::string(what) + ": " + SDL_GetError());
}

}  // namespace

// ---------------------------------------------------------------------------
// SdlInit
// ---------------------------------------------------------------------------
SdlPlatform::SdlInit::SdlInit(bool headless) {
    SDL_SetMainReady();  // we build with SDL_MAIN_HANDLED (no SDL2main)
    // Keep reading pads while another app has focus (e.g. a debugger window).
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    const Uint32 subsystems = SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER |
                              (headless ? 0u : (SDL_INIT_VIDEO | SDL_INIT_AUDIO));
    if (SDL_Init(subsystems) != 0) {
        throwSdlError("SDL_Init failed");
    }
}

SdlPlatform::SdlInit::~SdlInit() {
    SDL_Quit();
}

// ---------------------------------------------------------------------------
// SdlPlatform
// ---------------------------------------------------------------------------
SdlPlatform::SdlPlatform(const WindowConfig& config)
    : init_(config.headless), logicalW_(config.logicalW), logicalH_(config.logicalH), headless_(config.headless) {
    if (headless_) {
        SDL_Log("Headless: no window, no audio device");
        return;
    }
    // Crisp pixels when scaling up the 320x224 frame.
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

    window_.reset(SDL_CreateWindow(config.title.c_str(),
                                   SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   static_cast<int>(logicalW_ * config.scale),
                                   static_cast<int>(logicalH_ * config.scale),
                                   SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI));
    if (!window_) {
        throwSdlError("SDL_CreateWindow failed");
    }

    // No SDL_RENDERER_PRESENTVSYNC: FrameLimiter owns pacing, so the cap is the
    // same on 60/120/144 Hz monitors.
    renderer_.reset(SDL_CreateRenderer(window_.get(), -1, SDL_RENDERER_ACCELERATED));
    if (!renderer_) {
        throwSdlError("SDL_CreateRenderer failed");
    }
    SDL_RenderSetLogicalSize(renderer_.get(), static_cast<int>(logicalW_), static_cast<int>(logicalH_));
    SDL_RenderSetIntegerScale(renderer_.get(), SDL_TRUE);

    texture_.reset(SDL_CreateTexture(renderer_.get(), SDL_PIXELFORMAT_ARGB8888,
                                     SDL_TEXTUREACCESS_STREAMING,
                                     static_cast<int>(logicalW_), static_cast<int>(logicalH_)));
    if (!texture_) {
        throwSdlError("SDL_CreateTexture failed");
    }

    // Audio is optional: a missing/busy audio device must not prevent running.
    // allowed_changes = 0: we always get exactly this format; SDL converts to
    // whatever the hardware wants. 1024-frame chunks (~23 ms at 44.1 kHz).
    if (Mix_OpenAudioDevice(config.audioRateHz, AUDIO_S16SYS, 2, 1024, nullptr, 0) == 0) {
        audioOpen_ = true;
        const SDL_version* v = Mix_Linked_Version();
        SDL_Log("SDL_mixer %u.%u.%u: audio opened at %d Hz, S16 stereo", v->major, v->minor, v->patch,
                config.audioRateHz);
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Mix_OpenAudio failed, running muted: %s", Mix_GetError());
    }
}

SdlPlatform::~SdlPlatform() {
    if (audioOpen_) {
        Mix_CloseAudio();
    }
    if (!headless_) {
        Mix_Quit();
    }
    // texture_, renderer_, window_ then init_ are released by member destruction order.
}

bool SdlPlatform::pollEvents(const std::function<void(const SDL_Event&)>& handler) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (handler) {
            handler(event);
        }
        switch (event.type) {
            case SDL_QUIT:
                return false;
            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    return false;
                }
                break;
            default:
                break;
        }
    }
    return true;
}

void SdlPlatform::present(std::span<const uint32_t> argb) {
    if (headless_) {
        return;
    }
    if (argb.size() >= static_cast<size_t>(logicalW_) * logicalH_) {
        SDL_UpdateTexture(texture_.get(), nullptr, argb.data(),
                          static_cast<int>(logicalW_ * sizeof(uint32_t)));
    }
    SDL_SetRenderDrawColor(renderer_.get(), 0, 0, 0, 255);
    SDL_RenderClear(renderer_.get());
    SDL_RenderCopy(renderer_.get(), texture_.get(), nullptr, nullptr);
    SDL_RenderPresent(renderer_.get());
}

void SdlPlatform::setTitle(const std::string& title) {
    if (headless_) {
        return;
    }
    SDL_SetWindowTitle(window_.get(), title.c_str());
}

// ---------------------------------------------------------------------------
// FrameLimiter
// ---------------------------------------------------------------------------
FrameLimiter::FrameLimiter(double targetFps)
    : frequency_(SDL_GetPerformanceFrequency()),
      period_(static_cast<uint64_t>(static_cast<double>(frequency_) / targetFps)),
      nextDeadline_(SDL_GetPerformanceCounter() + period_) {}

void FrameLimiter::wait() {
    const uint64_t oneMs = frequency_ / 1000;
    uint64_t now = SDL_GetPerformanceCounter();

    // Coarse sleep, leaving ~2 ms of margin: SDL_Delay can oversleep by more
    // than 1 ms (measured on macOS), which showed up as +/-1.7 ms frame jitter.
    if (nextDeadline_ > now + 3 * oneMs) {
        SDL_Delay(static_cast<Uint32>((nextDeadline_ - now) / oneMs - 2));
    }
    // Fine-grained spin for the remainder.
    while ((now = SDL_GetPerformanceCounter()) < nextDeadline_) {
    }

    nextDeadline_ += period_;
    // More than one frame behind (debugger pause, window drag...): resync.
    if (now > nextDeadline_) {
        nextDeadline_ = now + period_;
        ++resyncs_;
    }

    if (lastWake_ != 0) {
        const uint64_t frame = now - lastWake_;
        sumTicks_ += frame;
        minTicks_ = std::min(minTicks_, frame);
        maxTicks_ = std::max(maxTicks_, frame);
        ++frames_;
    }
    lastWake_ = now;
}

FrameLimiter::Stats FrameLimiter::stats() const noexcept {
    const double toMs = 1000.0 / static_cast<double>(frequency_);
    Stats s;
    s.targetMs = static_cast<double>(period_) * toMs;
    s.frames   = frames_;
    s.resyncs  = resyncs_;
    if (frames_ != 0) {
        s.averageMs = static_cast<double>(sumTicks_) * toMs / static_cast<double>(frames_);
        s.minMs     = static_cast<double>(minTicks_) * toMs;
        s.maxMs     = static_cast<double>(maxTicks_) * toMs;
    }
    return s;
}

void FrameLimiter::resetStats() noexcept {
    sumTicks_ = maxTicks_ = frames_ = resyncs_ = 0;
    minTicks_ = UINT64_MAX;
}

}  // namespace snk::platform
