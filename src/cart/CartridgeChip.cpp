#include "cart/CartridgeChip.hpp"

#include "cart/MslugXProtection.hpp"
#include "cart/PvcProtection.hpp"
#include "cart/SmaProtection.hpp"

namespace snk {

std::unique_ptr<CartridgeChip> makeCartridgeChip(CartridgeChipType type) {
    switch (type) {
        case CartridgeChipType::MetalSlugX: return std::make_unique<MslugXProtection>();
        case CartridgeChipType::Pvc:        return std::make_unique<PvcProtection>();
        case CartridgeChipType::SmaMslug3:  return std::make_unique<SmaMslug3Protection>();
        case CartridgeChipType::None:       break;
    }
    return nullptr;
}

}  // namespace snk
