#pragma once

#include <bit>
#include <cstdint>
#include <functional>
#include <utility>

namespace snk {

// 68000 interrupt levels on the MVS/AES (autovectored). The Neo-Geo CD swaps
// VBlank and Timer.
enum class Irq : uint8_t {
    VBlank   = 1,
    Timer    = 2,  // LSPC programmable timer ("raster"/HBlank-style effects)
    ColdBoot = 3,  // asserted at power-on, acknowledged by the BIOS
};

// Collects interrupt sources and drives the 68000's IPL inputs. Neo-Geo
// interrupts are level-triggered: they stay asserted until the program clears
// them through REG_IRQACK (0x3C000C). Knows nothing about the CPU type; the
// owner connects a level-change callback.
class InterruptController {
public:
    using LevelChanged = std::function<void(uint8_t level)>;

    void onLevelChanged(LevelChanged callback) {
        callback_  = std::move(callback);
        lastLevel_ = 0xFF;  // force the current level out to the new listener
        update();
    }

    void reset() {
        pending_ = 0;
        update();
    }

    void raise(Irq irq) {
        pending_ = static_cast<uint8_t>(pending_ | bit(irq));
        update();
    }

    // REG_IRQACK bits: 0 = cold boot (L3), 1 = timer (L2), 2 = VBlank (L1).
    void acknowledge(uint8_t ackBits) {
        uint8_t clear = 0;
        if (ackBits & 0x1) clear = static_cast<uint8_t>(clear | bit(Irq::ColdBoot));
        if (ackBits & 0x2) clear = static_cast<uint8_t>(clear | bit(Irq::Timer));
        if (ackBits & 0x4) clear = static_cast<uint8_t>(clear | bit(Irq::VBlank));
        pending_ = static_cast<uint8_t>(pending_ & ~clear);
        update();
    }

    [[nodiscard]] bool isPending(Irq irq) const noexcept { return (pending_ & bit(irq)) != 0; }

    // Highest pending level, 0 if none.
    [[nodiscard]] uint8_t level() const noexcept {
        // bit_width's return type differs between standard libraries (int vs T).
        return pending_ ? static_cast<uint8_t>(static_cast<unsigned>(std::bit_width(pending_)) - 1u) : 0;
    }

private:
    static constexpr uint8_t bit(Irq irq) noexcept {
        return static_cast<uint8_t>(1u << static_cast<uint8_t>(irq));
    }

    void update() {
        const uint8_t current = level();
        if (current != lastLevel_) {
            lastLevel_ = current;
            if (callback_) {
                callback_(current);
            }
        }
    }

    LevelChanged callback_;
    uint8_t      pending_   = 0;    // bit N set = level N asserted
    uint8_t      lastLevel_ = 0xFF;
};

}  // namespace snk
