#pragma once

#include <array>
#include <cstdint>
#include <span>

// Neo-Geo colors.
//
// Palette RAM (68000 0x400000-0x401FFF, 2 switchable banks) holds 256
// palettes x 16 colors. It is separate from the LSPC's VRAM. Each color is
// one big-endian 16-bit word:
//
//   bit  15   14   13   12   11-8        7-4         3-0
//        D    R0   G0   B0   R4 R3 R2 R1 G4 G3 G2 G1 B4 B3 B2 B1
//
// i.e. 5 bits per channel, with each channel's LSB gathered in bits 14-12,
// plus a "dark" bit D shared by all three channels. On hardware D pulls each
// DAC output down by a fraction of one step, so a channel behaves like a
// 6-bit value whose LSB is !D:
//
//   c6 = (c5 << 1) | !D        c8 = (c6 << 2) | (c6 >> 4)   (6 -> 8 bit scaling)
//
// This is a linear model of the resistor DAC (MAME models the exact resistor
// weights); it keeps white at 255 and black-with-D at 0.
//
// Color 0 of every palette is transparent for sprites and the fix layer; the
// last entry (palette 255, color 15) is the backdrop.
namespace snk::palette {

inline constexpr uint32_t kEntries       = 4096;  // per bank
inline constexpr uint32_t kBackdropEntry = 4095;

[[nodiscard]] constexpr uint32_t toArgb(uint16_t color) noexcept {
    const uint32_t notDark = ((color >> 15) & 1u) ^ 1u;
    const auto channel = [notDark](uint32_t high4, uint32_t low1) {
        const uint32_t c6 = (((high4 << 1) | low1) << 1) | notDark;
        return (c6 << 2) | (c6 >> 4);
    };
    const uint32_t r = channel((color >> 8) & 0xFu, (color >> 14) & 1u);
    const uint32_t g = channel((color >> 4) & 0xFu, (color >> 13) & 1u);
    const uint32_t b = channel(color & 0xFu, (color >> 12) & 1u);
    return 0xFF00'0000u | (r << 16) | (g << 8) | b;
}

static_assert(toArgb(0x7FFF) == 0xFFFF'FFFFu);  // white
static_assert(toArgb(0x8000) == 0xFF00'0000u);  // black, dark bit set
static_assert(toArgb(0x0000) == 0xFF04'0404u);  // black, dark bit clear: one 6-bit step up
static_assert(toArgb(0x4F00) == 0xFFFF'0404u);  // pure red (R4-R0 set)

// REG_SHADOW: the whole output at half brightness.
[[nodiscard]] constexpr uint32_t shadow(uint32_t argb) noexcept {
    return 0xFF00'0000u | ((argb >> 1) & 0x007F'7F7Fu);
}

// Every possible palette word -> ARGB8888, built once (256 KiB). Converting a
// color is then a single load, cheap enough to resolve pixels against live
// palette RAM on every scanline, so mid-frame palette changes show up.
[[nodiscard]] const std::array<uint32_t, 0x1'0000>& lookupTable();

// Palette RAM (big-endian words) entry -> ARGB, through the lookup table.
[[nodiscard]] inline uint32_t entryToArgb(const std::array<uint32_t, 0x1'0000>& lut,
                                          std::span<const uint8_t> paletteRam, uint32_t entry) noexcept {
    const uint32_t offset = entry * 2;
    return lut[static_cast<uint32_t>(paletteRam[offset] << 8) | paletteRam[offset + 1]];
}

// For tools such as a tile viewer: color indices (0-15) of one palette ->
// ARGB. Index 0 becomes fully transparent (alpha 0), like on screen.
void indicesToArgb(std::span<const uint8_t> indices, std::span<const uint8_t> paletteRam, uint8_t paletteNumber,
                   std::span<uint32_t> out);

}  // namespace snk::palette
