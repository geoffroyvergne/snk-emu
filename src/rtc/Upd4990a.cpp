#include "rtc/Upd4990a.hpp"

#include <ctime>

namespace snk {

namespace {

constexpr uint32_t kSecond = Upd4990a::kClockHz;

constexpr uint8_t toBcd(int v) noexcept { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }
constexpr int fromBcd(uint8_t v) noexcept { return (v >> 4) * 10 + (v & 0x0F); }

int daysInMonth(int month, int year) noexcept {
    static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && year % 4 == 0) return 29;  // 2000-2099: every 4th year
    return (month >= 1 && month <= 12) ? kDays[month - 1] : 31;
}

}  // namespace

// ---------------------------------------------------------------------------
// Timer
// ---------------------------------------------------------------------------
uint32_t Upd4990a::Timer::advance(uint32_t ticks) noexcept {
    if (!enabled || period == 0) {
        return 0;
    }
    if (ticks < remaining) {
        remaining -= ticks;
        return 0;
    }
    ticks -= remaining;
    const uint32_t expired = 1 + ticks / period;
    remaining = period - ticks % period;
    return expired;
}

// ---------------------------------------------------------------------------
// Calendar
// ---------------------------------------------------------------------------
Upd4990a::DateTime Upd4990a::hostLocalTime() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    DateTime t;
    t.year      = 1900 + tm.tm_year;
    t.month     = tm.tm_mon + 1;
    t.day       = tm.tm_mday;
    t.dayOfWeek = tm.tm_wday;
    t.hour      = tm.tm_hour;
    t.minute    = tm.tm_min;
    t.second    = tm.tm_sec;
    return t;
}

void Upd4990a::setTime(const DateTime& t) {
    counter_[0] = toBcd(t.second);
    counter_[1] = toBcd(t.minute);
    counter_[2] = toBcd(t.hour);
    counter_[3] = toBcd(t.day);
    counter_[4] = static_cast<uint8_t>((t.month << 4) | (t.dayOfWeek & 0x0F));
    counter_[5] = toBcd(t.year % 100);
}

Upd4990a::DateTime Upd4990a::time() const {
    DateTime t;
    t.second    = fromBcd(counter_[0]);
    t.minute    = fromBcd(counter_[1]);
    t.hour      = fromBcd(counter_[2]);
    t.day       = fromBcd(counter_[3]);
    t.month     = counter_[4] >> 4;
    t.dayOfWeek = counter_[4] & 0x0F;
    t.year      = 2000 + fromBcd(counter_[5]);
    return t;
}

void Upd4990a::advanceSeconds(uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        DateTime t = time();
        if (++t.second < 60) { setTime(t); continue; }
        t.second = 0;
        if (++t.minute < 60) { setTime(t); continue; }
        t.minute = 0;
        if (++t.hour < 24) { setTime(t); continue; }
        t.hour      = 0;
        t.dayOfWeek = (t.dayOfWeek + 1) % 7;
        if (++t.day > daysInMonth(t.month, t.year)) {
            t.day = 1;
            if (++t.month > 12) {
                t.month = 1;
                t.year  = 2000 + (t.year + 1) % 100;
            }
        }
        setTime(t);
    }
}

// ---------------------------------------------------------------------------
// Power-on / time base
// ---------------------------------------------------------------------------
void Upd4990a::reset(const DateTime& now) {
    shift_.fill(0);
    command_ = 0;
    mode_    = RegisterHold;
    dataIn_ = clk_ = stb_ = false;
    tp_ = dataOut_ = false;
    counterHeld_ = false;
    setTime(now);
    second_.start(kSecond);
    tpTimer_      = {};  // TP stays low until the first command (see strobe())
    dataOutTimer_ = {};
}

void Upd4990a::advance(uint32_t ticks) {
    advanceSeconds(second_.advance(ticks));
    if (tpTimer_.advance(ticks) & 1u) {
        tp_ = !tp_;
    }
    if (dataOutTimer_.advance(ticks) & 1u) {
        dataOut_ = !dataOut_;
    }
}

// Time Set holds the upper divider stages; on release the next second comes
// 63/64 s later (the uPD4990A's +/-15.625 ms setting accuracy).
void Upd4990a::releaseCounter() {
    if (!counterHeld_) {
        return;
    }
    counterHeld_ = false;
    second_.enabled   = true;
    second_.remaining = kSecond / 64 * 63;
}

// ---------------------------------------------------------------------------
// Serial interface
// ---------------------------------------------------------------------------
void Upd4990a::write(uint8_t bits) {
    dataIn_ = (bits & 0x01) != 0;
    const bool clk = (bits & 0x02) != 0;
    const bool stb = (bits & 0x04) != 0;
    if (clk && !clk_) {
        clockIn();
    }
    clk_ = clk;
    if (stb && !stb_) {
        strobe();
    }
    stb_ = stb;
}

void Upd4990a::clockIn() {
    // The command register always shifts; its outgoing bit feeds the data register.
    const uint8_t carry = command_ & 1u;
    command_ = static_cast<uint8_t>((command_ >> 1) | (dataIn_ ? 0x08 : 0x00));
    if (mode_ == Shift) {
        for (size_t i = 0; i < shift_.size(); ++i) {
            const uint8_t next = (i + 1 < shift_.size()) ? static_cast<uint8_t>((shift_[i + 1] & 1u) << 7)
                                                         : static_cast<uint8_t>(carry << 7);
            shift_[i] = static_cast<uint8_t>((shift_[i] >> 1) | next);
        }
        dataOut_ = (shift_[0] & 1u) != 0;
    }
}

void Upd4990a::strobe() {
    mode_ = command_;

    switch (mode_) {
        case RegisterHold:
            releaseCounter();
            dataOutTimer_.start(kSecond / 2);  // 1 Hz square wave on DATA OUT
            dataOut_ = !dataOut_;
            if (!tpTimer_.enabled) {
                tpTimer_.start(kSecond / 128);  // a TP never configured starts at 64 Hz
                tp_ = !tp_;
            }
            break;
        case Shift:
            releaseCounter();
            dataOutTimer_.enabled = false;
            dataOut_ = (shift_[0] & 1u) != 0;
            break;
        case TimeSet:
            second_.enabled = false;  // held until Register Hold / Shift / Time Read
            counterHeld_    = true;
            dataOutTimer_.enabled = false;
            dataOut_ = (shift_[0] & 1u) != 0;
            counter_ = shift_;
            break;
        case TimeRead:
            releaseCounter();
            shift_ = counter_;
            dataOutTimer_.start(kSecond / 2);  // 1 Hz on the uPD4990A
            dataOut_ = !dataOut_;
            break;
        case Tp64Hz:   tpTimer_.start(kSecond / 128);  tp_ = !tp_; break;  // half periods
        case Tp256Hz:  tpTimer_.start(kSecond / 512);  tp_ = !tp_; break;
        case Tp2048Hz: tpTimer_.start(kSecond / 4096); tp_ = !tp_; break;
        case Tp4096Hz: tpTimer_.start(kSecond / 8192); tp_ = !tp_; break;
        case Tp1sInterval:  tpTimer_.start(kSecond / 2);  tp_ = !tp_; break;
        case Tp10sInterval: tpTimer_.start(kSecond * 5);  tp_ = !tp_; break;
        case Tp30sInterval: tpTimer_.start(kSecond * 15); tp_ = !tp_; break;
        case Tp60sInterval: tpTimer_.start(kSecond * 30); tp_ = !tp_; break;
        default:
            break;  // interval reset/run/stop and test mode: not used by the Neo-Geo BIOS
    }
}

}  // namespace snk
