#pragma once

#include <cstdint>
#include <memory>

namespace snk {

class MemoryBus;

// Custom logic some cartridges add on the 68000 bus (protection chips,
// bank-switching variants). The chip decodes a small window inside the P ROM
// banked area (0x200000-0x2FFFFF); the MemoryBus forwards every access to
// that window here and keeps serving the rest of the area from the P ROM.
class CartridgeChip {
public:
    virtual ~CartridgeChip() = default;

    [[nodiscard]] virtual const char* name() const noexcept = 0;
    // Inclusive address window the chip decodes.
    [[nodiscard]] virtual uint32_t firstAddress() const noexcept = 0;
    [[nodiscard]] virtual uint32_t lastAddress() const noexcept = 0;

    virtual void reset() noexcept = 0;
    // `address` is even. `bus` gives side-effect-free access to ROM/RAM, which
    // some chips read (e.g. tables stored in the P ROM).
    [[nodiscard]] virtual uint16_t read16(uint32_t address, const MemoryBus& bus) = 0;
    // `address` is even; `mask` selects the byte lanes written (0xFF00 / 0x00FF
    // for byte writes, the byte then sits on its lane). Chips that switch
    // banks do it through `bus`.
    virtual void write16(uint32_t address, uint16_t value, uint16_t mask, MemoryBus& bus) = 0;

    // Which accesses inside [firstAddress, lastAddress] the chip answers; the
    // rest of that range still reads the banked P ROM. Default: all of it.
    [[nodiscard]] virtual bool decodesRead(uint32_t address) const noexcept { return inWindow(address); }
    [[nodiscard]] virtual bool decodesWrite(uint32_t address) const noexcept { return inWindow(address); }

    [[nodiscard]] bool inWindow(uint32_t address) const noexcept {
        return address >= firstAddress() && address <= lastAddress();
    }
};

// Cartridge chips the emulator knows about.
enum class CartridgeChipType : uint8_t {
    None,
    MetalSlugX,  // NEO-MVS PROGEOP: Altera EPM7128 protection, 0x2FFFE0-0x2FFFEF
    Pvc,         // NEO-PVC: cartridge RAM + bank switch, 0x2FE000-0x2FFFFF
    SmaMslug3,   // NEO-SMA (Metal Slug 3): ID word at 0x2FE446, scrambled bank select at 0x2FFFE4
};

// nullptr for CartridgeChipType::None.
[[nodiscard]] std::unique_ptr<CartridgeChip> makeCartridgeChip(CartridgeChipType type);

}  // namespace snk
