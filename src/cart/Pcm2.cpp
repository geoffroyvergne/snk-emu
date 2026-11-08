// NEO-PCM2 decryption, ported from MAME (src/devices/bus/neogeo/prot_pcm2.cpp).
// license:BSD-3-Clause, copyright-holders: S. Smith, David Haywood, Fabio Priuli
#include "cart/Pcm2.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

#include "cart/BitSwap.hpp"

namespace snk::cart::pcm2 {

void decrypt(std::span<uint8_t> samples, uint32_t blockBytes) {
    const uint32_t words     = blockBytes / 2;  // words per block
    const uint32_t swapBit   = blockBytes / 4;
    std::vector<uint8_t> block(blockBytes);
    for (size_t at = 0; at + blockBytes <= samples.size(); at += blockBytes) {
        std::copy_n(samples.begin() + static_cast<std::ptrdiff_t>(at), blockBytes, block.begin());
        for (uint32_t j = 0; j < words; ++j) {
            const uint32_t from = j ^ swapBit;
            samples[at + 2 * j]     = block[2 * from];
            samples[at + 2 * j + 1] = block[2 * from + 1];
        }
    }
}

void swap(std::span<uint8_t> samples, uint32_t variant) {
    static constexpr uint32_t kAddrs[7][2] = {{0x000000, 0xa5000}, {0xffce20, 0x01000}, {0xfe2cf6, 0x4e001},
                                              {0xffac28, 0xc2000}, {0xfeb2c0, 0x0a000}, {0xff14ea, 0xa7001},
                                              {0xffb440, 0x02000}};
    static constexpr uint8_t kXor[7][8] = {{0xf9, 0xe0, 0x5d, 0xf3, 0xea, 0x92, 0xbe, 0xef},
                                           {0xc4, 0x83, 0xa8, 0x5f, 0x21, 0x27, 0x64, 0xaf},
                                           {0xc3, 0xfd, 0x81, 0xac, 0x6d, 0xe7, 0xbf, 0x9e},
                                           {0xc3, 0xfd, 0x81, 0xac, 0x6d, 0xe7, 0xbf, 0x9e},
                                           {0xcb, 0x29, 0x7d, 0x43, 0xd2, 0x3a, 0xc2, 0xb4},
                                           {0x4b, 0xa4, 0x63, 0x46, 0xf0, 0x91, 0xea, 0x62},
                                           {0x4b, 0xa4, 0x63, 0x46, 0xf0, 0x91, 0xea, 0x62}};
    constexpr uint32_t kSize = 0x100'0000;
    if (variant >= 7 || samples.size() < kSize) {
        throw std::invalid_argument("PCM2 swap: needs 16 MiB of samples and a variant 0-6");
    }
    const std::vector<uint8_t> buf(samples.begin(), samples.begin() + kSize);
    for (uint32_t i = 0; i < kSize; ++i) {
        uint32_t j = bitswap<24>(i, {23, 22, 21, 20, 19, 18, 17, 0, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 16});
        j ^= kAddrs[variant][1];
        const uint32_t d = (i + kAddrs[variant][0]) & 0xFF'FFFF;
        samples[j] = buf[d] ^ kXor[variant][j & 7];
    }
}

}  // namespace snk::cart::pcm2
