#include "cart/CartridgeDb.hpp"

#include <array>

namespace snk::cart {

namespace {

using enum ProgramLayout;
using Fix = VideoSystem::FixBanking;

constexpr std::array kCartridges{
    CartridgeInfo{.setName = "mslugx", .p1Crc = 0x81F1'F60B, .chip = CartridgeChipType::MetalSlugX},
    CartridgeInfo{.setName = "mslug3", .p1Crc = 0xB07E'DFD5, .chip = CartridgeChipType::SmaMslug3,
                  .layout = Linear, .programRegionSize = 0x90'0000, .programStart = 0x10'0000,
                  .smaCode = true, .program = ProgramCipher::SmaMslug3,
                  .gfx = cmc::Chip::Cmc42, .gfxKey = 0xAD, .fixSize = 0x8'0000, .fixBanking = Fix::Garou},
    CartridgeInfo{.setName = "samsho5", .p1Crc = 0x4A2A'09E6,
                  .layout = Linear, .programRegionSize = 0x80'0000, .program = ProgramCipher::Samsho5,
                  .gfx = cmc::Chip::Cmc50, .gfxKey = 0x0F, .fixSize = 0x2'0000, .fixBanking = Fix::Garou,
                  .m1Encrypted = true, .samples = SampleCipher::Pcm2Swap, .sampleParam = 4},
    CartridgeInfo{.setName = "mslug4", .p1Crc = 0x27E4'DEF3,
                  .gfx = cmc::Chip::Cmc50, .gfxKey = 0x31, .fixSize = 0x8'0000, .fixBanking = Fix::Garou,
                  .m1Encrypted = true, .samples = SampleCipher::Pcm2Decrypt, .sampleParam = 8},
    CartridgeInfo{.setName = "mslug5", .p1Crc = 0xD046'6792, .chip = CartridgeChipType::Pvc,
                  .layout = Interleaved32, .programRegionSize = 0x80'0000, .program = ProgramCipher::PvcMslug5,
                  .gfx = cmc::Chip::Cmc50, .gfxKey = 0x19, .fixSize = 0x2'0000, .fixBanking = Fix::Garou,
                  .m1Encrypted = true, .samples = SampleCipher::Pcm2Swap, .sampleParam = 2},
    CartridgeInfo{.setName = "svc", .p1Crc = 0x38E2'005E, .chip = CartridgeChipType::Pvc,
                  .layout = Interleaved32, .programRegionSize = 0x80'0000, .program = ProgramCipher::PvcSvc,
                  .gfx = cmc::Chip::Cmc50, .gfxKey = 0x57, .fixSize = 0x8'0000, .fixBanking = Fix::Kof2000,
                  .m1Encrypted = true, .samples = SampleCipher::Pcm2Swap, .sampleParam = 3},
    CartridgeInfo{.setName = "kof2003", .p1Crc = 0x530E'CC14, .chip = CartridgeChipType::Pvc,
                  .layout = Interleaved32, .programRegionSize = 0x90'0000, .program = ProgramCipher::PvcKof2003,
                  .gfx = cmc::Chip::Cmc50, .gfxKey = 0x9D, .fixSize = 0x8'0000, .fixBanking = Fix::Kof2000,
                  .m1Encrypted = true, .samples = SampleCipher::Pcm2Swap, .sampleParam = 5},
};

}  // namespace

const CartridgeInfo* findCartridge(uint32_t p1Crc) noexcept {
    for (const CartridgeInfo& info : kCartridges) {
        if (info.p1Crc == p1Crc) {
            return &info;
        }
    }
    return nullptr;
}

}  // namespace snk::cart
