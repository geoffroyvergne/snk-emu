#pragma once

#include <cstdint>
#include <span>

// NEO-PCM2: scrambles the V (ADPCM sample) ROMs of late Neo-Geo cartridges.
// Ported from MAME (src/devices/bus/neogeo/prot_pcm2.cpp, BSD-3-Clause,
// S. Smith, David Haywood, Fabio Priuli; thanks to Elsemi for the PCM2 info).
namespace snk::cart::pcm2 {

// Earlier PCM2 games (mslug4, rotd, pnyaa): swaps 16-bit words within blocks
// of `blockBytes` bytes.
void decrypt(std::span<uint8_t> samples, uint32_t blockBytes);

// Later games (mslug5, svc, kof2003, samsho5...): address bit swap + XOR over
// the first 16 MiB. `variant` 0-6 selects the per-game constants.
void swap(std::span<uint8_t> samples, uint32_t variant);

}  // namespace snk::cart::pcm2
