#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

#include <SDL.h>

namespace snk::platform {

struct WindowConfig {
    std::string title       = "SNK Emu";
    uint32_t    logicalW    = 320;
    uint32_t    logicalH    = 224;
    uint32_t    scale       = 3;
    int         audioRateHz = 44'100;
    // No window, renderer or audio device (SDL video/audio not even started):
    // for automated runs. Input devices and timing still work.
    bool        headless    = false;
};

// RAII owner of all SDL resources: subsystems, window, renderer, streaming
// texture and the SDL_mixer audio device. Destroyed in reverse order.
class SdlPlatform {
public:
    explicit SdlPlatform(const WindowConfig& config);
    ~SdlPlatform();

    SdlPlatform(const SdlPlatform&)            = delete;
    SdlPlatform& operator=(const SdlPlatform&) = delete;

    // Drains the event queue, passing each event to `handler` (if any).
    // Returns false when the user asked to quit.
    [[nodiscard]] bool pollEvents(const std::function<void(const SDL_Event&)>& handler = {});

    // Uploads an ARGB8888 frame of logicalW*logicalH pixels and presents it.
    void present(std::span<const uint32_t> argb);

    void setTitle(const std::string& title);
    [[nodiscard]] bool audioAvailable() const noexcept { return audioOpen_; }
    [[nodiscard]] bool headless() const noexcept { return headless_; }

private:
    struct WindowDeleter   { void operator()(SDL_Window* w)   const noexcept { SDL_DestroyWindow(w); } };
    struct RendererDeleter { void operator()(SDL_Renderer* r) const noexcept { SDL_DestroyRenderer(r); } };
    struct TextureDeleter  { void operator()(SDL_Texture* t)  const noexcept { SDL_DestroyTexture(t); } };

    // Must be the first member: SDL_Init in its ctor, SDL_Quit in its dtor,
    // so it outlives every SDL object below.
    struct SdlInit {
        explicit SdlInit(bool headless);
        ~SdlInit();
        SdlInit(const SdlInit&)            = delete;
        SdlInit& operator=(const SdlInit&) = delete;
    };

    SdlInit                                        init_;
    std::unique_ptr<SDL_Window, WindowDeleter>     window_;
    std::unique_ptr<SDL_Renderer, RendererDeleter> renderer_;
    std::unique_ptr<SDL_Texture, TextureDeleter>   texture_;
    uint32_t logicalW_  = 0;
    uint32_t logicalH_  = 0;
    bool     audioOpen_ = false;
    bool     headless_  = false;
};

// Fixed-rate frame pacer using the high-resolution performance counter.
// Sleeps coarsely with SDL_Delay, then spins for the final ~1 ms to hit the
// deadline precisely. Deadlines advance by a fixed period, so there is no
// long-term drift; if the emulator falls behind it resyncs instead of
// trying to "catch up" with a burst of frames.
class FrameLimiter {
public:
    explicit FrameLimiter(double targetFps);

    // Blocks until the next frame deadline.
    void wait();

    // Measured time between consecutive wait() returns (= host frame period).
    struct Stats {
        double   targetMs = 0.0;
        double   averageMs = 0.0;
        double   minMs = 0.0;
        double   maxMs = 0.0;
        uint64_t frames = 0;
        uint64_t resyncs = 0;  // times the emulator fell more than a frame behind
    };
    [[nodiscard]] Stats stats() const noexcept;
    void resetStats() noexcept;

private:
    uint64_t frequency_;
    uint64_t period_;
    uint64_t nextDeadline_;

    uint64_t lastWake_ = 0;
    uint64_t sumTicks_ = 0, minTicks_ = UINT64_MAX, maxTicks_ = 0, frames_ = 0, resyncs_ = 0;
};

}  // namespace snk::platform
