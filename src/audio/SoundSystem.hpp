#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "audio/AudioTypes.hpp"
#include "audio/Ym2610.hpp"
#include "cpu/z80/Z80.hpp"

namespace snk {

// Z80-side ROMs.
enum class SoundRom : uint8_t {
    BiosM,   // board sound BIOS (sm1.sm1), used while REG_BRDFIX is selected
    M1,      // cartridge Z80 program (xxx-m1.m1), used while REG_CRTFIX is selected
    AdpcmA,  // V ROMs (V1, V2, ... at increasing offsets)
    AdpcmB,  // separate ADPCM-B ROMs on some boards; falls back to AdpcmA if empty
};

// The Neo-Geo sound subsystem: the Z80, its memory/IO map, the 68000<->Z80
// communication latches, and the YM2610. No SDL: it produces 16-bit stereo
// frames at the YM2610's native rate (~55.5 kHz) and the frontend resamples.
//
// Z80 memory map:
//   0x0000-0x7FFF  M1 / SM1, fixed
//   0x8000-0xBFFF  16 KiB bank  (select: IN from port 0x0B, bank = port >> 8)
//   0xC000-0xDFFF   8 KiB bank  (port 0x0A)
//   0xE000-0xEFFF   4 KiB bank  (port 0x09)
//   0xF000-0xF7FF   2 KiB bank  (port 0x08)
//   0xF800-0xFFFF  2 KiB work RAM
// Z80 I/O map (low 4 bits decoded, mirrored):
//   IN  0x00       read the 68000's command (clears the pending NMI)
//   OUT 0x00       clear the command latch
//   I/O 0x04-0x07  YM2610
//   OUT 0x08/0x18  enable / disable NMI on new commands
//   IN  0x08-0x0B  ROM bank select (see above)
//   OUT 0x0C       reply to the 68000
// Publicly a Z80Bus: that is the Z80's view of the board, which debuggers and
// tests can drive directly.
class SoundSystem final : public Z80Bus {
public:
    SoundSystem();

    SoundSystem(const SoundSystem&)            = delete;
    SoundSystem& operator=(const SoundSystem&) = delete;

    void reset();

    // ---- ROMs (throws RomLoadError) ----------------------------------------
    void loadRom(SoundRom rom, std::span<const uint8_t> data, uint32_t offset = 0);
    void loadRomFile(SoundRom rom, const std::filesystem::path& path, uint32_t offset = 0);
    void unloadRom(SoundRom rom);

    // ---- 68000 side (via MemoryBus) ----------------------------------------
    void writeCommand(uint8_t command);                            // write 0x320000
    [[nodiscard]] uint8_t reply() const noexcept { return reply_; }  // read  0x320000
    void selectCartridgeRom(bool cartridge) noexcept { cartridgeRom_ = cartridge; }

    // ---- Scheduling / output (driven by System) -----------------------------
    void beginFrame() noexcept { samples_.clear(); }
    void runScanline();
    // Samples generated since beginFrame(), at hw::kYm2610SampleRateExact.
    [[nodiscard]] std::span<const StereoFrame> frameSamples() const noexcept { return samples_; }

    [[nodiscard]] const Z80&    cpu() const noexcept { return cpu_; }
    [[nodiscard]] const Ym2610& chip() const noexcept { return ym_; }
    [[nodiscard]] uint8_t pendingCommand() const noexcept { return command_; }

private:
    // Z80Bus
    uint8_t read(uint16_t address) override;
    void    write(uint16_t address, uint8_t value) override;
    uint8_t in(uint16_t port) override;
    void    out(uint16_t port, uint8_t value) override;

    void updateNmi() noexcept { cpu_.setNmiLine(nmiEnabled_ && nmiPending_); }
    void updateAdpcmRoms() noexcept;
    [[nodiscard]] const std::vector<uint8_t>& activeProgramRom() const noexcept {
        return cartridgeRom_ ? m1_ : sm1_;
    }

    Z80    cpu_;
    Ym2610 ym_;

    std::array<uint8_t, 0x800> ram_{};
    std::vector<uint8_t>       sm1_, m1_, adpcmA_, adpcmB_;
    std::array<uint8_t, 4>     banks_{};  // index = port & 3 (0x08..0x0B)

    bool    cartridgeRom_ = false;
    bool    nmiEnabled_   = false;
    bool    nmiPending_   = false;
    uint8_t command_      = 0;  // 68000 -> Z80
    uint8_t reply_        = 0;  // Z80 -> 68000

    int64_t                  cycleBalance_   = 0;  // Z80 cycles owed (see System)
    uint32_t                 ymClockBalance_ = 0;  // YM2610 clocks not yet turned into samples
    std::vector<StereoFrame> samples_;
};

}  // namespace snk
