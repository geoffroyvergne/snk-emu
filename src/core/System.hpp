#pragma once

#include <cstdint>
#include <span>

#include "audio/SoundSystem.hpp"
#include "core/Interrupts.hpp"
#include "core/Watchdog.hpp"
#include "cpu/m68k/M68000.hpp"
#include "input/InputPorts.hpp"
#include "memory/MemoryBus.hpp"
#include "rtc/Upd4990a.hpp"
#include "video/VideoSystem.hpp"

namespace snk {

// The whole emulated machine. Owns every component; there is no global state,
// so multiple instances can coexist (tests, netplay, save-state diffing...).
// Has no SDL dependency: the frontend feeds it input and reads its outputs.
class System {
public:
    System();

    System(const System&)            = delete;
    System& operator=(const System&) = delete;

    // Power-on reset: clears RAM, restarts the calendar from the host clock.
    void reset();
    // Reset line pulled by the watchdog (or a reset button): CPUs, sound and
    // video restart; work RAM, backup RAM and the calendar keep their contents.
    void hardwareReset();
    // The watchdog is on by default, as on hardware. Test harnesses and the
    // built-in test scene, which run no code that kicks it, turn it off.
    void setWatchdogEnabled(bool enabled) noexcept { watchdogEnabled_ = enabled; }
    // Number of watchdog-triggered resets since power-on.
    [[nodiscard]] uint32_t watchdogResets() const noexcept { return watchdogResets_; }

    // Emulates exactly one native video frame (~16.9 ms of machine time),
    // interleaving the CPUs scanline by scanline.
    void runFrame();

    [[nodiscard]] std::span<const uint32_t> framebuffer() const noexcept { return video_.framebuffer(); }
    [[nodiscard]] uint64_t frameCount() const noexcept { return frameCount_; }

    // ROM loading goes through the bus; call reset() afterwards.
    [[nodiscard]] MemoryBus& memory() noexcept { return bus_; }
    [[nodiscard]] const M68000& mainCpu() const noexcept { return mainCpu_; }
    [[nodiscard]] const InterruptController& interrupts() const noexcept { return irq_; }
    // C ROM / L0 ROM loading and VRAM inspection.
    [[nodiscard]] VideoSystem& video() noexcept { return video_; }
    // Frontends push controller state here (InputPorts::setState) each frame.
    [[nodiscard]] InputPorts& inputs() noexcept { return inputs_; }
    // Z80-side ROM loading (SM1, M1, V ROMs).
    [[nodiscard]] SoundSystem& sound() noexcept { return sound_; }
    [[nodiscard]] Upd4990a& rtc() noexcept { return rtc_; }
    // Audio produced by the last runFrame(), at hw::kYm2610SampleRateExact.
    [[nodiscard]] std::span<const StereoFrame> audioSamples() const noexcept { return sound_.frameSamples(); }

private:
    void runScanline();
    void resetChips();
    void syncTimerToCpu();

    // Declaration order matters: everything the CPU references through the
    // bus must exist before it.
    MemoryBus           bus_;
    InterruptController irq_;
    VideoSystem         video_;
    InputPorts          inputs_;
    SoundSystem         sound_;
    Upd4990a            rtc_;
    M68000              mainCpu_;
    Watchdog            watchdog_;

    // 68000 cycle budget still owed. Goes negative when an instruction
    // overshoots a slice; the overshoot is repaid by the next slice, so the
    // long-run clock rate is exact. (SoundSystem does the same for the Z80.)
    int64_t  mainCycleBalance_  = 0;
    uint32_t timerCyclesCredited_ = 0;  // cycles of the current slice already counted by the LSPC timer
    uint64_t frameCount_        = 0;
    uint32_t watchdogResets_    = 0;
    bool     watchdogEnabled_   = true;
    uint64_t rtcTickAccumulator_ = 0;  // 32.768 kHz ticks owed, scaled by the 6 MHz pixel clock
};

}  // namespace snk
