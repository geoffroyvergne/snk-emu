#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <vector>

#include "core/Hardware.hpp"

namespace snk {

// Neo-Geo video: the LSPC (Line SPrite Controller) plus the sprite tile
// pipeline fed by the C ROMs.
//
// There are no background layers. Everything is a sprite: a vertical strip
// 16 pixels wide and 1-32 tiles (16..512 lines) tall, optionally chained to
// the previous sprite ("sticky") to build wide objects. On top sits the fix
// layer, a static 40x32 grid of 8x8 tiles from the S ROM / SFIX.
//
// Rendering is line-based like the hardware: each scanline, up to 381 sprites
// are scanned and up to 96 are drawn into a line buffer of palette indices,
// the fix layer is drawn over them, and the line is resolved to ARGB8888
// against live palette RAM (see Palette.hpp). Tile format: TileCodec.hpp.
// No SDL here: the frontend uploads framebuffer() however it likes.
class VideoSystem {
public:
    static constexpr uint32_t kWidth  = hw::kScreenWidth;
    static constexpr uint32_t kHeight = hw::kScreenHeight;

    // ---- VRAM (word addresses, exactly as written to REG_VRAMADDR) ---------
    // Slow VRAM, 32 Ki words:
    static constexpr uint16_t kScb1Base   = 0x0000;  // sprite tile maps: 64 words/sprite (32 tiles x 2)
    static constexpr uint16_t kFixMapBase = 0x7000;  // fix map: 40 columns x 32 rows, column-major
    // Fast VRAM, 2 Ki words:
    static constexpr uint16_t kScb2Base   = 0x8000;  // per sprite: H shrink (11-8), V shrink (7-0)
    static constexpr uint16_t kScb3Base   = 0x8200;  // per sprite: Y (15-7), sticky (6), height in tiles (5-0)
    static constexpr uint16_t kScb4Base   = 0x8400;  // per sprite: X (15-7)

    static constexpr uint32_t kSlowVramWords = 0x8000;
    static constexpr uint32_t kFastVramWords = 0x0800;
    static constexpr uint32_t kVramWords     = kSlowVramWords + kFastVramWords;  // 68 KiB

    static constexpr uint16_t kStickyBit         = 0x0040;
    static constexpr uint32_t kMaxSprites        = 381;
    static constexpr uint32_t kMaxSpritesPerLine = 96;
    static constexpr uint32_t kFixColumns        = 40;
    static constexpr uint32_t kFixRows           = 32;

    // SCB1 odd word (tile attributes).
    static constexpr uint16_t kAttrHFlip     = 0x0001;
    static constexpr uint16_t kAttrVFlip     = 0x0002;
    static constexpr uint16_t kAttrAnim4     = 0x0004;  // 2-bit auto-animation
    static constexpr uint16_t kAttrAnim8     = 0x0008;  // 3-bit auto-animation

    // REG_LSPCMODE bits 7-4: LSPC timer control (kept as timerMode_ bits 3-0).
    static constexpr uint8_t kTimerIrqEnable      = 0x1;  // raise IRQ 2 when the counter expires
    static constexpr uint8_t kTimerReloadOnWrite  = 0x2;  // (re)start the counter on REG_TIMERLOW writes
    static constexpr uint8_t kTimerReloadAtVBlank = 0x4;  // (re)start it when VBlank begins
    static constexpr uint8_t kTimerRepeat         = 0x8;  // restart it every time it expires

    // LSPC registers: offsets from 0x3C0000, mirrored every 16 bytes.
    static constexpr uint32_t kRegVramAddr  = 0x0;
    static constexpr uint32_t kRegVramRw    = 0x2;
    static constexpr uint32_t kRegVramMod   = 0x4;
    static constexpr uint32_t kRegLspcMode  = 0x6;
    static constexpr uint32_t kRegTimerHigh = 0x8;
    static constexpr uint32_t kRegTimerLow  = 0xA;
    static constexpr uint32_t kRegIrqAck    = 0xC;  // handled by MemoryBus -> InterruptController
    static constexpr uint32_t kRegTimerStop = 0xE;

    // S ROM tile layout: an 8x8 tile is 32 bytes = 4 columns of 8 bytes, each
    // byte holding 2 pixels (low nibble = left). Column order in the ROM is
    // pixels 4-5, 6-7, 0-1, 2-3; this table gives each pixel pair's offset.
    static constexpr std::array<uint8_t, 4> kFixColumnOffset = {0x10, 0x18, 0x00, 0x08};

    VideoSystem();

    VideoSystem(const VideoSystem&)            = delete;
    VideoSystem& operator=(const VideoSystem&) = delete;

    // Clears VRAM and LSPC registers; keeps loaded tile data.
    void reset();

    // ---- Tile ROMs ---------------------------------------------------------
    // C ROMs come in pairs (C1/C2, C3/C4, ...): the odd ROM holds bitplanes
    // 0-1, the even ROM bitplanes 2-3. Each call appends the pair's tiles.
    // Tiles are decoded once to 4 bpp rows (nibble x = pixel x), the same
    // size as the source, so drawing never touches planar data. Throws RomLoadError.
    void addSpriteRomPair(std::span<const uint8_t> cOdd, std::span<const uint8_t> cEven);
    void addSpriteRomPairFiles(const std::filesystem::path& cOdd, const std::filesystem::path& cEven);
    void clearSpriteRoms() noexcept;
    [[nodiscard]] size_t tileCount() const noexcept { return tiles_.size() / 16; }

    // Optional: the vertical-shrink lookup ROM (000-lo.lo, first 64 KiB used).
    // Without it a linear approximation is used.
    void loadZoomRom(std::span<const uint8_t> data);

    // ---- 68000 side (via MemoryBus, 0x3C0000-0x3DFFFF) ---------------------
    [[nodiscard]] uint16_t readRegister(uint32_t offset) const noexcept;
    void writeRegister(uint32_t offset, uint16_t value) noexcept;

    // Direct VRAM access for tools, tests and save states (no auto-increment).
    [[nodiscard]] uint16_t vram(uint16_t address) const noexcept { return vram_[vramIndex(address)]; }
    void setVram(uint16_t address, uint16_t value) noexcept { vram_[vramIndex(address)] = value; }

    // ---- LSPC timer (the Neo-Geo's raster interrupt, IRQ 2) -------------------
    // A 32-bit counter clocked by the 6 MHz pixel clock: loading it with the
    // reload value R makes it expire R + 1 pixels later. Time is tracked in
    // 68000 cycles (2 per pixel) so System can split CPU slices exactly there.
    static constexpr uint64_t kTimerStopped = UINT64_MAX;
    [[nodiscard]] uint64_t cyclesUntilTimer() const noexcept { return timerRunning_ ? timerCyclesLeft_ : kTimerStopped; }
    // Advances the counter; true if it expired and IRQ 2 is enabled.
    bool advanceTimer(uint64_t m68kCycles) noexcept;
    void onVBlankStart() noexcept;  // REG_LSPCMODE "reload at VBlank"
    // Called just BEFORE a write that affects the timer is applied, so the
    // owner can bring the counter up to the current CPU cycle, and stop the
    // current CPU slice so the next expiry is recomputed.
    void setTimerSyncHandler(std::function<void()> handler) { timerSync_ = std::move(handler); }

    // ---- Rendering, driven by System ----------------------------------------
    void setRasterLine(uint32_t frameLine) noexcept { rasterLine_ = frameLine; }  // 0..263
    // Cartridges with more than 128 KiB of fix tiles pick a 4 KiB-tile bank
    // per row, from tables the game writes in VRAM (layout set per cartridge).
    enum class FixBanking : uint8_t {
        None,
        Garou,    // Garou, Metal Slug 3/4/5: one bank per row, set by marker words at 0x7500/0x7580
        Kof2000,  // KOF 2000/2003, SVC: 2 bits per tile in 0x7500-0x75FF
    };
    void setFixBanking(FixBanking banking) noexcept { fixBanking_ = banking; }

    // fixRom: SFIX or S ROM, whichever the system latch selects.
    void beginFrame(std::span<const uint8_t> fixRom);
    // paletteRam: the active 8 KiB bank (big-endian words), read live.
    void renderLine(uint32_t screenY, std::span<const uint8_t> paletteRam, bool shadow);  // screenY 0..223
    void endFrame() noexcept;

    [[nodiscard]] std::span<const uint32_t> framebuffer() const noexcept { return framebuffer_; }
    [[nodiscard]] uint8_t autoAnimationCounter() const noexcept { return autoAnimCounter_; }

    // One decoded 16-pixel tile row: pixel x in bits 4x..4x+3.
    [[nodiscard]] uint64_t spriteTileRow(uint32_t tile, uint32_t row) const noexcept;

private:
    // 0x0000-0x7FFF -> slow VRAM; 0x8000-0xFFFF -> fast VRAM (2 Ki words, mirrored).
    static constexpr uint32_t vramIndex(uint16_t address) noexcept {
        return (address & 0x8000u) ? kSlowVramWords + (address & (kFastVramWords - 1))
                                   : address & (kSlowVramWords - 1);
    }

    void drawSprites(uint32_t spriteSpaceLine);
    void drawSpriteLine(uint32_t sprite, uint32_t spriteLine, uint32_t size, uint32_t zoomY,
                        uint32_t x, uint32_t zoomX);
    void drawFix(uint32_t screenY);

    // VRAM + LSPC registers.
    std::array<uint16_t, kVramWords> vram_{};
    uint16_t vramAddr_ = 0;
    uint16_t vramMod_  = 0;  // signed in hardware; two's-complement add gives the same result
    uint8_t  autoAnimSpeed_        = 0;  // frames between animation steps, minus one
    uint8_t  autoAnimFrameCounter_ = 0;
    uint8_t  autoAnimCounter_      = 0;
    bool     autoAnimDisabled_     = false;
    void loadTimer() noexcept;

    uint8_t  timerMode_       = 0;  // kTimer* bits
    uint32_t timerReload_     = 0;
    bool     timerStopPal_    = false;  // PAL-only: pause during borders (ignored, NTSC timing)
    bool     timerRunning_    = false;
    uint64_t timerCyclesLeft_ = 0;
    std::function<void()> timerSync_;
    uint32_t rasterLine_   = 0;

    // Tile data.
    std::vector<uint64_t>         tiles_;   // 16 rows per tile
    uint32_t                      tileMask_ = 0;
    std::array<uint8_t, 0x10000>  zoomTable_{};  // [vShrink][line] -> tile slot (7-4), tile row (3-0)

    // Per-frame state.
    std::span<const uint8_t>      fixRom_;
    FixBanking                    fixBanking_ = FixBanking::None;
    std::array<uint16_t, kWidth>  lineBuffer_{}; // palette index per pixel, 0 = transparent
    std::array<uint32_t, kWidth * kHeight> framebuffer_{};
};

}  // namespace snk
