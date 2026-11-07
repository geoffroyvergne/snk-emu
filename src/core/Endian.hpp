#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <span>

// Neo-Geo ROMs and the 68000 bus are big-endian; hosts (x86-64, ARM64) are
// little-endian. All multi-byte ROM/RAM accesses must go through these helpers.
namespace snk::endian {

[[nodiscard]] constexpr uint16_t byteswap16(uint16_t v) noexcept {
    return static_cast<uint16_t>((v << 8) | (v >> 8));
}

[[nodiscard]] constexpr uint32_t byteswap32(uint32_t v) noexcept {
    return ((v & 0x0000'00FFu) << 24) |
           ((v & 0x0000'FF00u) << 8)  |
           ((v & 0x00FF'0000u) >> 8)  |
           ((v & 0xFF00'0000u) >> 24);
}

// Big-endian <-> host conversion (no-ops on big-endian hosts).
[[nodiscard]] constexpr uint16_t be16_to_host(uint16_t v) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return byteswap16(v);
    } else {
        return v;
    }
}

[[nodiscard]] constexpr uint32_t be32_to_host(uint32_t v) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return byteswap32(v);
    } else {
        return v;
    }
}

[[nodiscard]] constexpr uint16_t host_to_be16(uint16_t v) noexcept { return be16_to_host(v); }
[[nodiscard]] constexpr uint32_t host_to_be32(uint32_t v) noexcept { return be32_to_host(v); }

// Read/write big-endian values from a byte buffer. Explicit shifts are
// alignment-safe and endian-agnostic; compilers lower them to a single
// load + bswap (or movbe / rev) instruction.
[[nodiscard]] constexpr uint16_t read_be16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((uint16_t{p[0]} << 8) | uint16_t{p[1]});
}

[[nodiscard]] constexpr uint32_t read_be32(const uint8_t* p) noexcept {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) |
           (uint32_t{p[2]} << 8)  |  uint32_t{p[3]};
}

constexpr void write_be16(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

constexpr void write_be32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

// Some ROM dumps (e.g. P-ROMs in certain sets) are stored word-swapped.
// Converts an entire buffer in place between big- and little-endian 16-bit words.
inline void swap_words_in_place(std::span<uint8_t> data) noexcept {
    for (size_t i = 0; i + 1 < data.size(); i += 2) {
        const uint8_t tmp = data[i];
        data[i]     = data[i + 1];
        data[i + 1] = tmp;
    }
}

// Compile-time sanity checks.
static_assert(byteswap16(0x1234) == 0x3412);
static_assert(byteswap32(0x1234'5678u) == 0x7856'3412u);
static_assert([] {
    constexpr uint8_t bytes[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    return read_be16(bytes) == 0xDEAD && read_be32(bytes) == 0xDEAD'BEEFu;
}());

}  // namespace snk::endian
