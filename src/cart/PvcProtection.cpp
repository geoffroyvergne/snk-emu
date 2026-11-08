// NEO-PVC protection, ported from MAME (src/devices/bus/neogeo/prot_pvc.cpp).
// license:BSD-3-Clause, copyright-holders: S. Smith, David Haywood, Fabio Priuli
#include "cart/PvcProtection.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "cart/BitSwap.hpp"
#include "memory/MemoryBus.hpp"

namespace snk {

using cart::bitswap;

uint16_t PvcProtection::read16(uint32_t address, const MemoryBus& /*bus*/) {
    return ram_[(address - firstAddress()) >> 1];
}

void PvcProtection::write16(uint32_t address, uint16_t value, uint16_t mask, MemoryBus& bus) {
    const uint32_t offset = (address - firstAddress()) >> 1;
    ram_[offset] = static_cast<uint16_t>((ram_[offset] & ~mask) | (value & mask));
    if (offset == 0xFF0) {
        unpackColor();
    } else if (offset == 0xFF4 || offset == 0xFF5) {
        packColor();
    } else if (offset >= 0xFF8) {
        // Bank switch: the window at 0x200000 shows the P ROM from any byte address.
        const uint32_t bank = (ram_[0xFF8] >> 8) | (uint32_t{ram_[0xFF9]} << 8);
        ram_[0xFF8]         = static_cast<uint16_t>((ram_[0xFF8] & 0xFE00) | 0x00A0);
        ram_[0xFF9] &= 0x7FFF;
        bus.setProgramBankBase(bank + 0x10'0000);
    }
}

// Neo-Geo colour word -> separate 5-bit components (+ dark bit).
void PvcProtection::unpackColor() noexcept {
    const uint16_t pen = ram_[0xFF0];
    const auto b = static_cast<uint8_t>(((pen & 0x000F) << 1) | ((pen & 0x1000) >> 12));
    const auto g = static_cast<uint8_t>(((pen & 0x00F0) >> 3) | ((pen & 0x2000) >> 13));
    const auto r = static_cast<uint8_t>(((pen & 0x0F00) >> 7) | ((pen & 0x4000) >> 14));
    const auto s = static_cast<uint8_t>((pen & 0x8000) >> 15);
    ram_[0xFF1]  = static_cast<uint16_t>((g << 8) | b);
    ram_[0xFF2]  = static_cast<uint16_t>((s << 8) | r);
}

void PvcProtection::packColor() noexcept {
    const uint16_t gb = ram_[0xFF4];
    const uint16_t sr = ram_[0xFF5];
    ram_[0xFF6] = static_cast<uint16_t>(((gb & 0x001E) >> 1) | ((gb & 0x1E00) >> 5) | ((sr & 0x001E) << 7) |
                                        ((gb & 0x0001) << 12) | ((gb & 0x0100) << 5) | ((sr & 0x0001) << 14) |
                                        ((sr & 0x0100) << 7));
}

// ---------------------------------------------------------------------------
// Program decryption, on the image in 68000 byte order. The index flips
// (i ^ 1, MAME's BYTE_XOR_LE) are kept exactly as MAME writes them.
// ---------------------------------------------------------------------------
namespace {

constexpr uint32_t le(uint32_t i) noexcept { return i ^ 1u; }

struct PvcKeys {
    std::array<uint8_t, 0x20> xor1;
    std::array<uint8_t, 0x20> xor2;
    std::array<int, 16>       wordSwap;    // bits of each 16-bit word in the banked area
    std::array<int, 8>        fixedBlocks; // 64 KiB block order inside the first MiB
    uint32_t                  pageXor;     // 256-byte page XOR inside 4 KiB blocks
    std::array<int, 8>        bankBlocks;  // 4 KiB block order inside each MiB
};

// mslug5 / svc: 8 MiB region, the last MiB becomes the first bank.
void decrypt8M(std::span<uint8_t> rom, const PvcKeys& k) {
    constexpr uint32_t kSize = 0x80'0000;
    if (rom.size() < kSize) throw std::invalid_argument("PVC: program region must be 8 MiB");

    for (uint32_t i = 0; i < 0x10'0000; ++i) rom[i] ^= k.xor1[le(i) % 0x20];
    for (uint32_t i = 0x10'0000; i < kSize; ++i) rom[i] ^= k.xor2[le(i) % 0x20];
    for (uint32_t i = 0x10'0000; i < kSize; i += 4) {
        auto w = static_cast<uint16_t>(rom[le(i + 1)] | (rom[le(i + 2)] << 8));
        w = static_cast<uint16_t>(bitswap<16>(w, k.wordSwap));
        rom[le(i + 1)] = static_cast<uint8_t>(w & 0xFF);
        rom[le(i + 2)] = static_cast<uint8_t>(w >> 8);
    }

    std::vector<uint8_t> buf(rom.begin(), rom.begin() + kSize);
    for (uint32_t i = 0; i < 0x10; ++i) {
        const uint32_t ofst = (i & 0xF0) + bitswap<8>(i & 0x0F, k.fixedBlocks);
        std::memcpy(&rom[i * 0x1'0000], &buf[ofst * 0x1'0000], 0x1'0000);
    }
    for (uint32_t i = 0x10'0000; i < kSize; i += 0x100) {
        const uint32_t ofst = (i & 0xF0'00FF) + ((i & 0x00'0F00) ^ k.pageXor) +
                              (bitswap<8>((i & 0x0F'F000) >> 12, k.bankBlocks) << 12);
        std::memcpy(&rom[i], &buf[ofst], 0x100);
    }
    buf.assign(rom.begin(), rom.begin() + kSize);
    std::memcpy(&rom[0x10'0000], &buf[0x70'0000], 0x10'0000);
    std::memcpy(&rom[0x20'0000], &buf[0x10'0000], 0x60'0000);
}

}  // namespace

void PvcProtection::decryptMslug5(std::span<uint8_t> rom) {
    decrypt8M(rom, PvcKeys{
        {0xc2, 0x4b, 0x74, 0xfd, 0x0b, 0x34, 0xeb, 0xd7, 0x10, 0x6d, 0xf9, 0xce, 0x5d, 0xd5, 0x61, 0x29,
         0xf5, 0xbe, 0x0d, 0x82, 0x72, 0x45, 0x0f, 0x24, 0xb3, 0x34, 0x1b, 0x99, 0xea, 0x09, 0xf3, 0x03},
        {0x36, 0x09, 0xb0, 0x64, 0x95, 0x0f, 0x90, 0x42, 0x6e, 0x0f, 0x30, 0xf6, 0xe5, 0x08, 0x30, 0x64,
         0x08, 0x04, 0x00, 0x2f, 0x72, 0x09, 0xa0, 0x13, 0xc9, 0x0b, 0xa0, 0x3e, 0xc2, 0x00, 0x40, 0x2b},
        {15, 14, 13, 12, 10, 11, 8, 9, 6, 7, 4, 5, 3, 2, 1, 0},
        {7, 6, 5, 4, 1, 0, 3, 2},
        0x00700,
        {5, 4, 7, 6, 1, 0, 3, 2},
    });
}

void PvcProtection::decryptSvc(std::span<uint8_t> rom) {
    decrypt8M(rom, PvcKeys{
        {0x3b, 0x6a, 0xf7, 0xb7, 0xe8, 0xa9, 0x20, 0x99, 0x9f, 0x39, 0x34, 0x0c, 0xc3, 0x9a, 0xa5, 0xc8,
         0xb8, 0x18, 0xce, 0x56, 0x94, 0x44, 0xe3, 0x7a, 0xf7, 0xdd, 0x42, 0xf0, 0x18, 0x60, 0x92, 0x9f},
        {0x69, 0x0b, 0x60, 0xd6, 0x4f, 0x01, 0x40, 0x1a, 0x9f, 0x0b, 0xf0, 0x75, 0x58, 0x0e, 0x60, 0xb4,
         0x14, 0x04, 0x20, 0xe4, 0xb9, 0x0d, 0x10, 0x89, 0xeb, 0x07, 0x30, 0x90, 0x50, 0x0e, 0x20, 0x26},
        {15, 14, 13, 12, 10, 11, 8, 9, 6, 7, 4, 5, 3, 2, 1, 0},
        {7, 6, 5, 4, 2, 3, 0, 1},
        0x00a00,
        {4, 5, 6, 7, 1, 0, 3, 2},
    });
}

// kof2003: 9 MiB region (P1/P2 interleaved + P3 at 0x800000).
void PvcProtection::decryptKof2003(std::span<uint8_t> rom) {
    static constexpr std::array<uint8_t, 0x20> xor1{
        0x3b, 0x6a, 0xf7, 0xb7, 0xe8, 0xa9, 0x20, 0x99, 0x9f, 0x39, 0x34, 0x0c, 0xc3, 0x9a, 0xa5, 0xc8,
        0xb8, 0x18, 0xce, 0x56, 0x94, 0x44, 0xe3, 0x7a, 0xf7, 0xdd, 0x42, 0xf0, 0x18, 0x60, 0x92, 0x9f};
    static constexpr std::array<uint8_t, 0x20> xor2{
        0x2f, 0x02, 0x60, 0xbb, 0x77, 0x01, 0x30, 0x08, 0xd8, 0x01, 0xa0, 0xdf, 0x37, 0x0a, 0xf0, 0x65,
        0x28, 0x03, 0xd0, 0x23, 0xd3, 0x03, 0x70, 0x42, 0xbb, 0x06, 0xf0, 0x28, 0xba, 0x0f, 0xf0, 0x7a};
    constexpr uint32_t kSize = 0x90'0000;
    if (rom.size() < kSize) throw std::invalid_argument("PVC: kof2003 program region must be 9 MiB");

    for (uint32_t i = 0; i < 0x10'0000; ++i) rom[0x80'0000 + i] ^= rom[0x10'0002 | i];
    for (uint32_t i = 0; i < 0x10'0000; ++i) rom[i] ^= xor1[le(i) % 0x20];
    for (uint32_t i = 0x10'0000; i < 0x80'0000; ++i) rom[i] ^= xor2[le(i) % 0x20];
    for (uint32_t i = 0x10'0000; i < 0x80'0000; i += 4) {
        auto w = static_cast<uint16_t>(rom[le(i + 1)] | (rom[le(i + 2)] << 8));
        w = static_cast<uint16_t>(bitswap<16>(w, {15, 14, 13, 12, 5, 4, 7, 6, 9, 8, 11, 10, 3, 2, 1, 0}));
        rom[le(i + 1)] = static_cast<uint8_t>(w & 0xFF);
        rom[le(i + 2)] = static_cast<uint8_t>(w >> 8);
    }
    std::vector<uint8_t> buf(kSize);
    for (uint32_t i = 0; i < 0x10; ++i) {
        const uint32_t ofst = (i & 0xF0) + bitswap<8>(i & 0x0F, {7, 6, 5, 4, 0, 1, 2, 3});
        std::memcpy(&buf[i * 0x1'0000], &rom[ofst * 0x1'0000], 0x1'0000);
    }
    for (uint32_t i = 0x10'0000; i < kSize; i += 0x100) {
        const uint32_t ofst = (i & 0xF0'00FF) + ((i & 0x00'0F00) ^ 0x00800) +
                              (bitswap<8>((i & 0x0F'F000) >> 12, {4, 5, 6, 7, 1, 0, 3, 2}) << 12);
        std::memcpy(&buf[i], &rom[ofst], 0x100);
    }
    std::memcpy(&rom[0x00'0000], &buf[0x00'0000], 0x10'0000);
    std::memcpy(&rom[0x10'0000], &buf[0x80'0000], 0x10'0000);
    std::memcpy(&rom[0x20'0000], &buf[0x10'0000], 0x70'0000);
}

}  // namespace snk
