#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Neo-Geo sprite tile (C ROM) decoding.
//
// A sprite tile is 16x16 pixels, 4 bits per pixel, stored as 4 bitplanes
// split over a PAIR of C ROMs: the odd ROM (C1, C3, ...) holds planes 0 and 1,
// the even ROM (C2, C4, ...) holds planes 2 and 3. Each ROM spends 64 bytes
// per tile, so tile t starts at byte t * 64 in both ROMs:
//
//   bytes  0-31  right half of the tile (pixels x = 8..15), rows 0..15
//   bytes 32-63  left half of the tile  (pixels x = 0..7),  rows 0..15
//
// Each row of a half takes 2 bytes, first the lower plane, then the upper one:
//
//   odd ROM,  byte (half + 2y)     -> plane 0 of 8 pixels of row y
//   odd ROM,  byte (half + 2y + 1) -> plane 1
//   even ROM, byte (half + 2y)     -> plane 2
//   even ROM, byte (half + 2y + 1) -> plane 3
//
// Within a byte, bit n is pixel n of that half: bit 0 is the LEFTMOST pixel.
// A pixel's color index is the 4 plane bits stacked:
//
//   color(x, y) = p0 | p1 << 1 | p2 << 2 | p3 << 3,  each pk = bit (x & 7)
//   of plane k's byte for row y in half (x < 8 ? 32 : 0).
//
// MAME loads a pair interleaved (ROM_LOAD16_BYTE: odd ROM on even bytes, even
// ROM on odd bytes). In that form a tile is 128 bytes and each row-half is 4
// consecutive bytes in plane order 0, 2, 1, 3.
//
// Color 0 is transparent for sprites. The color index selects an entry inside
// one of 256 palettes (chosen per tile in SCB1); see Palette.hpp.
namespace snk::tiles {

inline constexpr uint32_t kTileSize            = 16;
inline constexpr size_t   kTileBytesPerRom     = 64;
inline constexpr size_t   kTileBytesInterleaved = 128;

// Spreads the 8 bits of one plane byte into 8 nibbles: bit n -> bit 4n.
// Decoding a row half is then 4 table lookups, 3 shifts and 3 ORs instead
// of a loop over 32 individual bits.
inline constexpr std::array<uint32_t, 256> kPlaneSpread = [] {
    std::array<uint32_t, 256> table{};
    for (uint32_t byte = 0; byte < 256; ++byte) {
        uint32_t spread = 0;
        for (uint32_t bit = 0; bit < 8; ++bit) {
            spread |= ((byte >> bit) & 1u) << (4 * bit);
        }
        table[byte] = spread;
    }
    return table;
}();

// 8 pixels of a row half from its 4 plane bytes; pixel n in bits 4n..4n+3.
[[nodiscard]] constexpr uint32_t decodeRowHalf(uint8_t p0, uint8_t p1, uint8_t p2, uint8_t p3) noexcept {
    return kPlaneSpread[p0] | (kPlaneSpread[p1] << 1) | (kPlaneSpread[p2] << 2) | (kPlaneSpread[p3] << 3);
}

// One 16-pixel row (pixel x in bits 4x..4x+3) from a tile's 64 bytes in each
// ROM of the pair. This packed form is what VideoSystem keeps in memory.
[[nodiscard]] constexpr uint64_t decodeRow(const uint8_t* oddTile, const uint8_t* evenTile, uint32_t y) noexcept {
    const size_t   left  = 32 + size_t{y} * 2;
    const size_t   right = size_t{y} * 2;
    const uint64_t pixels0to7  = decodeRowHalf(oddTile[left], oddTile[left + 1], evenTile[left], evenTile[left + 1]);
    const uint64_t pixels8to15 = decodeRowHalf(oddTile[right], oddTile[right + 1], evenTile[right], evenTile[right + 1]);
    return pixels0to7 | (pixels8to15 << 32);
}

// Same row from an interleaved pair (128 bytes per tile, planes 0,2,1,3).
[[nodiscard]] constexpr uint64_t decodeRowInterleaved(const uint8_t* tile, uint32_t y) noexcept {
    const uint8_t* left  = tile + 64 + size_t{y} * 4;
    const uint8_t* right = tile + size_t{y} * 4;
    const uint64_t pixels0to7  = decodeRowHalf(left[0], left[2], left[1], left[3]);
    const uint64_t pixels8to15 = decodeRowHalf(right[0], right[2], right[1], right[3]);
    return pixels0to7 | (pixels8to15 << 32);
}

constexpr void unpackRow(uint64_t row, uint8_t* out16) noexcept {
    for (uint32_t x = 0; x < kTileSize; ++x) {
        out16[x] = static_cast<uint8_t>((row >> (4 * x)) & 0xFu);
    }
}

// Decodes tile `tileIndex` into 256 bytes (row-major, one color index 0-15
// per byte) from a pair kept as two separate ROM images.
constexpr void decodeTile(const uint8_t* cOdd, const uint8_t* cEven, uint32_t tileIndex, uint8_t* out256) noexcept {
    const size_t base = size_t{tileIndex} * kTileBytesPerRom;
    for (uint32_t y = 0; y < kTileSize; ++y) {
        unpackRow(decodeRow(cOdd + base, cEven + base, y), out256 + y * kTileSize);
    }
}

// Same, from a pair interleaved MAME-style into one buffer (128 bytes/tile).
constexpr void decodeTile(const uint8_t* cRomInterleaved, uint32_t tileIndex, uint8_t* out256) noexcept {
    const uint8_t* tile = cRomInterleaved + size_t{tileIndex} * kTileBytesInterleaved;
    for (uint32_t y = 0; y < kTileSize; ++y) {
        unpackRow(decodeRowInterleaved(tile, y), out256 + y * kTileSize);
    }
}

}  // namespace snk::tiles
