#pragma once

#include <array>
#include <cstdint>

namespace snk {

// NEC uPD4990A serial calendar/clock (MVS boards only).
//
// Wiring on the MVS: CS, OE and C0-C2 are tied high (serial command mode).
// The 68000 drives DATA IN / CLK / STB through bits 0-2 of a write to
// 0x380051 and reads TP (time pulse) and DATA OUT on REG_STATUS_A bits 6/7.
// The BIOS waits on TP edges during boot and reads/sets the date for its
// bookkeeping screens.
//
// Protocol: every CLK rising edge shifts DATA IN into a 4-bit command
// register; the bit falling out of it shifts into a 48-bit data register
// (when the current mode is Shift). A STB rising edge executes the command.
// Data register = 6 bytes, LSB first: seconds, minutes, hours, day (BCD),
// month (binary, high nibble) | weekday (low nibble), year (BCD).
//
// Time is emulated, not host time: advance() is fed the chip's 32.768 kHz
// ticks from the scheduler. Behavior follows MAME's upd1990a device.
class Upd4990a {
public:
    static constexpr uint32_t kClockHz = 32'768;

    struct DateTime {
        int year      = 2000;  // 2000-2099
        int month     = 1;     // 1-12
        int day       = 1;     // 1-31
        int dayOfWeek = 6;     // 0 = Sunday
        int hour      = 0;
        int minute    = 0;
        int second    = 0;
    };

    enum Command : uint8_t {
        RegisterHold = 0, Shift, TimeSet, TimeRead,
        Tp64Hz, Tp256Hz, Tp2048Hz, Tp4096Hz,
        Tp1sInterval, Tp10sInterval, Tp30sInterval, Tp60sInterval,
        IntervalReset, IntervalRun, IntervalStop, Test,
    };

    // Power-on state, with the calendar set to `now`.
    void reset(const DateTime& now);
    void setTime(const DateTime& time);
    [[nodiscard]] DateTime time() const;
    [[nodiscard]] static DateTime hostLocalTime();

    // Write to 0x380051: bit 0 = DATA IN, bit 1 = CLK, bit 2 = STB.
    void write(uint8_t bits);

    [[nodiscard]] bool tp() const noexcept { return tp_; }
    [[nodiscard]] bool dataOut() const noexcept { return dataOut_; }

    // Advances emulated time by `ticks` periods of the 32.768 kHz crystal.
    void advance(uint32_t ticks);

private:
    // A free-running toggle/expiry timer in 32.768 kHz ticks.
    struct Timer {
        bool     enabled   = false;
        uint32_t period    = 0;
        uint32_t remaining = 0;
        // MAME's adjust(zero, 0, period): fires immediately, then every period.
        void start(uint32_t p) noexcept { enabled = true; period = p; remaining = p; }
        // Returns how many times the timer expired during `ticks`.
        uint32_t advance(uint32_t ticks) noexcept;
    };

    void strobe();
    void clockIn();
    void advanceSeconds(uint32_t count);
    void releaseCounter();

    std::array<uint8_t, 6> counter_{};  // live calendar, same layout as the data register
    std::array<uint8_t, 6> shift_{};    // 48-bit data register
    uint8_t                command_  = 0;  // 4-bit serial command register
    uint8_t                mode_     = RegisterHold;

    bool dataIn_ = false, clk_ = false, stb_ = false;
    bool tp_ = false, dataOut_ = false;
    bool counterHeld_ = false;

    Timer second_;   // calendar: one expiry per second
    Timer tpTimer_;  // TP output: one toggle per expiry
    Timer dataOutTimer_;
};

}  // namespace snk
