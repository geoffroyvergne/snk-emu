#pragma once

#include <cstdint>
#include <span>

#include "cart/CartridgeChip.hpp"

namespace snk {

// NEO-SMA, Metal Slug 3 variant. The chip holds part of the program (the
// "neo-sma" dump, placed at 0x0C0000) and decrypts the P ROMs on the fly.
//
// Run time: an ID word at 0x2FE446 (0x9A37) and a bank select at 0x2FFFE4
// whose value is bit-scrambled and maps to an irregular table of offsets.
// The standard bank select at 0x2FFFF0 keeps working.
//
// Ported from MAME (src/devices/bus/neogeo/prot_sma.cpp, BSD-3-Clause,
// S. Smith, David Haywood, Fabio Priuli).
class SmaMslug3Protection final : public CartridgeChip {
public:
    [[nodiscard]] const char* name() const noexcept override { return "NEO-SMA protection (Metal Slug 3)"; }
    [[nodiscard]] uint32_t firstAddress() const noexcept override { return kIdAddress; }
    [[nodiscard]] uint32_t lastAddress() const noexcept override { return kBankAddress + 1; }
    [[nodiscard]] bool decodesRead(uint32_t address) const noexcept override { return (address & ~1u) == kIdAddress; }
    [[nodiscard]] bool decodesWrite(uint32_t address) const noexcept override { return (address & ~1u) == kBankAddress; }

    void reset() noexcept override {}
    [[nodiscard]] uint16_t read16(uint32_t /*address*/, const MemoryBus& /*bus*/) override { return 0x9A37; }
    void write16(uint32_t address, uint16_t value, uint16_t mask, MemoryBus& bus) override;

    // Program image (9 MiB, 68000 byte order): SMA code at 0x0C0000, P1 at
    // 0x100000, P2 at 0x500000. Decrypted in place.
    static void decrypt(std::span<uint8_t> image);

private:
    static constexpr uint32_t kIdAddress   = 0x2F'E446;
    static constexpr uint32_t kBankAddress = 0x2F'FFE4;
};

}  // namespace snk
