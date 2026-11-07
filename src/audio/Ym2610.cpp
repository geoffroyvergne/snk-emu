// ymfm-backed implementation of snk::Ym2610.

#include "audio/Ym2610.hpp"

#include <algorithm>

#include "ymfm_opn.h"

namespace snk {

// ymfm talks to the outside world through ymfm_interface: timers, IRQ and
// ADPCM sample reads land here.
class Ym2610::Engine final : public ymfm::ymfm_interface {
public:
    Engine() : chip_(*this) {
        chip_.set_fidelity(ymfm::OPN_FIDELITY_MED);  // clock / 144, one output per scheduler sample
    }

    void reset() {
        timers_ = {};
        chip_.reset();
    }

    ymfm::ym2610& chip() noexcept { return chip_; }

    void setIrqHandler(IrqHandler handler) { irq_ = std::move(handler); }
    void setAdpcm(std::span<const uint8_t> a, std::span<const uint8_t> b) noexcept {
        adpcmA_ = a;
        adpcmB_ = b;
    }

    // Runs both timers forward by `clocks` input clocks.
    void advanceTimers(int32_t clocks) {
        for (uint32_t t = 0; t < timers_.size(); ++t) {
            Timer& timer = timers_[t];
            if (!timer.active) continue;
            timer.remaining -= clocks;
            if (timer.remaining <= 0) {
                const int32_t late = -timer.remaining;
                timer.active = false;
                m_engine->engine_timer_expired(t);  // usually re-arms via ymfm_set_timer
                if (timer.active) timer.remaining -= late;
            }
        }
    }

    // ---- ymfm_interface ------------------------------------------------------
    void ymfm_set_timer(uint32_t tnum, int32_t durationInClocks) override {
        if (tnum >= timers_.size()) return;
        timers_[tnum].active    = durationInClocks >= 0;
        timers_[tnum].remaining = durationInClocks;
    }

    // The busy flag is never reported: sound drivers poll it after register
    // writes, and with per-scanline scheduling a busy window would stall them
    // for a whole line. Writes are applied instantly, so nothing is lost.
    bool ymfm_is_busy() override { return false; }

    void ymfm_update_irq(bool asserted) override {
        if (irq_) irq_(asserted);
    }

    uint8_t ymfm_external_read(ymfm::access_class type, uint32_t address) override {
        const std::span<const uint8_t> rom = type == ymfm::ACCESS_ADPCM_A   ? adpcmA_
                                             : type == ymfm::ACCESS_ADPCM_B ? adpcmB_
                                                                             : std::span<const uint8_t>{};
        return address < rom.size() ? rom[address] : uint8_t{0};
    }

private:
    struct Timer {
        bool    active    = false;
        int32_t remaining = 0;
    };

    ymfm::ym2610             chip_;
    std::array<Timer, 2>     timers_{};
    IrqHandler               irq_;
    std::span<const uint8_t> adpcmA_;
    std::span<const uint8_t> adpcmB_;
};

Ym2610::Ym2610() : engine_(std::make_unique<Engine>()) {
    engine_->reset();
}

Ym2610::~Ym2610() = default;

void Ym2610::reset() {
    registers_.fill(0);
    addressA_ = addressB_ = 0;
    engine_->reset();
}

void Ym2610::setIrqHandler(IrqHandler handler) {
    engine_->setIrqHandler(std::move(handler));
}

void Ym2610::setAdpcmRoms(std::span<const uint8_t> adpcmA, std::span<const uint8_t> adpcmB) noexcept {
    engine_->setAdpcm(adpcmA, adpcmB);
}

void Ym2610::write(uint8_t port, uint8_t value) {
    switch (port & 3) {
        case 0: addressA_ = value; break;
        case 1: registers_[addressA_] = value; break;
        case 2: addressB_ = value; break;
        case 3: registers_[0x100u + addressB_] = value; break;
    }
    engine_->chip().write(port & 3u, value);
}

uint8_t Ym2610::read(uint8_t port) {
    return engine_->chip().read(port & 3u);
}

void Ym2610::generate(std::span<StereoFrame> out) {
    ymfm::ym2610::output_data sample{};
    for (StereoFrame& frame : out) {
        engine_->chip().generate(&sample, 1);
        engine_->advanceTimers(static_cast<int32_t>(hw::kYm2610ClocksPerSample));

        // data[0]/[1]: FM + ADPCM left/right, data[2]: SSG (mono).
        const int32_t ssg   = sample.data[2] * 84 / 100;
        const int32_t left  = sample.data[0] * 98 / 100 + ssg;
        const int32_t right = sample.data[1] * 98 / 100 + ssg;
        frame.left  = static_cast<int16_t>(std::clamp(left, -32768, 32767));
        frame.right = static_cast<int16_t>(std::clamp(right, -32768, 32767));
    }
}

}  // namespace snk
