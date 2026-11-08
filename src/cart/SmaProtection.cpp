// NEO-SMA (Metal Slug 3), ported from MAME (src/devices/bus/neogeo/prot_sma.cpp).
// license:BSD-3-Clause, copyright-holders: S. Smith, David Haywood, Fabio Priuli
#include "cart/SmaProtection.hpp"

#include <array>
#include <stdexcept>
#include <vector>

#include "cart/BitSwap.hpp"
#include "core/Endian.hpp"
#include "memory/MemoryBus.hpp"

namespace snk {

using cart::bitswap;

void SmaMslug3Protection::write16(uint32_t /*address*/, uint16_t value, uint16_t /*mask*/, MemoryBus& bus) {
    static constexpr std::array<uint32_t, 64> kBankOffset{
        0x000000, 0x020000, 0x040000, 0x060000, 0x070000, 0x090000, 0x0b0000, 0x0d0000,
        0x0e0000, 0x0f0000, 0x120000, 0x130000, 0x140000, 0x150000, 0x180000, 0x190000,
        0x1a0000, 0x1b0000, 0x1e0000, 0x1f0000, 0x200000, 0x210000, 0x240000, 0x250000,
        0x260000, 0x270000, 0x2a0000, 0x2b0000, 0x2c0000, 0x2d0000, 0x300000, 0x310000,
        0x320000, 0x330000, 0x360000, 0x370000, 0x380000, 0x390000, 0x3c0000, 0x3d0000,
        0x400000, 0x410000, 0x440000, 0x450000, 0x460000, 0x470000, 0x4a0000, 0x4b0000,
        0x4c0000,  // the rest is unused (0)
    };
    const uint32_t bank = bitswap<6>(value, {9, 3, 6, 15, 12, 14});
    bus.setProgramBankBase(0x10'0000 + kBankOffset[bank]);
}

void SmaMslug3Protection::decrypt(std::span<uint8_t> image) {
    if (image.size() < 0x90'0000) throw std::invalid_argument("SMA: program image must be 9 MiB");

    // MAME works on 16-bit words as the 68000 sees them.
    std::vector<uint16_t> rom(image.size() / 2);
    for (size_t i = 0; i < rom.size(); ++i) rom[i] = endian::read_be16(&image[2 * i]);
    uint16_t* banked = rom.data() + 0x10'0000 / 2;

    // Data lines of the whole P ROM area.
    for (uint32_t i = 0; i < 0x80'0000 / 2; ++i) {
        banked[i] = static_cast<uint16_t>(bitswap<16>(banked[i], {4, 11, 14, 3, 1, 13, 0, 7, 2, 8, 12, 15, 10, 9, 5, 6}));
    }
    // Address lines, and the fixed part relocated from the P ROMs.
    for (uint32_t i = 0; i < 0x0C'0000 / 2; ++i) {
        rom[i] = rom[0x5D'0000 / 2 +
                     bitswap<19>(i, {18, 15, 2, 1, 13, 3, 0, 9, 6, 16, 4, 11, 5, 7, 12, 17, 14, 10, 8})];
    }
    // Address lines of the banked part, per 64 KiB block.
    std::vector<uint16_t> block(0x1'0000 / 2);
    for (uint32_t i = 0; i < 0x80'0000 / 2; i += 0x1'0000 / 2) {
        std::copy_n(banked + i, block.size(), block.begin());
        for (uint32_t j = 0; j < 0x1'0000 / 2; ++j) {
            banked[i + j] = block[bitswap<15>(j, {2, 11, 0, 14, 6, 4, 13, 8, 9, 3, 10, 7, 5, 12, 1})];
        }
    }

    for (size_t i = 0; i < rom.size(); ++i) endian::write_be16(&image[2 * i], rom[i]);
}

}  // namespace snk
