#pragma once

#include <cstdint>
#include <span>

// Program scrambling of the KOF 2002 generation (NEO-PCM2 boards without a
// PVC): 512 KiB blocks of the P ROM image are shuffled.
// Ported from MAME (src/devices/bus/neogeo/prot_kof2k2.cpp, BSD-3-Clause,
// S. Smith, David Haywood, Fabio Priuli).
namespace snk::cart::kof2k2 {

// Samurai Shodown V: 8 MiB image (P1 at 0, P2 at 0x400000).
void decryptSamsho5(std::span<uint8_t> image);

}  // namespace snk::cart::kof2k2
