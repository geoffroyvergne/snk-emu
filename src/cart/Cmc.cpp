// NEO-CMC42 / NEO-CMC50 decryption, ported from MAME
// (src/devices/bus/neogeo/prot_cmc.cpp).
// license:BSD-3-Clause, copyright-holders: S. Smith, David Haywood, Fabio Priuli
#include "cart/Cmc.hpp"

#include <algorithm>
#include <array>

#include "cart/BitSwap.hpp"

namespace snk::cart::cmc {

namespace {

#include "cart/CmcTables.inc"

struct Tables {
    const std::array<uint8_t, 256>& type0T03;
    const std::array<uint8_t, 256>& type0T12;
    const std::array<uint8_t, 256>& type1T03;
    const std::array<uint8_t, 256>& type1T12;
    const std::array<uint8_t, 256>& address8To15Xor1;
    const std::array<uint8_t, 256>& address8To15Xor2;
    const std::array<uint8_t, 256>& address16To23Xor1;
    const std::array<uint8_t, 256>& address16To23Xor2;
    const std::array<uint8_t, 256>& address0To7Xor;
};

constexpr Tables kCmc42{kof99_type0_t03,          kof99_type0_t12,          kof99_type1_t03,
                        kof99_type1_t12,          kof99_address_8_15_xor1,  kof99_address_8_15_xor2,
                        kof99_address_16_23_xor1, kof99_address_16_23_xor2, kof99_address_0_7_xor};
constexpr Tables kCmc50{kof2000_type0_t03,          kof2000_type0_t12,          kof2000_type1_t03,
                        kof2000_type1_t12,          kof2000_address_8_15_xor1,  kof2000_address_8_15_xor2,
                        kof2000_address_16_23_xor1, kof2000_address_16_23_xor2, kof2000_address_0_7_xor};

// One pair of bytes of a 32-bit group: XOR both with values derived from the
// group's position.
void decryptPair(const Tables& t, uint8_t& r0, uint8_t& r1, uint8_t c0, uint8_t c1,
                 const std::array<uint8_t, 256>& table0Hi, const std::array<uint8_t, 256>& table0Lo,
                 const std::array<uint8_t, 256>& table1, uint32_t base, bool invert) {
    const uint8_t tmp  = table1[(base & 0xFF) ^ t.address0To7Xor[(base >> 8) & 0xFF]];
    const auto    xor0 = static_cast<uint8_t>((table0Hi[(base >> 8) & 0xFF] & 0xFE) | (tmp & 0x01));
    const auto    xor1 = static_cast<uint8_t>((tmp & 0xFE) | (table0Lo[(base >> 8) & 0xFF] & 0x01));
    if (invert) {
        r0 = c1 ^ xor0;
        r1 = c0 ^ xor1;
    } else {
        r0 = c0 ^ xor0;
        r1 = c1 ^ xor1;
    }
}

}  // namespace

void decryptSprites(std::span<uint8_t> rom, Chip chip, uint8_t key) {
    const Tables&  t      = chip == Chip::Cmc42 ? kCmc42 : kCmc50;
    const auto     groups = static_cast<uint32_t>(rom.size() / 4);
    std::vector<uint8_t> buf(rom.size());

    // Data XOR.
    for (uint32_t r = 0; r < groups; ++r) {
        decryptPair(t, buf[4 * r + 0], buf[4 * r + 3], rom[4 * r + 0], rom[4 * r + 3], t.type0T03, t.type0T12,
                    t.type1T03, r, ((r >> 8) & 1u) != 0);
        decryptPair(t, buf[4 * r + 1], buf[4 * r + 2], rom[4 * r + 1], rom[4 * r + 2], t.type0T12, t.type0T03,
                    t.type1T12, r, (((r >> 16) ^ t.address16To23Xor2[(r >> 8) & 0xFF]) & 1u) != 0);
    }

    // Address XOR: each 32-bit group moves to a scrambled position.
    for (uint32_t r = 0; r < groups; ++r) {
        uint32_t b = r ^ key;
        b ^= uint32_t{t.address8To15Xor1[(b >> 16) & 0xFF]} << 8;
        b ^= uint32_t{t.address8To15Xor2[b & 0xFF]} << 8;
        b ^= uint32_t{t.address16To23Xor1[b & 0xFF]} << 16;
        b ^= uint32_t{t.address16To23Xor2[(b >> 8) & 0xFF]} << 16;
        b ^= t.address0To7Xor[(b >> 8) & 0xFF];

        if (rom.size() == 0x300'0000) {  // preisle2
            b = r < 0x200'0000 / 4 ? b & (0x200'0000 / 4 - 1) : 0x200'0000 / 4 + (b & (0x100'0000 / 4 - 1));
        } else if (rom.size() == 0x600'0000) {  // kf2k3pcb
            b = r < 0x400'0000 / 4 ? b & (0x400'0000 / 4 - 1) : 0x400'0000 / 4 + (b & (0x100'0000 / 4 - 1));
        } else {
            b &= groups - 1;  // as MAME: clamps to the region size
        }
        std::copy_n(buf.begin() + 4 * static_cast<std::ptrdiff_t>(b), 4, rom.begin() + 4 * static_cast<std::ptrdiff_t>(r));
    }
}

std::vector<uint8_t> extractFix(std::span<const uint8_t> sprites, size_t fixSize) {
    std::vector<uint8_t> fix(fixSize);
    if (sprites.size() < fixSize) {
        return fix;
    }
    const uint8_t* src = sprites.data() + sprites.size() - fixSize;
    for (size_t i = 0; i < fixSize; ++i) {
        fix[i] = src[(i & ~size_t{0x1F}) + ((i & 7) << 2) + ((~i & 8) >> 2) + ((i & 0x10) >> 4)];
    }
    return fix;
}

namespace {

uint32_t m1AddressScramble(uint32_t address, uint16_t key) {
    static constexpr int p1[8][16] = {
        {15, 14, 10, 7, 1, 2, 3, 8, 0, 12, 11, 13, 6, 9, 5, 4},
        {7, 1, 8, 11, 15, 9, 2, 3, 5, 13, 4, 14, 10, 0, 6, 12},
        {8, 6, 14, 3, 10, 7, 15, 1, 4, 0, 2, 5, 13, 11, 12, 9},
        {2, 8, 15, 9, 3, 4, 11, 7, 13, 6, 0, 10, 1, 12, 14, 5},
        {1, 13, 6, 15, 14, 3, 8, 10, 9, 4, 7, 12, 5, 2, 0, 11},
        {11, 15, 3, 4, 7, 0, 9, 2, 6, 14, 12, 1, 8, 5, 10, 13},
        {10, 5, 13, 8, 6, 15, 1, 14, 11, 9, 3, 0, 12, 7, 4, 2},
        {9, 3, 7, 0, 2, 12, 4, 11, 14, 10, 5, 8, 15, 13, 1, 6},
    };
    const uint32_t block = (address >> 16) & 7;
    uint32_t       aux   = address & 0xFFFF;
    const int*     p     = p1[block];

    aux ^= bitswap<16>(key, {12, 0, 2, 4, 8, 15, 7, 13, 10, 1, 3, 6, 11, 9, 14, 5});
    aux = bitswap<16>(aux, {p[15], p[14], p[13], p[12], p[11], p[10], p[9], p[8], p[7], p[6], p[5], p[4], p[3],
                            p[2], p[1], p[0]});
    aux ^= m1_address_0_7_xor[(aux >> 8) & 0xFF];
    aux ^= uint32_t{m1_address_8_15_xor[aux & 0xFF]} << 8;
    aux = bitswap<16>(aux, {7, 15, 14, 6, 5, 13, 12, 4, 11, 3, 10, 2, 9, 1, 8, 0});
    return (block << 16) | aux;
}

}  // namespace

std::vector<uint8_t> decryptM1(std::span<const uint8_t> encrypted) {
    constexpr size_t kSize = 0x8'0000;
    std::vector<uint8_t> rom(kSize, 0);  // smaller dumps are padded, as MAME's region is
    std::copy_n(encrypted.begin(), std::min(encrypted.size(), kSize), rom.begin());

    // The chip derives its key from a 16-bit byte sum of the first 64 KiB.
    uint16_t key = 0;
    for (size_t i = 0; i < 0x1'0000; ++i) {
        key = static_cast<uint16_t>(key + rom[i]);
    }
    std::vector<uint8_t> plain(kSize);
    for (uint32_t i = 0; i < kSize; ++i) {
        plain[i] = rom[m1AddressScramble(i, key)];
    }
    return plain;
}

}  // namespace snk::cart::cmc
