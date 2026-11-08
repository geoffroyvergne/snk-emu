#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// NEO-CMC42 / NEO-CMC50: the encryption chips of late Neo-Geo cartridges.
//
//   - C ROM data and addresses are scrambled, with a per-game key.
//   - There is no S1 ROM: the fix layer tiles are the last bytes of the
//     (decrypted) C ROM data, in a slightly different order.
//   - CMC50 also scrambles the M1 Z80 ROM, keyed by a checksum of its first
//     64 KiB.
//
// Algorithms ported from MAME (src/devices/bus/neogeo/prot_cmc.cpp,
// BSD-3-Clause, S. Smith, David Haywood, Fabio Priuli).
namespace snk::cart::cmc {

enum class Chip : uint8_t { Cmc42, Cmc50 };

// `sprites` is MAME's sprite region layout: C1 bytes at even offsets, C2 at
// odd offsets, then C3/C4 after them, and so on. Decrypted in place.
void decryptSprites(std::span<uint8_t> sprites, Chip chip, uint8_t key);

// Fix layer tiles (fixSize bytes) rebuilt from the end of the decrypted sprites.
[[nodiscard]] std::vector<uint8_t> extractFix(std::span<const uint8_t> sprites, size_t fixSize);

// CMC50 M1: returns the plain 512 KiB Z80 program.
[[nodiscard]] std::vector<uint8_t> decryptM1(std::span<const uint8_t> encrypted);

}  // namespace snk::cart::cmc
