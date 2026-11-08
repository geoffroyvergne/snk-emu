#include "core/System.hpp"

#include <algorithm>

#include "core/Hardware.hpp"

namespace snk {

System::System() : mainCpu_(bus_) {
    bus_.connectInterrupts(&irq_);
    bus_.connectVideo(&video_);
    bus_.connectInputs(&inputs_);
    bus_.connectSound(&sound_);
    bus_.connectRtc(&rtc_);
    bus_.connectWatchdog(&watchdog_);
    irq_.onLevelChanged([this](uint8_t level) { mainCpu_.setInterruptLevel(level); });
    video_.setTimerSyncHandler([this] { syncTimerToCpu(); });
    reset();
}

void System::reset() {
    bus_.reset(/*clearRam=*/true);
    rtc_.reset(Upd4990a::hostLocalTime());  // the calendar starts at the host's local time
    rtcTickAccumulator_ = 0;
    frameCount_         = 0;
    watchdogResets_     = 0;
    resetChips();
}

void System::hardwareReset() {
    bus_.reset(/*clearRam=*/false);
    resetChips();
}

void System::resetChips() {
    // The bus is reset first: it restores the BIOS vectors the CPU fetches.
    irq_.reset();
    sound_.reset();
    mainCpu_.reset();
    video_.reset();
    watchdog_.reset();
    mainCycleBalance_ = 0;
    irq_.raise(Irq::ColdBoot);  // the BIOS expects and acknowledges this after every reset
}

void System::runFrame() {
    video_.beginFrame(bus_.fixRom());
    sound_.beginFrame();
    for (uint32_t line = 0; line < hw::kTotalLines; ++line) {
        video_.setRasterLine(line);
        if (line == hw::kVBlankStartLine) {
            // End of the 224 active lines: VBlank, 68000 autovector IRQ 1.
            irq_.raise(Irq::VBlank);
            video_.onVBlankStart();
        }
        runScanline();
        // Draw the line after the CPU has run it, so mid-frame VRAM writes land
        // on the right line, as on hardware.
        if (line >= hw::kFirstActiveLine && line < hw::kVBlankStartLine) {
            video_.renderLine(line - hw::kFirstActiveLine, bus_.paletteRam(), bus_.shadowEnabled());
        }
    }
    video_.endFrame();
    ++frameCount_;
}

// One scanline = 768 68000 cycles (384 pixels x 2). The 68000 runs in slices
// that end either at the end of the line or exactly where the LSPC timer
// expires, so the timer IRQ (raster effects) lands on the right pixel rather
// than at a line boundary.
void System::runScanline() {
    mainCycleBalance_ += hw::kM68kCyclesPerLine;
    while (mainCycleBalance_ > 0) {
        const uint64_t untilTimer = video_.cyclesUntilTimer();
        const auto     slice      = static_cast<int32_t>(
            std::min<uint64_t>(static_cast<uint64_t>(mainCycleBalance_), std::max<uint64_t>(untilTimer, 1)));

        timerCyclesCredited_ = 0;
        const uint32_t used = mainCpu_.run(slice);
        mainCycleBalance_ -= used;
        // Count the rest of the slice (a mid-slice timer write already counted its part).
        if (video_.advanceTimer(used - std::min(used, timerCyclesCredited_))) {
            irq_.raise(Irq::Timer);
        }
        timerCyclesCredited_ = 0;
        if (used == 0) {
            break;  // CPU made no progress (e.g. halted backend): avoid spinning
        }
    }
    sound_.runScanline();

    if (watchdogEnabled_ && watchdog_.advance(hw::kM68kCyclesPerLine)) {
        ++watchdogResets_;
        hardwareReset();
    }

    // Calendar chip: 32.768 kHz crystal, advanced in exact integer steps
    // (one line = 384 pixels at 6 MHz = 2.097152 ticks).
    rtcTickAccumulator_ += uint64_t{hw::kTotalPixelsPerLine} * Upd4990a::kClockHz;
    const uint64_t ticks = rtcTickAccumulator_ / hw::kPixelClockHz;
    rtcTickAccumulator_ %= hw::kPixelClockHz;
    rtc_.advance(static_cast<uint32_t>(ticks));
}

// A timer register is about to be written from inside the CPU slice: count
// the cycles elapsed so far under the old settings, then cut the slice short
// so runScanline() recomputes when the timer expires next.
void System::syncTimerToCpu() {
    const uint32_t now = mainCpu_.cyclesThisSlice();
    if (now > timerCyclesCredited_) {
        if (video_.advanceTimer(now - timerCyclesCredited_)) {
            irq_.raise(Irq::Timer);
        }
        timerCyclesCredited_ = now;
    }
    mainCpu_.endTimeslice();
}

}  // namespace snk
