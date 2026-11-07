#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace snk {

class System;

// A synthetic scene that drives the real video pipeline without any game ROM.
// Everything goes through the same paths a game uses: tiles are encoded into
// native C ROM / S ROM formats, colors are written to palette RAM, and all
// VRAM writes go through the LSPC registers at 0x3C0000 (address, data,
// auto-increment modulo). On screen:
//   - a scrolling floor of 21 sticky-chained sprites
//   - 8 bouncing balls, each with a different H/V shrink
//   - a 4-tile column showing H/V flips
//   - an 8-frame auto-animated sprite (LSPC hardware animation)
//   - fix-layer text with a live frame counter
//   - the raw input registers (read through the 68000 bus, like a game
//     would) in hex; P1's joystick moves the arrow column
class TestScene {
public:
    explicit TestScene(System& system);

    // Animates the scene; call once per frame, before System::runFrame().
    void update(uint64_t frame);

private:
    void writeVram(uint16_t address, uint16_t value);
    void setVramCursor(uint16_t address, uint16_t modulo);
    void writeVramNext(uint16_t value);
    void writeText(uint32_t column, uint32_t row, const char* text);

    System& system_;
    int     columnX_ = 292;
    int     columnY_ = 56;
};

// Native ROM-format encoders (the inverse of VideoSystem's decoders).
// pixels: row-major color indices 0-15.
void encodeSpriteTile(const std::array<uint8_t, 256>& pixels, std::span<uint8_t, 64> cOdd,
                      std::span<uint8_t, 64> cEven);
void encodeFixTile(const std::array<uint8_t, 64>& pixels, std::span<uint8_t, 32> sRomTile);

}  // namespace snk
