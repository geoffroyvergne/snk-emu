#pragma once

#include <cstdint>
#include <vector>

namespace snk {

class MemoryBus;

// Motorola 68000 main CPU @ 12 MHz.
//
// Backend-agnostic interface; the implementation (M68000.cpp) is currently
// backed by Musashi and is the only file that includes Musashi headers.
//
// Musashi keeps one global CPU. Each M68000 instance owns a private copy of
// Musashi's context and swaps it in on demand, so several Systems can coexist
// (tests, run-ahead, ...). Constraint inherited from Musashi: only one
// instance can execute at a time, and only from one thread at a time.
class M68000 {
public:
    explicit M68000(MemoryBus& bus);
    ~M68000();

    M68000(const M68000&)            = delete;
    M68000& operator=(const M68000&) = delete;

    // Pulses RESET: supervisor mode, IPL mask 7, SSP/PC loaded from vectors 0 and 4.
    void reset();

    // Executes whole instructions until at least `cycles` clocks have elapsed.
    // May overshoot by up to one instruction; returns the cycles actually run
    // so the caller can carry the difference into the next slice.
    uint32_t run(int32_t cycles);

    // Makes run() return after the current instruction. Callable from inside
    // bus handlers (e.g. a write that needs the Z80 to catch up).
    void endTimeslice() noexcept;

    // Cycles executed so far in the run() call in progress; 0 outside run().
    // Lets bus handlers know "when" within the slice an access happens.
    [[nodiscard]] uint32_t cyclesThisSlice() const noexcept;

    // Drives the IPL0-2 inputs (0 = none, 1..7). Level-sensitive, autovectored.
    void setInterruptLevel(uint8_t level);

    // Register inspection: valid whether or not this instance is the active one.
    [[nodiscard]] uint32_t pc() const;
    [[nodiscard]] uint16_t sr() const;
    [[nodiscard]] uint32_t dataReg(unsigned index) const;     // D0-D7
    [[nodiscard]] uint32_t addressReg(unsigned index) const;  // A0-A7 (A7 = current SP)

    [[nodiscard]] uint64_t totalCycles() const noexcept { return totalCycles_; }

private:
    void activate();
    [[nodiscard]] uint32_t readRegister(int reg) const;

    MemoryBus&           bus_;
    std::vector<uint8_t> context_;  // Musashi CPU state while another instance is active
    uint64_t             totalCycles_ = 0;
    bool                 executing_   = false;
};

}  // namespace snk
