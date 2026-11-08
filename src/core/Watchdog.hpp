#pragma once

#include <cstdint>

namespace snk {

// Neo-Geo watchdog: resets the whole system unless the 68000 writes to
// 0x300001 (any mirror) at least every 3,244,030 master clocks (~0.135 s).
//
// Games use it as copy protection: they write a "failed" marker to backup
// RAM after a busy-wait that never kicks the watchdog. On a genuine board the
// reset comes first and the marker is never written; a board without a
// watchdog reaches it and the game shows a "WARNING ... ORIGINAL SNK BOARDS
// ONLY" screen from then on (Metal Slug 2, KOF 99, ...).
class Watchdog {
public:
    static constexpr uint64_t kTimeoutCycles = 3'244'030 / 2;  // in 68000 cycles (12 MHz)

    void reset() noexcept { elapsed_ = 0; }
    void kick() noexcept { elapsed_ = 0; }

    // True once the timeout has passed since the last kick (then restarts).
    [[nodiscard]] bool advance(uint32_t cycles) noexcept {
        elapsed_ += cycles;
        if (elapsed_ < kTimeoutCycles) {
            return false;
        }
        elapsed_ = 0;
        return true;
    }

private:
    uint64_t elapsed_ = 0;
};

}  // namespace snk
