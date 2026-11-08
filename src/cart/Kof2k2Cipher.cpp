// KOF 2002-generation program scrambling, ported from MAME
// (src/devices/bus/neogeo/prot_kof2k2.cpp).
// license:BSD-3-Clause, copyright-holders: S. Smith, David Haywood, Fabio Priuli
#include "cart/Kof2k2Cipher.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

namespace snk::cart::kof2k2 {

void decryptSamsho5(std::span<uint8_t> image) {
    static constexpr std::array<uint32_t, 16> kSource{
        0x000000, 0x080000, 0x700000, 0x680000, 0x500000, 0x180000, 0x200000, 0x480000,
        0x300000, 0x780000, 0x600000, 0x280000, 0x100000, 0x580000, 0x400000, 0x380000};
    constexpr size_t kBlock = 0x8'0000;
    if (image.size() < 16 * kBlock) throw std::invalid_argument("samsho5: program image must be 8 MiB");
    const std::vector<uint8_t> copy(image.begin(), image.begin() + 16 * kBlock);
    for (size_t i = 0; i < 16; ++i) {
        std::copy_n(copy.begin() + kSource[i], kBlock, image.begin() + static_cast<std::ptrdiff_t>(i * kBlock));
    }
}

}  // namespace snk::cart::kof2k2
