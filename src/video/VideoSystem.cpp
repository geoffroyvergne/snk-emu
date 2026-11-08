#include "video/VideoSystem.hpp"

#include <algorithm>
#include <bit>
#include <string>

#include "core/RomFile.hpp"
#include "video/Palette.hpp"
#include "video/TileCodec.hpp"

namespace snk {

namespace {

constexpr size_t   kTileBytesPerRom = tiles::kTileBytesPerRom;
constexpr size_t   kMaxTiles        = 1u << 20;  // 20-bit tile numbers
constexpr size_t   kMaxCRomSize     = kMaxTiles * kTileBytesPerRom;

// Horizontal shrink: for SCB2 H value v, the sprite is v+1 pixels wide and
// these are the source pixels kept. Fixed in the LSPC.
constexpr std::array<std::array<uint8_t, 16>, 16> kHorizontalShrink = {{
    {0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0},
    {0,0,0,0,1,0,0,0,1,0,0,0,0,0,0,0},
    {0,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0},
    {0,0,1,0,1,0,0,0,1,0,0,0,1,0,0,0},
    {0,0,1,0,1,0,0,0,1,0,0,0,1,0,1,0},
    {0,0,1,0,1,0,1,0,1,0,0,0,1,0,1,0},
    {0,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0},
    {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0},
    {1,0,1,0,1,0,1,0,1,1,1,0,1,0,1,0},
    {1,0,1,1,1,0,1,0,1,1,1,0,1,0,1,0},
    {1,0,1,1,1,0,1,0,1,1,1,0,1,0,1,1},
    {1,0,1,1,1,0,1,1,1,1,1,0,1,0,1,1},
    {1,0,1,1,1,0,1,1,1,1,1,0,1,1,1,1},
    {1,1,1,1,1,0,1,1,1,1,1,0,1,1,1,1},
    {1,1,1,1,1,0,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
}};

// Each row keeps exactly v+1 pixels, and each row is a superset of the previous.
constexpr bool horizontalShrinkTableIsConsistent() {
    for (size_t v = 0; v < 16; ++v) {
        size_t kept = 0;
        for (size_t i = 0; i < 16; ++i) {
            kept += kHorizontalShrink[v][i];
            if (v > 0 && kHorizontalShrink[v - 1][i] > kHorizontalShrink[v][i]) return false;
        }
        if (kept != v + 1) return false;
    }
    return true;
}
static_assert(horizontalShrinkTableIsConsistent());

// Stand-in for the L0 ROM: for shrink v, line i (0..v) samples source line
// i*256/(v+1) of the sprite's top half. Lines past the shrunk height point at
// the last row of tile slot 15. Load the real 000-lo.lo for exact results.
void buildApproximateZoomTable(std::array<uint8_t, 0x10000>& table) {
    for (uint32_t v = 0; v < 256; ++v) {
        for (uint32_t i = 0; i < 256; ++i) {
            const uint32_t src = (i <= v) ? (i * 256) / (v + 1) : 0xFF;
            table[(v << 8) | i] = static_cast<uint8_t>(src);
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction / reset
// ---------------------------------------------------------------------------
VideoSystem::VideoSystem() {
    buildApproximateZoomTable(zoomTable_);
    reset();
}

void VideoSystem::reset() {
    vram_.fill(0);
    vramAddr_ = vramMod_ = 0;
    autoAnimSpeed_ = autoAnimFrameCounter_ = autoAnimCounter_ = 0;
    autoAnimDisabled_ = false;
    timerMode_       = 0;
    timerReload_     = 0;
    timerStopPal_    = false;
    timerRunning_    = false;
    timerCyclesLeft_ = 0;
    rasterLine_   = 0;
    framebuffer_.fill(0xFF00'0000u);
}

// ---------------------------------------------------------------------------
// Tile ROMs
// ---------------------------------------------------------------------------
// C ROM tile format: see TileCodec.hpp.
void VideoSystem::addSpriteRomPair(std::span<const uint8_t> cOdd, std::span<const uint8_t> cEven) {
    if (cOdd.size() != cEven.size()) {
        throw RomLoadError("C ROM pair: odd and even ROMs differ in size (" +
                           std::to_string(cOdd.size()) + " vs " + std::to_string(cEven.size()) + ")");
    }
    if (cOdd.empty() || cOdd.size() % kTileBytesPerRom != 0) {
        throw RomLoadError("C ROM pair: size must be a non-zero multiple of 64 bytes");
    }
    const size_t newTiles = cOdd.size() / kTileBytesPerRom;
    if (tileCount() + newTiles > kMaxTiles) {
        throw RomLoadError("C ROMs: more than 2^20 tiles");
    }

    const size_t firstRow = tiles_.size();
    tiles_.resize(firstRow + newTiles * 16);

    for (size_t t = 0; t < newTiles; ++t) {
        const uint8_t* odd  = cOdd.data() + t * kTileBytesPerRom;
        const uint8_t* even = cEven.data() + t * kTileBytesPerRom;
        for (uint32_t y = 0; y < 16; ++y) {
            tiles_[firstRow + t * 16 + y] = tiles::decodeRow(odd, even, y);
        }
    }
    tileMask_ = static_cast<uint32_t>(std::bit_ceil(tileCount()) - 1);
}

void VideoSystem::addSpriteRomPairFiles(const std::filesystem::path& cOdd,
                                        const std::filesystem::path& cEven) {
    const auto odd  = readRomFile(cOdd, kMaxCRomSize);
    const auto even = readRomFile(cEven, kMaxCRomSize);
    try {
        addSpriteRomPair(odd, even);
    } catch (const RomLoadError& e) {
        throw RomLoadError(cOdd.string() + " + " + cEven.string() + ": " + e.what());
    }
}

void VideoSystem::clearSpriteRoms() noexcept {
    tiles_.clear();
    tileMask_ = 0;
}

void VideoSystem::loadZoomRom(std::span<const uint8_t> data) {
    if (data.size() < zoomTable_.size()) {
        throw RomLoadError("L0 zoom ROM: expected at least 64 KiB");
    }
    std::copy_n(data.begin(), zoomTable_.size(), zoomTable_.begin());
}

uint64_t VideoSystem::spriteTileRow(uint32_t tile, uint32_t row) const noexcept {
    return tile < tileCount() ? tiles_[size_t{tile} * 16 + (row & 15u)] : 0;
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------
// Reads decode only A1-A2: four registers, mirrored every 8 bytes (writes
// decode 8 registers, mirrored every 16). Reading never moves the VRAM address.
uint16_t VideoSystem::readRegister(uint32_t offset) const noexcept {
    switch (offset & 0x06) {
        case kRegVramAddr:
        case kRegVramRw:
            return vram_[vramIndex(vramAddr_)];
        case kRegVramMod:
            return vramMod_;
        default:  // kRegLspcMode
        {
            // Line counter (15-7, runs 0x0F8..0x1FF), NTSC (bit 3 = 0), anim counter (2-0).
            const uint32_t counter = 0xF8u + rasterLine_;
            return static_cast<uint16_t>((counter << 7) | (autoAnimCounter_ & 7u));
        }
    }
}

void VideoSystem::writeRegister(uint32_t offset, uint16_t value) noexcept {
    const uint32_t reg = offset & 0x0E;
    if (timerSync_ && (reg == kRegLspcMode || reg == kRegTimerHigh || reg == kRegTimerLow || reg == kRegTimerStop)) {
        timerSync_();  // count the time elapsed so far with the old settings
    }
    switch (reg) {
        case kRegVramAddr:
            vramAddr_ = value;
            break;
        case kRegVramRw:
            vram_[vramIndex(vramAddr_)] = value;
            // The modulo is added to bits 14-0 only: the slow/fast select bit sticks.
            vramAddr_ = static_cast<uint16_t>((vramAddr_ & 0x8000u) | ((vramAddr_ + vramMod_) & 0x7FFFu));
            break;
        case kRegVramMod:
            vramMod_ = value;
            break;
        case kRegLspcMode:
            autoAnimSpeed_    = static_cast<uint8_t>(value >> 8);
            timerMode_        = static_cast<uint8_t>((value >> 4) & 0x0F);
            autoAnimDisabled_ = (value & 0x0008) != 0;
            break;
        case kRegTimerHigh:
            timerReload_ = (timerReload_ & 0x0000'FFFFu) | (uint32_t{value} << 16);
            break;
        case kRegTimerLow:
            timerReload_ = (timerReload_ & 0xFFFF'0000u) | value;
            if (timerMode_ & kTimerReloadOnWrite) {
                loadTimer();
            }
            break;
        case kRegTimerStop:
            timerStopPal_ = (value & 1u) != 0;
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// LSPC timer
// ---------------------------------------------------------------------------
void VideoSystem::loadTimer() noexcept {
    timerCyclesLeft_ = (uint64_t{timerReload_} + 1) * 2;  // pixels -> 68000 cycles
    timerRunning_    = true;
}

bool VideoSystem::advanceTimer(uint64_t m68kCycles) noexcept {
    if (!timerRunning_ || m68kCycles < timerCyclesLeft_) {
        if (timerRunning_) timerCyclesLeft_ -= m68kCycles;
        return false;
    }
    // Expired. In repeat mode the counter restarts at once; cycles past the
    // expiry point (CPU overshoot) count toward the next period.
    const uint64_t overshoot = m68kCycles - timerCyclesLeft_;
    if (timerMode_ & kTimerRepeat) {
        const uint64_t period = (uint64_t{timerReload_} + 1) * 2;
        timerCyclesLeft_ = period - (overshoot % period);
    } else {
        timerRunning_ = false;
    }
    return (timerMode_ & kTimerIrqEnable) != 0;
}

void VideoSystem::onVBlankStart() noexcept {
    if (timerMode_ & kTimerReloadAtVBlank) {
        loadTimer();
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
void VideoSystem::beginFrame(std::span<const uint8_t> fixRom) {
    fixRom_ = fixRom;
}

void VideoSystem::renderLine(uint32_t screenY, std::span<const uint8_t> paletteRam, bool shadow) {
    if (screenY >= kHeight) {
        return;
    }
    lineBuffer_.fill(0);
    // Sprite Y space has 16 hidden lines above the visible area.
    drawSprites(screenY + 16);
    drawFix(screenY);

    // Resolve pens against palette RAM as it is right now, so palette writes
    // made earlier in the frame (raster effects) apply from this line on.
    uint32_t* out = &framebuffer_[size_t{screenY} * kWidth];
    if (paletteRam.size() < size_t{palette::kEntries} * 2) {
        std::fill_n(out, kWidth, 0xFF00'0000u);
        return;
    }
    const auto&    lut      = palette::lookupTable();
    const uint32_t backdrop = palette::entryToArgb(lut, paletteRam, palette::kBackdropEntry);
    for (uint32_t x = 0; x < kWidth; ++x) {
        const uint16_t pen = lineBuffer_[x];
        out[x] = pen ? palette::entryToArgb(lut, paletteRam, pen) : backdrop;
    }
    if (shadow) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            out[x] = palette::shadow(out[x]);
        }
    }
}

void VideoSystem::endFrame() noexcept {
    if (autoAnimFrameCounter_ == 0) {
        autoAnimFrameCounter_ = autoAnimSpeed_;
        ++autoAnimCounter_;
    } else {
        --autoAnimFrameCounter_;
    }
}

// Walks the sprite list in order; later sprites draw over earlier ones.
// Sticky sprites inherit Y, height and V shrink from the chain head, and sit
// immediately right of the previous sprite (whose width is its H shrink + 1).
void VideoSystem::drawSprites(uint32_t spriteSpaceLine) {
    uint32_t y = 0, size = 0, zoomY = 0, x = 0, zoomX = 0;
    uint32_t drawn = 0;

    for (uint32_t sprite = 0; sprite < kMaxSprites; ++sprite) {
        const uint16_t yControl = vram_[kScb3Base + sprite];
        const uint16_t shrink   = vram_[kScb2Base + sprite];

        if (yControl & kStickyBit) {
            x = (x + zoomX + 1) & 0x1FF;
        } else {
            y     = (0x200u - (uint32_t{yControl} >> 7)) & 0x1FF;
            size  = yControl & 0x3Fu;
            x     = uint32_t{vram_[kScb4Base + sprite]} >> 7;
            zoomY = shrink & 0xFFu;
        }
        zoomX = (uint32_t{shrink} >> 8) & 0x0F;

        if (size == 0) {
            continue;
        }
        const uint32_t spriteLine = (spriteSpaceLine - y) & 0x1FF;
        if (size <= 0x20 && spriteLine >= size * 16) {
            continue;  // heights above 32 tiles cover all 512 lines
        }
        if (drawn == kMaxSpritesPerLine) {
            break;
        }
        ++drawn;
        drawSpriteLine(sprite, spriteLine, size, zoomY, x, zoomX);
    }
}

void VideoSystem::drawSpriteLine(uint32_t sprite, uint32_t spriteLine, uint32_t size, uint32_t zoomY,
                                 uint32_t x, uint32_t zoomX) {
    // The L0 table describes the top 256 lines; the bottom 256 are its mirror.
    uint32_t zoomLine = spriteLine & 0xFF;
    bool     invert   = (spriteLine & 0x100) != 0;
    if (invert) {
        zoomLine ^= 0xFF;
    }
    if (size > 0x20) {
        // Oversized sprites wrap at the shrunk height, alternating halves.
        const uint32_t period = (zoomY + 1) << 1;
        zoomLine %= period;
        if (zoomLine > zoomY) {
            zoomLine = period - 1 - zoomLine;
            invert   = !invert;
        }
    }

    const uint8_t entry   = zoomTable_[(zoomY << 8) | zoomLine];
    uint32_t      tileRow = entry & 0x0Fu;
    uint32_t      slot    = uint32_t{entry} >> 4;
    if (invert) {
        tileRow ^= 0x0F;
        slot    ^= 0x1F;
    }

    const uint32_t mapAddr = kScb1Base + (sprite << 6) + (slot << 1);
    const uint16_t attr    = vram_[mapAddr + 1];
    uint32_t code = ((uint32_t{attr} << 12) & 0xF'0000u) | vram_[mapAddr];

    if (!autoAnimDisabled_) {
        if (attr & kAttrAnim8) {
            code = (code & ~7u) | (autoAnimCounter_ & 7u);
        } else if (attr & kAttrAnim4) {
            code = (code & ~3u) | (autoAnimCounter_ & 3u);
        }
    }
    if (attr & kAttrVFlip) {
        tileRow ^= 0x0F;
    }

    code &= tileMask_;
    if (code >= tileCount()) {
        return;
    }
    const uint64_t row = tiles_[size_t{code} * 16 + tileRow];
    if (row == 0) {
        return;  // fully transparent row
    }

    const auto  paletteBase = static_cast<uint16_t>((attr >> 8) << 4);
    const bool  hFlip       = (attr & kAttrHFlip) != 0;
    const auto& keep        = kHorizontalShrink[zoomX];

    for (uint32_t n = 0; n < 16; ++n) {
        if (!keep[n]) {
            continue;
        }
        const uint32_t src   = hFlip ? 15 - n : n;
        const auto     color = static_cast<uint16_t>((row >> (src * 4)) & 0xFu);
        if (color != 0 && x < kWidth) {
            lineBuffer_[x] = static_cast<uint16_t>(paletteBase | color);
        }
        x = (x + 1) & 0x1FF;  // 9-bit X: sprites wrap around from the right edge
    }
}

// Fix map entry: palette (15-12), tile (11-0). The map covers 256 lines; the
// visible 224 are rows 2-29.
void VideoSystem::drawFix(uint32_t screenY) {
    if (fixRom_.empty()) {
        return;
    }
    const uint32_t fy   = screenY + 16;
    const uint32_t row  = fy >> 3;
    const uint32_t line = fy & 7u;

    // Only a cartridge fix ROM larger than 128 KiB is banked (the BIOS SFIX never is).
    const bool banked = fixBanking_ != FixBanking::None && fixRom_.size() > 0x2'0000;
    std::array<uint8_t, 32> garouBanks{};
    if (banked && fixBanking_ == FixBanking::Garou) {
        // A marker pair (0x0200 at 0x7500+k, 0xFFxx at 0x7580+k) switches the
        // bank from that row on; each marker covers two rows (as MAME).
        uint8_t  bank = 0;
        uint32_t y    = 0;
        for (uint32_t k = 0; y < 32; k += 2) {
            if (vram_[0x7500 + k] == 0x0200 && (vram_[0x7580 + k] & 0xFF00) == 0xFF00) {
                bank            = static_cast<uint8_t>(vram_[0x7580 + k] & 3);
                garouBanks[y++] = bank;
            }
            if (y < 32) garouBanks[y++] = bank;
        }
    }

    for (uint32_t col = 0; col < kFixColumns; ++col) {
        const uint16_t entry = vram_[kFixMapBase + col * kFixRows + row];
        uint32_t       code  = entry & 0x0FFFu;
        if (banked) {
            if (fixBanking_ == FixBanking::Garou) {
                code += 0x1000u * (garouBanks[(row - 2) & 31] ^ 3u);
            } else {
                const uint16_t bits = vram_[0x7500 + ((row - 1) & 31) + 32 * (col / 6)];
                code += 0x1000u * (((bits >> ((5 - (col % 6)) * 2)) & 3u) ^ 3u);
            }
        }
        const size_t base = (size_t{code} * 32) & (std::bit_ceil(fixRom_.size()) - 1);
        if (base + 32 > fixRom_.size()) {
            continue;
        }
        const auto paletteBase = static_cast<uint16_t>((entry >> 12) << 4);
        for (uint32_t px = 0; px < 8; ++px) {
            const uint8_t byte  = fixRom_[base + kFixColumnOffset[px >> 1] + line];
            const auto    color = static_cast<uint16_t>((px & 1u) ? (byte >> 4) : (byte & 0x0Fu));
            if (color != 0) {
                lineBuffer_[col * 8 + px] = static_cast<uint16_t>(paletteBase | color);
            }
        }
    }
}

}  // namespace snk
