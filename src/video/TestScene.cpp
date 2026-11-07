#include "video/TestScene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include <vector>

#include "core/System.hpp"

namespace snk {

namespace {

constexpr uint32_t kPaletteRam = 0x40'0000;
constexpr uint32_t kLspcAddr   = 0x3C'0000;
constexpr uint32_t kLspcData   = 0x3C'0002;
constexpr uint32_t kLspcMod    = 0x3C'0004;
constexpr uint32_t kLspcMode   = 0x3C'0006;
constexpr uint32_t kRegCrtFix  = 0x3A'001B;  // select the cartridge S ROM for the fix layer

// Sprite tile numbers in our generated C ROM.
constexpr uint16_t kTileBall    = 1;
constexpr uint16_t kTileBrick   = 2;
constexpr uint16_t kTileArrow   = 3;
constexpr uint16_t kTileSpinner = 8;   // 8 frames: 8..15 (auto-animation replaces the low 3 bits)
constexpr uint32_t kTileCount   = 16;

// Palettes. Fix tiles can only use 0-15.
constexpr uint8_t kPalText    = 1;
constexpr uint8_t kPalBall    = 2;
constexpr uint8_t kPalBrick   = 3;
constexpr uint8_t kPalArrow   = 4;
constexpr uint8_t kPalSpinner = 5;

// Sprite numbers.
constexpr uint32_t kFloorFirst  = 1;   // 1..21, chained
constexpr uint32_t kFloorCount  = 21;
constexpr uint32_t kBallFirst   = 30;  // 30..37
constexpr uint32_t kBallCount   = 8;
constexpr uint32_t kColumn      = 50;
constexpr uint32_t kSpinner     = 60;

constexpr int kFloorTop = 192;  // screen Y of the floor's top edge

// 5-bit-per-channel color -> Neo-Geo palette word (dark bit clear).
constexpr uint16_t neoColor(uint32_t r, uint32_t g, uint32_t b) {
    return static_cast<uint16_t>(((r & 1u) << 14) | ((g & 1u) << 13) | ((b & 1u) << 12) |
                                 ((r >> 1) << 8) | ((g >> 1) << 4) | (b >> 1));
}

// SCB encoders. Sprite Y is stored as 496 - Y (top of screen = 496).
constexpr uint16_t scb2(uint32_t hShrink, uint32_t vShrink) {
    return static_cast<uint16_t>((hShrink << 8) | vShrink);
}
constexpr uint16_t scb3(int screenY, uint32_t heightInTiles, bool sticky = false) {
    const auto y = static_cast<uint32_t>(0x200 - (screenY + 16)) & 0x1FFu;
    return static_cast<uint16_t>((y << 7) | (sticky ? VideoSystem::kStickyBit : 0u) | heightInTiles);
}
constexpr uint16_t scb4(int x) {
    return static_cast<uint16_t>((static_cast<uint32_t>(x) & 0x1FFu) << 7);
}
// SCB1 odd word.
constexpr uint16_t tileAttr(uint8_t palette, uint32_t tile, uint16_t flags = 0) {
    return static_cast<uint16_t>((uint32_t{palette} << 8) | (((tile >> 16) & 0xFu) << 4) | flags);
}

// ---- Procedural tile art ----------------------------------------------------
using SpritePixels = std::array<uint8_t, 256>;

SpritePixels makeBall() {
    SpritePixels px{};
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const double dx = x - 7.5, dy = y - 7.5;
            if (dx * dx + dy * dy > 7.2 * 7.2) continue;
            const double light = std::hypot(x - 5.0, y - 5.0);  // highlight top-left
            px[static_cast<size_t>(y * 16 + x)] = static_cast<uint8_t>(1 + std::min(14.0, light * 1.25));
        }
    }
    return px;
}

SpritePixels makeBrick() {
    SpritePixels px{};
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const bool mortar = (y % 8 == 0) || (y < 8 ? x == 0 : x == 8);
            px[static_cast<size_t>(y * 16 + x)] =
                static_cast<uint8_t>(mortar ? 1 : 2 + ((x * 7 + y * 13) % 3));
        }
    }
    return px;
}

// Asymmetric on purpose, so flips are visible: up arrow on the left, block bottom-right.
SpritePixels makeArrow() {
    SpritePixels px{};
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const bool head  = y >= 1 && y <= 6 && std::abs(2 * x - 11) <= 2 * (y - 1);
            const bool shaft = y > 6 && y <= 14 && (x == 5 || x == 6);
            const bool block = x >= 11 && x <= 14 && y >= 11 && y <= 14;
            px[static_cast<size_t>(y * 16 + x)] = static_cast<uint8_t>(head || shaft ? 1 : block ? 2 : 0);
        }
    }
    return px;
}

// Frame k of a spinning bar inside a ring.
SpritePixels makeSpinner(int k) {
    SpritePixels px{};
    const double angle = k * 3.14159265358979 / 8.0;
    const double ux = std::cos(angle), uy = std::sin(angle);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const double dx = x - 7.5, dy = y - 7.5;
            const double r  = std::hypot(dx, dy);
            const double distToBar = std::abs(dx * uy - dy * ux);
            uint8_t c = 0;
            if (r >= 6.5 && r <= 7.6) c = 2;
            else if (r < 6.0 && distToBar < 1.2) c = 1;
            px[static_cast<size_t>(y * 16 + x)] = c;
        }
    }
    return px;
}

// ---- 3x5 font for the fix layer ---------------------------------------------
struct Glyph {
    char             ch;
    std::string_view rows[5];
};

constexpr Glyph kFont[] = {
    {'A', {".#.", "#.#", "###", "#.#", "#.#"}}, {'B', {"##.", "#.#", "##.", "#.#", "##."}},
    {'C', {"###", "#..", "#..", "#..", "###"}}, {'D', {"##.", "#.#", "#.#", "#.#", "##."}},
    {'E', {"###", "#..", "##.", "#..", "###"}}, {'F', {"###", "#..", "##.", "#..", "#.."}},
    {'G', {"###", "#..", "#.#", "#.#", "###"}}, {'L', {"#..", "#..", "#..", "#..", "###"}},
    {'M', {"#.#", "###", "###", "#.#", "#.#"}}, {'N', {"##.", "#.#", "#.#", "#.#", "#.#"}},
    {'O', {"###", "#.#", "#.#", "#.#", "###"}}, {'P', {"##.", "#.#", "##.", "#..", "#.."}},
    {'R', {"##.", "#.#", "##.", "#.#", "#.#"}}, {'S', {"###", "#..", "###", "..#", "###"}},
    {'T', {"###", ".#.", ".#.", ".#.", ".#."}}, {'-', {"...", "...", "###", "...", "..."}},
    {'0', {"###", "#.#", "#.#", "#.#", "###"}}, {'1', {".#.", "##.", ".#.", ".#.", "###"}},
    {'2', {"###", "..#", "###", "#..", "###"}}, {'3', {"###", "..#", ".##", "..#", "###"}},
    {'4', {"#.#", "#.#", "###", "..#", "..#"}}, {'5', {"###", "#..", "###", "..#", "###"}},
    {'6', {"###", "#..", "###", "#.#", "###"}}, {'7', {"###", "..#", "..#", "..#", "..#"}},
    {'8', {"###", "#.#", "###", "#.#", "###"}}, {'9', {"###", "#.#", "###", "..#", "###"}},
};

// S ROM with one tile per ASCII code (tile 0 and unlisted characters blank).
// Glyphs are drawn in color 1 with a color-2 drop shadow.
std::vector<uint8_t> buildFixRom() {
    std::vector<uint8_t> rom(256 * 32, 0);
    for (const Glyph& g : kFont) {
        std::array<uint8_t, 64> px{};
        for (int y = 0; y < 5; ++y) {
            for (int x = 0; x < 3; ++x) {
                if (g.rows[y][static_cast<size_t>(x)] != '#') continue;
                const int ox = 2 + x, oy = 1 + y;
                auto& shadow = px[static_cast<size_t>((oy + 1) * 8 + ox + 1)];
                if (shadow == 0) shadow = 2;
                px[static_cast<size_t>(oy * 8 + ox)] = 1;
            }
        }
        const size_t tile = static_cast<unsigned char>(g.ch);
        encodeFixTile(px, std::span<uint8_t, 32>(rom.data() + tile * 32, 32));
    }
    return rom;
}

}  // namespace

// ---------------------------------------------------------------------------
// Encoders (see VideoSystem.cpp for the format description)
// ---------------------------------------------------------------------------
void encodeSpriteTile(const std::array<uint8_t, 256>& pixels, std::span<uint8_t, 64> cOdd,
                      std::span<uint8_t, 64> cEven) {
    std::fill(cOdd.begin(), cOdd.end(), uint8_t{0});
    std::fill(cEven.begin(), cEven.end(), uint8_t{0});
    for (uint32_t y = 0; y < 16; ++y) {
        for (uint32_t x = 0; x < 16; ++x) {
            const uint32_t c   = pixels[y * 16 + x] & 0xFu;
            const size_t   off = (x < 8 ? 32u : 0u) + y * 2;
            const uint32_t bit = x & 7u;
            cOdd[off]      = static_cast<uint8_t>(cOdd[off]      | (((c >> 0) & 1u) << bit));
            cOdd[off + 1]  = static_cast<uint8_t>(cOdd[off + 1]  | (((c >> 1) & 1u) << bit));
            cEven[off]     = static_cast<uint8_t>(cEven[off]     | (((c >> 2) & 1u) << bit));
            cEven[off + 1] = static_cast<uint8_t>(cEven[off + 1] | (((c >> 3) & 1u) << bit));
        }
    }
}

void encodeFixTile(const std::array<uint8_t, 64>& pixels, std::span<uint8_t, 32> sRomTile) {
    std::fill(sRomTile.begin(), sRomTile.end(), uint8_t{0});
    for (uint32_t y = 0; y < 8; ++y) {
        for (uint32_t x = 0; x < 8; ++x) {
            const uint32_t c   = pixels[y * 8 + x] & 0xFu;
            uint8_t&       out = sRomTile[VideoSystem::kFixColumnOffset[x >> 1] + y];
            out = static_cast<uint8_t>(out | ((x & 1u) ? c << 4 : c));
        }
    }
}

// ---------------------------------------------------------------------------
// Scene setup
// ---------------------------------------------------------------------------
TestScene::TestScene(System& system) : system_(system) {
    MemoryBus&   bus   = system_.memory();
    VideoSystem& video = system_.video();

    // ---- C ROM pair (C1/C2) with our tiles ----
    std::vector<uint8_t> c1(kTileCount * 64), c2(kTileCount * 64);
    const auto putTile = [&](uint32_t tile, const SpritePixels& px) {
        encodeSpriteTile(px, std::span<uint8_t, 64>(c1.data() + tile * 64, 64),
                         std::span<uint8_t, 64>(c2.data() + tile * 64, 64));
    };
    putTile(kTileBall, makeBall());
    putTile(kTileBrick, makeBrick());
    putTile(kTileArrow, makeArrow());
    for (int k = 0; k < 8; ++k) putTile(kTileSpinner + static_cast<uint32_t>(k), makeSpinner(k));
    video.clearSpriteRoms();
    video.addSpriteRomPair(c1, c2);

    // ---- S ROM, selected through the system latch ----
    bus.loadRom(RomType::SRom, buildFixRom());
    bus.write8(kRegCrtFix, 0);

    // ---- Palettes (written into palette RAM like a game would) ----
    const auto setColor = [&](uint32_t palette, uint32_t index, uint16_t color) {
        bus.write16(kPaletteRam + (palette * 16 + index) * 2, color);
    };
    setColor(255, 15, neoColor(2, 3, 9));   // backdrop: last palette entry
    setColor(kPalText, 1, neoColor(31, 31, 31));
    setColor(kPalText, 2, neoColor(4, 4, 10));
    for (uint32_t i = 1; i < 16; ++i) {     // ball: white-yellow highlight -> dark red
        const uint32_t t = i - 1;
        setColor(kPalBall, i, neoColor(31 - t, 31 - t * 2, 24 - std::min(24u, t * 3)));
    }
    setColor(kPalBrick, 1, neoColor(12, 11, 10));
    setColor(kPalBrick, 2, neoColor(22, 9, 5));
    setColor(kPalBrick, 3, neoColor(20, 7, 4));
    setColor(kPalBrick, 4, neoColor(25, 11, 6));
    setColor(kPalArrow, 1, neoColor(8, 28, 31));
    setColor(kPalArrow, 2, neoColor(31, 24, 4));
    setColor(kPalSpinner, 1, neoColor(31, 10, 28));
    setColor(kPalSpinner, 2, neoColor(14, 6, 20));

    // ---- Clear all 68 KiB of VRAM through the data port ----
    setVramCursor(0x0000, 1);
    for (uint32_t i = 0; i < VideoSystem::kSlowVramWords; ++i) writeVramNext(0);
    setVramCursor(0x8000, 1);
    for (uint32_t i = 0; i < VideoSystem::kFastVramWords; ++i) writeVramNext(0);

    const auto setTileMap = [&](uint32_t sprite, uint32_t slot, uint32_t tile, uint16_t attr) {
        setVramCursor(static_cast<uint16_t>(VideoSystem::kScb1Base + sprite * 64 + slot * 2), 1);
        writeVramNext(static_cast<uint16_t>(tile));
        writeVramNext(attr);
    };
    const auto setSprite = [&](uint32_t sprite, uint16_t s2, uint16_t s3, uint16_t s4) {
        writeVram(static_cast<uint16_t>(VideoSystem::kScb2Base + sprite), s2);
        writeVram(static_cast<uint16_t>(VideoSystem::kScb3Base + sprite), s3);
        writeVram(static_cast<uint16_t>(VideoSystem::kScb4Base + sprite), s4);
    };

    // Floor: one 2-tile-high head sprite + 20 sticky sprites to its right.
    for (uint32_t i = 0; i < kFloorCount; ++i) {
        const uint32_t s = kFloorFirst + i;
        setTileMap(s, 0, kTileBrick, tileAttr(kPalBrick, kTileBrick));
        setTileMap(s, 1, kTileBrick, tileAttr(kPalBrick, kTileBrick, VideoSystem::kAttrHFlip));
        setSprite(s, scb2(15, 255), i == 0 ? scb3(kFloorTop, 2) : scb3(0, 0, true), scb4(0));
    }

    // Balls: one tile each, progressively shrunk.
    for (uint32_t i = 0; i < kBallCount; ++i) {
        const uint32_t s = kBallFirst + i;
        setTileMap(s, 0, kTileBall, tileAttr(kPalBall, kTileBall));
        setSprite(s, scb2(15 - i, 255 - i * 30), scb3(100, 1), scb4(20 + static_cast<int>(i) * 34));
    }

    // Column: the same arrow tile with each flip combination.
    const uint16_t flips[4] = {0, VideoSystem::kAttrHFlip, VideoSystem::kAttrVFlip,
                               VideoSystem::kAttrHFlip | VideoSystem::kAttrVFlip};
    for (uint32_t slot = 0; slot < 4; ++slot) {
        setTileMap(kColumn, slot, kTileArrow, tileAttr(kPalArrow, kTileArrow, flips[slot]));
    }
    setSprite(kColumn, scb2(15, 255), scb3(columnY_, 4), scb4(columnX_));

    // Spinner: hardware auto-animation over tiles 8..15, one step every 4 frames.
    setTileMap(kSpinner, 0, kTileSpinner, tileAttr(kPalSpinner, kTileSpinner, VideoSystem::kAttrAnim8));
    setSprite(kSpinner, scb2(15, 255), scb3(40, 1), scb4(150));
    bus.write16(kLspcMode, 3u << 8);

    writeText(2, 3, "NEO-GEO LSPC TEST");
}

void TestScene::update(uint64_t frame) {
    // Floor scrolls left; the 20 chained sprites follow the head automatically.
    writeVram(static_cast<uint16_t>(VideoSystem::kScb4Base + kFloorFirst),
              scb4(-static_cast<int>(frame % 16)));

    // Balls bounce on the floor (parabola), out of phase.
    for (uint32_t i = 0; i < kBallCount; ++i) {
        const int vShrink = 255 - static_cast<int>(i) * 30;
        const int height  = ((vShrink + 1) * 16 + 255) / 256;  // visible lines after shrink
        const int phase   = static_cast<int>((frame * 2 + i * 11) % 90) - 45;
        const int jump    = 110 - phase * phase * 110 / (45 * 45);
        writeVram(static_cast<uint16_t>(VideoSystem::kScb3Base + kBallFirst + i),
                  scb3(kFloorTop - height - jump, 1));
    }

    // Read the input registers exactly as 68000 code would (active low).
    MemoryBus&    bus     = system_.memory();
    const uint8_t p1      = bus.read8(0x30'0000);  // REG_P1CNT
    const uint8_t p2      = bus.read8(0x34'0000);  // REG_P2CNT
    const uint8_t statusA = bus.read8(0x32'0001);  // REG_STATUS_A
    const uint8_t statusB = bus.read8(0x38'0000);  // REG_STATUS_B
    const uint8_t sysType = bus.read8(0x30'0081);  // REG_SYSTYPE

    constexpr char kHex[] = "0123456789ABCDEF";
    char line[] = "P1 00 P2 00 SA 00 SB 00 ST 00";
    const uint8_t regs[] = {p1, p2, statusA, statusB, sysType};
    for (size_t i = 0; i < 5; ++i) {
        line[i * 6 + 3] = kHex[regs[i] >> 4];
        line[i * 6 + 4] = kHex[regs[i] & 0xF];
    }
    writeText(2, 5, line);

    // P1 moves the arrow column (bits are 0 while pressed).
    const int speed = (p1 & JoyBit::A) ? 1 : 3;  // hold A to move faster
    if (!(p1 & JoyBit::Left))  columnX_ -= speed;
    if (!(p1 & JoyBit::Right)) columnX_ += speed;
    if (!(p1 & JoyBit::Up))    columnY_ -= speed;
    if (!(p1 & JoyBit::Down))  columnY_ += speed;
    columnX_ = std::clamp(columnX_, 0, static_cast<int>(VideoSystem::kWidth) - 16);
    columnY_ = std::clamp(columnY_, 0, static_cast<int>(VideoSystem::kHeight) - 64);
    writeVram(static_cast<uint16_t>(VideoSystem::kScb3Base + kColumn), scb3(columnY_, 4));
    writeVram(static_cast<uint16_t>(VideoSystem::kScb4Base + kColumn), scb4(columnX_));

    char counter[] = "FRAME 00000";
    uint64_t n = frame;
    for (int d = 10; d >= 6; --d, n /= 10) counter[d] = static_cast<char>('0' + n % 10);
    writeText(26, 3, counter);
}

// ---------------------------------------------------------------------------
// LSPC register helpers: exactly what 68000 code does.
// ---------------------------------------------------------------------------
void TestScene::setVramCursor(uint16_t address, uint16_t modulo) {
    system_.memory().write16(kLspcMod, modulo);
    system_.memory().write16(kLspcAddr, address);
}

void TestScene::writeVramNext(uint16_t value) {
    system_.memory().write16(kLspcData, value);
}

void TestScene::writeVram(uint16_t address, uint16_t value) {
    setVramCursor(address, 0);
    writeVramNext(value);
}

// The fix map is column-major: a modulo of 32 steps to the next column.
void TestScene::writeText(uint32_t column, uint32_t row, const char* text) {
    setVramCursor(static_cast<uint16_t>(VideoSystem::kFixMapBase + column * VideoSystem::kFixRows + row),
                  VideoSystem::kFixRows);
    for (const char* c = text; *c != '\0'; ++c) {
        writeVramNext(static_cast<uint16_t>((uint32_t{kPalText} << 12) | static_cast<unsigned char>(*c)));
    }
}

}  // namespace snk
