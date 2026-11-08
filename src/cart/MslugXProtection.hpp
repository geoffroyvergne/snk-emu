#pragma once

#include "cart/CartridgeChip.hpp"

namespace snk {

// Metal Slug X protection (Altera EPM7128 on the NEO-MVS PROGEOP board).
//
// The game writes a command to 0x2FFFE0-0x2FFFEF, then reads back a
// bitstream, one bit per read, taken from a table in its own P1 ROM at
// 0x0DEDD2. If the bits don't match, it hangs before showing anything.
// Behaviour follows MAME's mslugx_prot_device.
class MslugXProtection final : public CartridgeChip {
public:
    [[nodiscard]] const char* name() const noexcept override { return "Metal Slug X protection"; }
    [[nodiscard]] uint32_t firstAddress() const noexcept override { return 0x2F'FFE0; }
    [[nodiscard]] uint32_t lastAddress() const noexcept override { return 0x2F'FFEF; }

    void reset() noexcept override;
    [[nodiscard]] uint16_t read16(uint32_t address, const MemoryBus& bus) override;
    void write16(uint32_t address, uint16_t value, uint16_t mask, MemoryBus& bus) override;

private:
    static constexpr uint32_t kTable          = 0x0D'EDD2;  // bit table in P1
    static constexpr uint32_t kSelectRegister = 0x10'F00A;  // work RAM word the game uses as bit index

    uint16_t command_ = 0;
    uint16_t counter_ = 0;
};

}  // namespace snk
