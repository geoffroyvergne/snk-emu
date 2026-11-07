#pragma once

#include <cstdint>

// Neo-Geo MVS/AES hardware constants. Single source of truth for clocks and
// video geometry; every component derives its timing from these.
namespace snk::hw {

inline constexpr uint32_t kMasterClockHz = 24'000'000;
inline constexpr uint32_t kM68kClockHz   = kMasterClockHz / 2;  // 12 MHz main CPU
inline constexpr uint32_t kZ80ClockHz    = kMasterClockHz / 6;  //  4 MHz sound CPU
inline constexpr uint32_t kPixelClockHz  = kMasterClockHz / 4;  //  6 MHz

// Visible area.
inline constexpr uint32_t kScreenWidth  = 320;
inline constexpr uint32_t kScreenHeight = 224;

// Full raster including horizontal/vertical blanking.
inline constexpr uint32_t kTotalPixelsPerLine = 384;
inline constexpr uint32_t kTotalLines         = 264;

// Native refresh is ~59.1856 Hz (6 MHz / (384 * 264)). The frontend caps at a
// configurable rate (60 FPS by default); cycle budgets per frame always use the
// native raster so emulated timing stays correct regardless of the host cap.
inline constexpr double kNativeRefreshHz =
    static_cast<double>(kPixelClockHz) / (kTotalPixelsPerLine * kTotalLines);

// CPU budgets per scanline (the unit of interleaving between chips).
inline constexpr uint32_t kM68kCyclesPerLine = kM68kClockHz / kPixelClockHz * kTotalPixelsPerLine;  // 768
inline constexpr uint32_t kZ80CyclesPerLine  = kM68kCyclesPerLine * kZ80ClockHz / kM68kClockHz;    // 256

inline constexpr uint32_t kM68kCyclesPerFrame = kM68kCyclesPerLine * kTotalLines;  // 202'752
inline constexpr uint32_t kZ80CyclesPerFrame  = kZ80CyclesPerLine * kTotalLines;   //  67'584

// The LSPC line counter runs 0x0F8..0x1FF (264 lines): 8 lines of vsync and
// 16 of top border, then 224 active lines (0x110..0x1EF), then 16 of bottom
// border. VBlank (and its IRQ) starts at counter 0x1F0.
inline constexpr uint32_t kFirstActiveLine = 24;
inline constexpr uint32_t kVBlankStartLine = kFirstActiveLine + kScreenHeight;  // 248

static_assert(kM68kCyclesPerFrame == 202'752 && kZ80CyclesPerFrame == 67'584);

// YM2610: 8 MHz input clock; one output sample every 144 clocks (~55.5 kHz).
inline constexpr uint32_t kYm2610ClockHz         = kMasterClockHz / 3;
inline constexpr uint32_t kYm2610ClocksPerSample = 144;
inline constexpr uint32_t kYm2610SampleRateHz    = kYm2610ClockHz / kYm2610ClocksPerSample;  // 55'555
inline constexpr uint32_t kYm2610ClocksPerLine   = static_cast<uint32_t>(
    uint64_t{kM68kCyclesPerLine} * kYm2610ClockHz / kM68kClockHz);  // 512 (64-bit: the product overflows 32)
inline constexpr double   kYm2610SampleRateExact =
    static_cast<double>(kYm2610ClockHz) / kYm2610ClocksPerSample;  // 55'555.55...

static_assert(kYm2610ClocksPerLine == 512);

}  // namespace snk::hw
