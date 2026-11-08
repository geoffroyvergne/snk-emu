#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "cart/CartridgeChip.hpp"

namespace snk {

// NEO-PVC (Metal Slug 5, SNK vs. Capcom, KOF 2003).
//
// Load time: the P ROMs are XOR/bit/block scrambled (decrypt* below).
// Run time: 8 KiB of cartridge RAM at 0x2FE000-0x2FFFFF, overlaying the top
// of the banked ROM window, with two helpers the game relies on: palette
// colour unpack/pack registers (0x2FFFE0-0x2FFFEB) and a bank switch to any
// byte address (0x2FFFF0-0x2FFFF3).
//
// Ported from MAME (src/devices/bus/neogeo/prot_pvc.cpp, BSD-3-Clause,
// S. Smith, David Haywood, Fabio Priuli).
class PvcProtection final : public CartridgeChip {
public:
    [[nodiscard]] const char* name() const noexcept override { return "NEO-PVC protection"; }
    [[nodiscard]] uint32_t firstAddress() const noexcept override { return 0x2F'E000; }
    [[nodiscard]] uint32_t lastAddress() const noexcept override { return 0x2F'FFFF; }

    void reset() noexcept override {}  // RAM keeps its contents across resets, as in MAME
    [[nodiscard]] uint16_t read16(uint32_t address, const MemoryBus& bus) override;
    void write16(uint32_t address, uint16_t value, uint16_t mask, MemoryBus& bus) override;

    // 68000 program decryption, on the image in 68000 byte order (P1/P2
    // interleaved word by word, P3 at 0x800000 for kof2003).
    static void decryptMslug5(std::span<uint8_t> rom);
    static void decryptSvc(std::span<uint8_t> rom);
    static void decryptKof2003(std::span<uint8_t> rom);

private:
    void unpackColor() noexcept;
    void packColor() noexcept;

    std::array<uint16_t, 0x1000> ram_{};
};

}  // namespace snk
