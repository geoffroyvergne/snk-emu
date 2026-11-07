// floooh/chips-backed implementation of snk::Z80.

#include "cpu/z80/Z80.hpp"

#include "chips/z80.h"

namespace snk {

namespace {

// Typed replacements for z80.h's Z80_GET/SET_ADDR/DATA macros (which trip
// our -Wconversion settings when expanded here).
constexpr uint16_t pinAddress(uint64_t pins) noexcept { return static_cast<uint16_t>(pins & 0xFFFFu); }
constexpr uint8_t pinData(uint64_t pins) noexcept { return static_cast<uint8_t>((pins >> 16) & 0xFFu); }
constexpr uint64_t withData(uint64_t pins, uint8_t data) noexcept {
    return (pins & ~0xFF'0000ull) | (uint64_t{data} << 16);
}

}  // namespace

struct Z80::Core {
    z80_t    cpu{};
    uint64_t pins = 0;
};

Z80::Z80(Z80Bus& bus) : bus_(bus), core_(std::make_unique<Core>()) {
    core_->pins = z80_init(&core_->cpu);
}

Z80::~Z80() = default;

void Z80::reset() {
    core_->pins   = z80_reset(&core_->cpu);
    irqLine_      = false;
    nmiLine_      = false;
    nmiRequested_ = false;
}

void Z80::setNmiLine(bool asserted) noexcept {
    if (asserted && !nmiLine_) {
        nmiRequested_ = true;
    }
    nmiLine_ = asserted;
}

uint32_t Z80::run(int32_t cycles) {
    if (cycles <= 0) {
        return 0;
    }
    z80_t&   cpu  = core_->cpu;
    uint64_t pins = core_->pins;

    for (int32_t i = 0; i < cycles; ++i) {
        // Present the interrupt inputs; z80.h detects the NMI rising edge itself.
        pins &= ~(Z80_INT | Z80_NMI);
        if (irqLine_) pins |= Z80_INT;
        if (nmiLine_) pins |= Z80_NMI;

        pins = z80_tick(&cpu, pins);

        if (pins & Z80_MREQ) {
            const uint16_t address = pinAddress(pins);
            if (pins & Z80_RD) {
                pins = withData(pins, bus_.read(address));
            } else if (pins & Z80_WR) {
                bus_.write(address, pinData(pins));
            }
        } else if (pins & Z80_IORQ) {
            if (pins & Z80_M1) {
                // Interrupt acknowledge. The Neo-Geo sound drivers run in IM 1
                // (RST 38h), which ignores the bus; 0xFF is also RST 38h in IM 0.
                pins = withData(pins, 0xFF);
            } else if (pins & Z80_RD) {
                pins = withData(pins, bus_.in(pinAddress(pins)));
            } else if (pins & Z80_WR) {
                bus_.out(pinAddress(pins), pinData(pins));
            }
        }
    }

    core_->pins = pins;
    const auto used = static_cast<uint32_t>(cycles);
    totalCycles_ += used;
    return used;
}

uint16_t Z80::pc() const noexcept { return core_->cpu.pc; }
uint16_t Z80::sp() const noexcept { return core_->cpu.sp; }

}  // namespace snk
