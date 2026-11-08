#pragma once

#include <cstdint>
#include <optional>

#include "cart/CartridgeChip.hpp"
#include "cart/Cmc.hpp"
#include "video/VideoSystem.hpp"

namespace snk::cart {

// How a cartridge's program ROM files form the 68000 image.
enum class ProgramLayout : uint8_t {
    Standard,      // P1 at 0, P2... from 0x100000 (each file word-swapped)
    Interleaved32, // P1/P2 alternate word by word (MAME ROM_LOAD32_WORD_SWAP), P3 at 0x800000
    Linear,        // P files back to back from `programStart`, in an image of `programRegionSize`
};

enum class ProgramCipher : uint8_t { None, PvcMslug5, PvcSvc, PvcKof2003, Samsho5, SmaMslug3 };
enum class SampleCipher : uint8_t { None, Pcm2Decrypt, Pcm2Swap };

// Everything beyond plain ROMs a cartridge needs. Values from MAME's
// cartridge definitions (src/devices/bus/neogeo/*.cpp, src/mame/neogeo/neogeo.cpp).
struct CartridgeInfo {
    const char*                setName;   // MAME name, for messages
    uint32_t                   p1Crc;     // CRC32 of the first program ROM file
    CartridgeChipType          chip     = CartridgeChipType::None;
    ProgramLayout              layout   = ProgramLayout::Standard;
    uint32_t                   programRegionSize = 0;  // Interleaved32 / Linear: total image size
    uint32_t                   programStart = 0;       // Linear: where P1 goes
    bool                       smaCode  = false;       // "neo-sma" dump at 0x0C0000 (SMA chip's internal ROM)
    ProgramCipher              program  = ProgramCipher::None;
    std::optional<cmc::Chip>   gfx;                    // C ROMs encrypted -> fix layer comes from them
    uint8_t                    gfxKey   = 0;
    uint32_t                   fixSize  = 0x2'0000;    // bytes of fix tiles taken from the C ROMs
    VideoSystem::FixBanking    fixBanking = VideoSystem::FixBanking::None;
    bool                       m1Encrypted = false;    // CMC50 M1
    SampleCipher               samples  = SampleCipher::None;
    uint32_t                   sampleParam = 0;        // PCM2 block size or swap variant
};

// nullptr for a cartridge without special hardware.
[[nodiscard]] const CartridgeInfo* findCartridge(uint32_t p1Crc) noexcept;

}  // namespace snk::cart
