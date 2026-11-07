#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <utility>

#include "audio/AudioTypes.hpp"
#include "core/Hardware.hpp"

namespace snk {

// Yamaha YM2610 (OPNB) @ 8 MHz: 4 FM, 3 SSG, 6 ADPCM-A, 1 ADPCM-B channels.
//
// Backend: ymfm (Aaron Giles), the same core MAME uses, at "medium" fidelity
// so it produces exactly one sample per 144 input clocks (~55.5 kHz) -- the
// rate SoundSystem schedules. The chip's two timers (which pace every sound
// driver's music) are run here in input clocks and raise the IRQ line wired
// to the Z80's /INT. ymfm stays out of this header.
//
// Output mix follows MAME's Neo-Geo routing: FM+ADPCM left/right at 0.98,
// SSG (mono) at 0.84 into both sides.
class Ym2610 {
public:
    static constexpr uint32_t kClockHz      = hw::kYm2610ClockHz;
    static constexpr uint32_t kSampleRateHz = hw::kYm2610SampleRateHz;

    using IrqHandler = std::function<void(bool asserted)>;

    Ym2610();
    ~Ym2610();

    Ym2610(const Ym2610&)            = delete;
    Ym2610& operator=(const Ym2610&) = delete;

    void reset();

    void setIrqHandler(IrqHandler handler);

    // ADPCM-A and ADPCM-B sample memory (V ROMs). Most MVS games use the same
    // ROMs for both. Spans must stay valid until the next call.
    void setAdpcmRoms(std::span<const uint8_t> adpcmA, std::span<const uint8_t> adpcmB) noexcept;

    // Port 0: address A, 1: data A, 2: address B, 3: data B (Z80 ports 0x04-0x07).
    void write(uint8_t port, uint8_t value);
    // Port 0: status A, 1: SSG register data, 2: status B (ADPCM end flags).
    [[nodiscard]] uint8_t read(uint8_t port);

    // Produces out.size() samples at kSampleRateHz, advancing the chip and its timers.
    void generate(std::span<StereoFrame> out);

    // Last value written to a register (bank A: 0x000-0x0FF, bank B: 0x100-0x1FF).
    [[nodiscard]] uint8_t registerValue(uint32_t index) const noexcept { return registers_[index & 0x1FF]; }

private:
    class Engine;  // ymfm::ym2610 + ymfm_interface glue

    std::unique_ptr<Engine>    engine_;
    std::array<uint8_t, 0x200> registers_{};
    uint8_t                    addressA_ = 0;
    uint8_t                    addressB_ = 0;
};

}  // namespace snk
