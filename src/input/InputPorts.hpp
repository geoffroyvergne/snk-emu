#pragma once

#include <array>
#include <cstdint>

namespace snk {

// ---------------------------------------------------------------------------
// Register bit layouts. On hardware every input is ACTIVE LOW: a pressed
// button reads as 0. The masks below name the bits; InputPorts handles the
// inversion, so the rest of the code works in positive logic (1 = pressed).
// ---------------------------------------------------------------------------

// REG_P1CNT (0x300000, even byte) and REG_P2CNT (0x340000, even byte).
struct JoyBit {
    static constexpr uint8_t Up    = 0x01;
    static constexpr uint8_t Down  = 0x02;
    static constexpr uint8_t Left  = 0x04;
    static constexpr uint8_t Right = 0x08;
    static constexpr uint8_t A     = 0x10;
    static constexpr uint8_t B     = 0x20;
    static constexpr uint8_t C     = 0x40;
    static constexpr uint8_t D     = 0x80;
};

// REG_STATUS_A (0x320001, odd byte). MVS only.
struct StatusABit {
    static constexpr uint8_t Coin1        = 0x01;  // active low
    static constexpr uint8_t Coin2        = 0x02;  // active low
    static constexpr uint8_t Service      = 0x04;  // active low
    static constexpr uint8_t Coin3        = 0x08;  // active low, 4/6-slot boards
    static constexpr uint8_t Coin4        = 0x10;  // active low, 4/6-slot boards
    static constexpr uint8_t BoardType    = 0x20;  // 4-slot vs 6-slot board
    static constexpr uint8_t RtcTimePulse = 0x40;  // uPD4990A TP output (merged in by MemoryBus)
    static constexpr uint8_t RtcData      = 0x80;  // uPD4990A DATA OUT (merged in by MemoryBus)
};

// REG_STATUS_B (0x380000, even byte).
struct StatusBBit {
    static constexpr uint8_t Start1        = 0x01;  // active low
    static constexpr uint8_t Select1       = 0x02;  // active low, AES
    static constexpr uint8_t Start2        = 0x04;  // active low
    static constexpr uint8_t Select2       = 0x08;  // active low, AES
    static constexpr uint8_t CardDetect1   = 0x10;  // 0 = memory card inserted
    static constexpr uint8_t CardDetect2   = 0x20;  // 0 = memory card inserted
    static constexpr uint8_t CardWriteProt = 0x40;  // 1 = write protected
    static constexpr uint8_t SystemMvs     = 0x80;  // 1 = MVS, 0 = AES
};

// REG_SYSTYPE (0x300081, odd byte, MVS).
struct SysTypeBit {
    static constexpr uint8_t MultiSlot  = 0x40;  // 1 = 4/6-slot board, 0 = 1/2-slot
    static constexpr uint8_t TestButton = 0x80;  // active low: enters the BIOS test menu
};

// REG_DIPSW (0x300001, odd byte, MVS). A switch set to ON reads as 0.
struct DipBit {
    static constexpr uint8_t SettingsMode = 0x01;
    static constexpr uint8_t CoinChutes   = 0x02;  // ON = 2 chutes
    static constexpr uint8_t Mahjong      = 0x04;  // ON = mahjong controller
    static constexpr uint8_t FreePlay     = 0x40;
    static constexpr uint8_t Freeze       = 0x80;
};

// Buttons that are not on a joystick port, in one positive-logic mask.
struct SystemButton {
    static constexpr uint16_t Start1  = 0x0001;
    static constexpr uint16_t Select1 = 0x0002;
    static constexpr uint16_t Start2  = 0x0004;
    static constexpr uint16_t Select2 = 0x0008;
    static constexpr uint16_t Coin1   = 0x0010;
    static constexpr uint16_t Coin2   = 0x0020;
    static constexpr uint16_t Coin3   = 0x0040;
    static constexpr uint16_t Coin4   = 0x0080;
    static constexpr uint16_t Service = 0x0100;
    static constexpr uint16_t Test    = 0x0200;
};

// What the frontend reports each frame. Positive logic: 1 = pressed.
struct InputState {
    std::array<uint8_t, 2> joystick{};  // JoyBit masks for players 1 and 2
    uint16_t               system = 0;  // SystemButton mask
};

enum class SystemType : uint8_t { Mvs, Aes };

// The machine's input hardware: turns an InputState into the exact bytes the
// 68000 reads. Bytes are recomputed on every update, so bus reads (which
// games do many times per frame) are a single load.
class InputPorts {
public:
    InputPorts() { recompute(); }

    // Latches a new input snapshot (typically once per frame).
    void setState(const InputState& state) noexcept {
        state_ = state;
        for (auto& joy : state_.joystick) {
            joy = cancelOpposites(joy);
        }
        recompute();
    }

    // Board configuration.
    void setSystemType(SystemType type) noexcept { systemType_ = type; recompute(); }
    void setMultiSlot(bool multiSlot) noexcept { multiSlot_ = multiSlot; recompute(); }
    void setDipSwitchesOn(uint8_t onMask) noexcept { dipOn_ = onMask; recompute(); }

    [[nodiscard]] const InputState& state() const noexcept { return state_; }
    [[nodiscard]] SystemType systemType() const noexcept { return systemType_; }

    // ---- Register values as the 68000 sees them (active low) ---------------
    [[nodiscard]] uint8_t p1() const noexcept { return p1_; }            // 0x300000
    [[nodiscard]] uint8_t dipSwitches() const noexcept { return dip_; }  // 0x300001
    [[nodiscard]] uint8_t sysType() const noexcept { return sysType_; }  // 0x300081
    [[nodiscard]] uint8_t statusA() const noexcept { return statusA_; }  // 0x320001
    [[nodiscard]] uint8_t p2() const noexcept { return p2_; }            // 0x340000
    [[nodiscard]] uint8_t statusB() const noexcept { return statusB_; }  // 0x380000

private:
    // A physical stick cannot report Up+Down or Left+Right; some games
    // misbehave if they see it, so both cancel out (keyboard players can press both).
    static constexpr uint8_t cancelOpposites(uint8_t joy) noexcept {
        if ((joy & (JoyBit::Up | JoyBit::Down)) == (JoyBit::Up | JoyBit::Down)) {
            joy = static_cast<uint8_t>(joy & ~(JoyBit::Up | JoyBit::Down));
        }
        if ((joy & (JoyBit::Left | JoyBit::Right)) == (JoyBit::Left | JoyBit::Right)) {
            joy = static_cast<uint8_t>(joy & ~(JoyBit::Left | JoyBit::Right));
        }
        return joy;
    }

    // Starts from all-released (0xFF) and pulls low each active input.
    static constexpr uint8_t activeLow(uint8_t idle, uint8_t pressedMask) noexcept {
        return static_cast<uint8_t>(idle & ~pressedMask);
    }

    void recompute() noexcept {
        const uint16_t sys = state_.system;
        const auto pressed = [sys](uint16_t button, uint8_t bit) -> uint8_t {
            return (sys & button) ? bit : uint8_t{0};
        };

        p1_  = activeLow(0xFF, state_.joystick[0]);
        p2_  = activeLow(0xFF, state_.joystick[1]);
        dip_ = activeLow(0xFF, dipOn_);

        statusA_ = activeLow(0xFF & ~(StatusABit::RtcTimePulse | StatusABit::RtcData),
                             static_cast<uint8_t>(pressed(SystemButton::Coin1, StatusABit::Coin1) |
                                                  pressed(SystemButton::Coin2, StatusABit::Coin2) |
                                                  pressed(SystemButton::Service, StatusABit::Service) |
                                                  pressed(SystemButton::Coin3, StatusABit::Coin3) |
                                                  pressed(SystemButton::Coin4, StatusABit::Coin4)));

        // No memory card inserted (detect bits high), not write-protected.
        uint8_t statusBIdle = 0xFF & ~StatusBBit::CardWriteProt;
        if (systemType_ == SystemType::Aes) {
            statusBIdle = static_cast<uint8_t>(statusBIdle & ~StatusBBit::SystemMvs);
        }
        statusB_ = activeLow(statusBIdle,
                             static_cast<uint8_t>(pressed(SystemButton::Start1, StatusBBit::Start1) |
                                                  pressed(SystemButton::Select1, StatusBBit::Select1) |
                                                  pressed(SystemButton::Start2, StatusBBit::Start2) |
                                                  pressed(SystemButton::Select2, StatusBBit::Select2)));

        uint8_t sysTypeIdle = 0xFF;
        if (!multiSlot_) {
            sysTypeIdle = static_cast<uint8_t>(sysTypeIdle & ~SysTypeBit::MultiSlot);
        }
        sysType_ = activeLow(sysTypeIdle, pressed(SystemButton::Test, SysTypeBit::TestButton));
    }

    InputState state_{};
    SystemType systemType_ = SystemType::Mvs;
    bool       multiSlot_  = false;
    uint8_t    dipOn_      = 0;  // all DIP switches OFF

    uint8_t p1_ = 0xFF, p2_ = 0xFF, dip_ = 0xFF, sysType_ = 0xFF, statusA_ = 0xFF, statusB_ = 0xFF;
};

}  // namespace snk
