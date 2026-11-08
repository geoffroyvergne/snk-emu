#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace snk::cart {

// MAME-style bitswap: bits[0] names the source bit that becomes the most
// significant result bit, bits[N-1] the source of bit 0.
template <size_t N>
[[nodiscard]] constexpr uint32_t bitswap(uint32_t value, const std::array<int, N>& bits) noexcept {
    uint32_t result = 0;
    for (size_t i = 0; i < N; ++i) {
        result = (result << 1) | ((value >> bits[i]) & 1u);
    }
    return result;
}

}  // namespace snk::cart
