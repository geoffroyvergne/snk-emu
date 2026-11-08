#include "cart/MslugXProtection.hpp"

#include "memory/MemoryBus.hpp"

namespace snk {

namespace {

// Bit `index` of the table, most significant bit of each byte first.
uint16_t tableBit(const MemoryBus& bus, uint32_t table, uint32_t index) noexcept {
    const uint8_t byte = bus.peek8(table + ((index >> 3) & 0x0FFF));
    return static_cast<uint16_t>((byte >> (~index & 0x07)) & 1u);
}

}  // namespace

void MslugXProtection::reset() noexcept {
    command_ = 0;
    counter_ = 0;
}

void MslugXProtection::write16(uint32_t address, uint16_t value, uint16_t /*mask*/, MemoryBus& /*bus*/) {
    switch (address & 0x0E) {
        case 0x00:  // start of a new read sequence
            command_ = 0;
            break;
        case 0x02:  // command bits, pulsed with data then 0
        case 0x04:
            command_ |= value;
            break;
        case 0x0A:  // init
            counter_ = 0;
            command_ = 0;
            break;
        default:  // 0x06 "finished", others unused
            break;
    }
}

uint16_t MslugXProtection::read16(uint32_t /*address*/, const MemoryBus& bus) {
    switch (command_) {
        case 0x0001: {  // sequential stream
            const uint16_t bit = tableBit(bus, kTable, counter_);
            ++counter_;
            return bit;
        }
        case 0x0FFF: {  // random access, index taken from work RAM
            const auto select = static_cast<uint32_t>(bus.peek16(kSelectRegister) - 1u);
            return tableBit(bus, kTable, select);
        }
        default:
            return 0;
    }
}

}  // namespace snk
