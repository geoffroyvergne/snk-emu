#pragma once

#include <cstdint>
#include <memory>

namespace snk {

// Everything the Z80 can reach. Implemented by SoundSystem.
class Z80Bus {
public:
    virtual uint8_t read(uint16_t address)               = 0;
    virtual void    write(uint16_t address, uint8_t value) = 0;
    // Full 16-bit port address: on IN A,(C) / OUT (C),A the upper byte comes
    // from register B. The Neo-Geo uses it for ROM bank switching.
    virtual uint8_t in(uint16_t port)                    = 0;
    virtual void    out(uint16_t port, uint8_t value)    = 0;

protected:
    ~Z80Bus() = default;
};

// Zilog Z80 sound CPU @ 4 MHz.
//
// Backend: floooh/chips z80.h, a cycle-stepped core. Each run() ticks the
// chip clock by clock and services the memory / I/O / interrupt-acknowledge
// cycles it requests, so execution stops exactly on the requested cycle (no
// instruction overshoot). State is per instance (no globals). The interface
// keeps the backend out of this header.
class Z80 {
public:
    explicit Z80(Z80Bus& bus);
    ~Z80();

    Z80(const Z80&)            = delete;
    Z80& operator=(const Z80&) = delete;

    // Power-on/RESET: PC = 0, interrupts disabled, IM 0.
    void reset();

    // Executes exactly `cycles` clock cycles (0 if cycles <= 0); returns them.
    uint32_t run(int32_t cycles);

    // /INT: level-triggered (held by the YM2610 until it is acknowledged).
    void setIrqLine(bool asserted) noexcept { irqLine_ = asserted; }
    // /NMI: edge-triggered; a 0 -> 1 transition requests one NMI.
    void setNmiLine(bool asserted) noexcept;

    [[nodiscard]] bool irqLine() const noexcept { return irqLine_; }
    // True once an NMI edge was presented since reset (diagnostics).
    [[nodiscard]] bool nmiRequested() const noexcept { return nmiRequested_; }
    [[nodiscard]] uint64_t totalCycles() const noexcept { return totalCycles_; }

    [[nodiscard]] uint16_t pc() const noexcept;
    [[nodiscard]] uint16_t sp() const noexcept;

private:
    struct Core;  // wraps z80_t + its pin state

    Z80Bus&               bus_;
    std::unique_ptr<Core> core_;
    bool                  irqLine_      = false;
    bool                  nmiLine_      = false;
    bool                  nmiRequested_ = false;
    uint64_t              totalCycles_  = 0;
};

}  // namespace snk
