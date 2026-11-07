#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "core/Endian.hpp"
#include "core/RomFile.hpp"

namespace snk {

class InputPorts;
class SoundSystem;
class Upd4990a;
class InterruptController;
class VideoSystem;

// ROM images the bus knows how to load.
enum class RomType : uint8_t {
    Bios,   // 68000 system ROM (sp-s2.sp1, uni-bios, ...)  -> 0xC00000, 16-bit
    SFix,   // Board fix-layer tiles (sfix.sfix), used while REG_BRDFIX is selected
    PRom,   // Cartridge program: 1st MiB -> 0x000000, the rest banked at 0x200000, 16-bit
    SRom,   // Cartridge fix-layer tiles (xxx-s1.s1), used while REG_CRTFIX is selected
};

// How the 16-bit words are laid out in the file.
enum class RomByteOrder : uint8_t {
    Auto,         // P ROM: detect via the "NEO-GEO" header; BIOS: WordSwapped; 8-bit ROMs: as-is
    BigEndian,    // already in 68000 byte order
    WordSwapped,  // bytes of every 16-bit word swapped (the usual layout of .p1 / .sp1 dumps)
};

// Unified 68000 memory bus (24-bit address space).
//
//   0x000000-0x0FFFFF  P ROM, first MiB  (0x000000-0x00007F = BIOS vectors while REG_SWPBIOS)
//   0x100000-0x1FFFFF  Work RAM, 64 KiB, mirrored
//   0x200000-0x2FFFFF  P ROM, banked 1 MiB window (bank select: write to 0x2FFFF0-0x2FFFFF)
//   0x300000-0x3FFFFF  I/O: inputs, Z80 latch, system latch (0x3A00xx), LSPC (0x3C00xx)
//   0x400000-0x7FFFFF  Palette RAM, 2 banks x 8 KiB, mirrored every 8 KiB
//   0x800000-0xBFFFFF  Memory card (not implemented)
//   0xC00000-0xCFFFFF  BIOS ROM, mirrored
//   0xD00000-0xDFFFFF  Backup RAM (MVS), 64 KiB, write-protected unless REG_SRAMUNLOCK
//
// Dispatch uses a 256-entry page table (one entry per 64 KiB). RAM/ROM pages
// hold a direct pointer plus a mirror mask, so the common case is one table
// lookup and one load; only I/O and register pages fall through to the
// out-of-line slow path. All memory is stored in big-endian byte order, exactly
// as the 68000 sees it; 16/32-bit accesses go through snk::endian helpers.
class MemoryBus {
public:
    static constexpr uint32_t kAddressMask = 0x00FF'FFFF;
    static constexpr uint8_t  kOpenBus     = 0xFF;

    static constexpr uint32_t kWorkRamSize    = 0x1'0000;
    static constexpr uint32_t kBackupRamSize  = 0x1'0000;
    static constexpr uint32_t kPaletteBankSize = 0x2000;
    static constexpr uint32_t kVectorTableSize = 0x80;

    MemoryBus();

    MemoryBus(const MemoryBus&)            = delete;
    MemoryBus& operator=(const MemoryBus&) = delete;

    // ---- ROM loading -------------------------------------------------------
    // `offset` places the image inside the ROM region, so multi-file sets can be
    // assembled (e.g. P1 at 0x000000, P2 at 0x100000). Throws RomLoadError.
    // Call reset() after loading so the CPU fetches the new reset vectors.
    void loadRom(RomType type, std::span<const uint8_t> data, uint32_t offset = 0,
                 RomByteOrder order = RomByteOrder::Auto);
    void loadRomFile(RomType type, const std::filesystem::path& path, uint32_t offset = 0,
                     RomByteOrder order = RomByteOrder::Auto);
    // Discards a ROM region (e.g. before loading another game; loads merge otherwise).
    void unloadRom(RomType type);

    // Clears work/palette RAM and the system latch; keeps ROMs and backup RAM.
    void reset();

    // ---- 68000 accesses ----------------------------------------------------
    // Reads are non-const: I/O reads can have side effects. Word/long accesses
    // must be even; raising the address error on odd ones is the CPU's job.
    [[nodiscard]] uint8_t  read8(uint32_t address);
    [[nodiscard]] uint16_t read16(uint32_t address);
    [[nodiscard]] uint32_t read32(uint32_t address);
    void write8(uint32_t address, uint8_t value);
    void write16(uint32_t address, uint16_t value);
    void write32(uint32_t address, uint32_t value);

    // Side-effect-free reads for debuggers/disassemblers: RAM and ROM only,
    // I/O registers read as open bus.
    [[nodiscard]] uint8_t  peek8(uint32_t address) const noexcept;
    [[nodiscard]] uint16_t peek16(uint32_t address) const noexcept;

    // REG_IRQACK (0x3C000C) writes are forwarded here. Not owned; may be null.
    void connectInterrupts(InterruptController* irq) noexcept { irq_ = irq; }
    // LSPC registers (0x3C0000-0x3DFFFF) are forwarded here. Not owned; may be null.
    void connectVideo(VideoSystem* video) noexcept { video_ = video; }
    // Input registers (0x300000, 0x320001, 0x340000, 0x380000) read from here.
    void connectInputs(const InputPorts* inputs) noexcept { inputs_ = inputs; }
    // 0x320000 (Z80 command/reply) and the SM1/M1 select are forwarded here.
    void connectSound(SoundSystem* sound) noexcept { sound_ = sound; }
    // MVS calendar chip: written through 0x380051, read on REG_STATUS_A bits 6-7.
    void connectRtc(Upd4990a* rtc) noexcept { rtc_ = rtc; }

    // ---- Views for other chips --------------------------------------------
    [[nodiscard]] std::span<const uint8_t> fixRom() const noexcept;     // SFIX or S ROM, per latch
    [[nodiscard]] std::span<const uint8_t> paletteRam() const noexcept; // active 8 KiB bank

    // Battery-backed RAM at 0xD00000 (64 KiB, 68000 big-endian byte order).
    // Survives reset(); frontends persist it between sessions. The BIOS skips
    // its destructive backup-RAM test when the contents are already valid.
    [[nodiscard]] std::span<uint8_t> backupRam() noexcept { return backupRam_; }
    [[nodiscard]] std::span<const uint8_t> backupRam() const noexcept { return backupRam_; }
    [[nodiscard]] bool biosVectorsMapped() const noexcept { return biosVectors_; }
    [[nodiscard]] bool shadowEnabled() const noexcept { return shadow_; }
    [[nodiscard]] uint8_t programBank() const noexcept { return programBank_; }
    // Diagnostics: address of the most recent slow-path (I/O or unmapped)
    // read (kNoIoRead if none). When the 68000 hangs, this is usually the
    // register it polls.
    static constexpr uint32_t kNoIoRead = 0xFFFF'FFFF;
    [[nodiscard]] uint32_t lastIoRead() const noexcept { return lastIoRead_; }

private:
    struct Page {
        const uint8_t* read  = nullptr;  // nullptr -> slow path
        uint8_t*       write = nullptr;  // nullptr -> slow path (ROM, I/O, locked SRAM)
        uint32_t       mask  = 0;        // offset = address & mask (handles mirroring)
    };

    struct RomImage {
        std::vector<uint8_t> data;  // padded with 0xFF for clean mirroring/banking
        size_t               used = 0;
    };

    RomImage&       image(RomType type) noexcept;
    const RomImage& image(RomType type) const noexcept;

    void mapAll();
    void mapRange(uint32_t firstPage, uint32_t lastPage, const uint8_t* read, uint8_t* write,
                  uint32_t mask) noexcept;
    void mapVectors();
    void mapBankedProgram() noexcept;
    void mapPalette() noexcept;
    void mapBackupRam() noexcept;

    uint8_t readSlow8(uint32_t address);
    uint16_t readSlow16(uint32_t address);
    void writeSlow8(uint32_t address, uint8_t value);
    void writeSlow16(uint32_t address, uint16_t value);
    void writeSystemLatch(uint32_t address);
    void writeLspc(uint32_t address, uint16_t value);
    void setCartridgeFix(bool cartridge) noexcept;
    void writeIoControl(uint8_t offset, uint8_t value);

    std::array<Page, 256> pages_{};
    InterruptController*  irq_    = nullptr;
    VideoSystem*          video_  = nullptr;
    const InputPorts*     inputs_ = nullptr;
    SoundSystem*          sound_  = nullptr;
    Upd4990a*             rtc_    = nullptr;

    // Volatile memory.
    std::array<uint8_t, kWorkRamSize>          workRam_{};
    std::array<uint8_t, 2 * kPaletteBankSize>  paletteRam_{};
    std::array<uint8_t, kBackupRamSize>        backupRam_{};  // battery-backed: survives reset()
    std::array<uint8_t, 0x1'0000>              vectorPage_{}; // page 0 view while BIOS vectors are mapped

    // ROMs.
    RomImage bios_, sFix_, pRom_, sRom_;

    // System latch state (0x3A00xx).
    bool    biosVectors_ = true;   // REG_SWPBIOS / REG_SWPROM
    bool    cartFix_     = false;  // REG_CRTFIX / REG_BRDFIX
    bool    sramLocked_  = true;   // REG_SRAMLOCK / REG_SRAMUNLOCK
    bool    shadow_      = false;  // REG_SHADOW / REG_NOSHADOW
    uint8_t paletteBank_ = 0;      // REG_PALBANK0 / REG_PALBANK1
    uint8_t programBank_ = 0;      // P ROM bank at 0x200000
    uint32_t lastIoRead_ = kNoIoRead;
};

// ---------------------------------------------------------------------------
// Hot path, inline: one table lookup + one (byte-swapping) load/store.
// ---------------------------------------------------------------------------
inline uint8_t MemoryBus::read8(uint32_t address) {
    address &= kAddressMask;
    const Page& page = pages_[address >> 16];
    if (page.read) [[likely]] {
        return page.read[address & page.mask];
    }
    return readSlow8(address);
}

inline uint16_t MemoryBus::read16(uint32_t address) {
    address &= kAddressMask & ~1u;
    const Page& page = pages_[address >> 16];
    if (page.read) [[likely]] {
        return endian::read_be16(page.read + (address & page.mask));
    }
    return readSlow16(address);
}

// The 68000 performs long accesses as two word bus cycles, high word first.
inline uint32_t MemoryBus::read32(uint32_t address) {
    const uint32_t hi = read16(address);
    return (hi << 16) | read16(address + 2);
}

inline uint8_t MemoryBus::peek8(uint32_t address) const noexcept {
    address &= kAddressMask;
    const Page& page = pages_[address >> 16];
    return page.read ? page.read[address & page.mask] : kOpenBus;
}

inline uint16_t MemoryBus::peek16(uint32_t address) const noexcept {
    address &= kAddressMask & ~1u;
    const Page& page = pages_[address >> 16];
    return page.read ? endian::read_be16(page.read + (address & page.mask)) : uint16_t{0xFFFF};
}

inline void MemoryBus::write8(uint32_t address, uint8_t value) {
    address &= kAddressMask;
    const Page& page = pages_[address >> 16];
    if (page.write) [[likely]] {
        page.write[address & page.mask] = value;
        return;
    }
    writeSlow8(address, value);
}

inline void MemoryBus::write16(uint32_t address, uint16_t value) {
    address &= kAddressMask & ~1u;
    const Page& page = pages_[address >> 16];
    if (page.write) [[likely]] {
        endian::write_be16(page.write + (address & page.mask), value);
        return;
    }
    writeSlow16(address, value);
}

inline void MemoryBus::write32(uint32_t address, uint32_t value) {
    write16(address, static_cast<uint16_t>(value >> 16));
    write16(address + 2, static_cast<uint16_t>(value));
}

}  // namespace snk
