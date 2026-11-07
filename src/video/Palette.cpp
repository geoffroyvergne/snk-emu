#include "video/Palette.hpp"

#include <algorithm>

namespace snk::palette {

const std::array<uint32_t, 0x1'0000>& lookupTable() {
    // Static storage, built on first use (thread-safe initialization).
    static const std::array<uint32_t, 0x1'0000> table = [] {
        std::array<uint32_t, 0x1'0000> t{};
        for (uint32_t color = 0; color < t.size(); ++color) {
            t[color] = toArgb(static_cast<uint16_t>(color));
        }
        return t;
    }();
    return table;
}

void indicesToArgb(std::span<const uint8_t> indices, std::span<const uint8_t> paletteRam, uint8_t paletteNumber,
                   std::span<uint32_t> out) {
    const auto&    lut  = lookupTable();
    const uint32_t base = uint32_t{paletteNumber} * 16;
    const size_t   n    = std::min(indices.size(), out.size());
    for (size_t i = 0; i < n; ++i) {
        const uint32_t index = indices[i] & 0xFu;
        out[i] = index == 0 ? 0u : entryToArgb(lut, paletteRam, base + index);
    }
}

}  // namespace snk::palette
