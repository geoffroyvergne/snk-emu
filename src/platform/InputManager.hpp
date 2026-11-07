#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <SDL.h>

#include "input/InputPorts.hpp"

namespace snk::platform {

// One keyboard key -> one Neo-Geo input.
struct KeyBinding {
    SDL_Scancode key;
    int          player;  // 0 or 1 for joystick bits; -1 for system buttons
    uint16_t     mask;    // JoyBit (player >= 0) or SystemButton (player == -1)
};

// Neo-Geo actions a gamepad button can trigger. Directions always come from
// the D-pad / hat and the left stick in addition to any button mapped here.
enum class PadAction : uint8_t { A, B, C, D, Start, Coin, Select, Count };
inline constexpr size_t kPadActionCount = static_cast<size_t>(PadAction::Count);

// Button assignments, used for both players.
struct PadMapping {
    // Recognised game controllers (SDL's standard layout: Xbox, PlayStation,
    // Switch Pro, 8BitDo, ...). SDL_CONTROLLER_BUTTON_INVALID = unassigned.
    std::array<SDL_GameControllerButton, kPadActionCount> controller{
        SDL_CONTROLLER_BUTTON_A,     // A: bottom face button
        SDL_CONTROLLER_BUTTON_B,     // B: right
        SDL_CONTROLLER_BUTTON_X,     // C: left
        SDL_CONTROLLER_BUTTON_Y,     // D: top
        SDL_CONTROLLER_BUTTON_START,
        SDL_CONTROLLER_BUTTON_BACK,  // Coin: Back / Select / Share / Minus
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
    };
    // Any other joystick (arcade sticks, generic USB pads, encoders): raw
    // button numbers, -1 = unassigned. Directions from hat 0 or axes 0/1.
    std::array<int, kPadActionCount> joystick{0, 1, 2, 3, 9, 8, -1};
    int16_t deadzone = 12'000;  // stick deflection (0..32767) that counts as a direction
};

// Translates keyboard and gamepads into the Neo-Geo's InputState.
//
// Keyboard: fixed bindings by scancode (physical position: same on QWERTY
// and AZERTY). Gamepads: the first two devices become players 1 and 2
// (hot-pluggable); each player's Coin button inserts a coin in that
// player's slot. Recognised controllers use SDL's standard layout; anything
// else is read as a raw joystick so arcade sticks work too.
//
// Configuration lives in the user data folder:
//   controls.ini          pad button mapping (written with defaults if missing)
//   gamecontrollerdb.txt  optional community mappings (github.com/mdqinc/SDL_GameControllerDB)
//
// Usage, once per frame:
//   platform.pollEvents([&](const SDL_Event& e) { input.handleEvent(e); });
//   system.inputs().setState(input.sample());
class InputManager {
public:
    // configDir: where controls.ini / gamecontrollerdb.txt live; empty = no files.
    explicit InputManager(const std::filesystem::path& configDir = {});
    ~InputManager();

    InputManager(const InputManager&)            = delete;
    InputManager& operator=(const InputManager&) = delete;

    // Pad hotplug. SDL also reports pads connected at startup this way.
    void handleEvent(const SDL_Event& event);

    // Reads keyboard + pads into one snapshot (an input counts if either is pressed).
    [[nodiscard]] InputState sample() const;

    void setKeyBindings(std::vector<KeyBinding> bindings) { keyBindings_ = std::move(bindings); }
    [[nodiscard]] static std::vector<KeyBinding> defaultKeyBindings();

    void setPadMapping(const PadMapping& mapping) noexcept { mapping_ = mapping; }
    [[nodiscard]] const PadMapping& padMapping() const noexcept { return mapping_; }

    // controls.ini <-> PadMapping. Unknown keys/values are reported, not fatal.
    static PadMapping parseControls(const std::string& text, std::vector<std::string>* warnings = nullptr);
    [[nodiscard]] static std::string formatControls(const PadMapping& mapping);

    // Players with a pad attached, and a short description per player.
    [[nodiscard]] int connectedPads() const noexcept;
    [[nodiscard]] std::string padName(int player) const;

private:
    struct Pad {
        SDL_GameController* controller = nullptr;  // set for recognised controllers
        SDL_Joystick*       joystick   = nullptr;  // always set while attached
        SDL_JoystickID      id         = -1;
        void close() noexcept;
    };

    void openDevice(int deviceIndex, bool asController);
    void closeDevice(SDL_JoystickID instanceId);
    void sampleController(const Pad& pad, int player, InputState& state) const;
    void sampleJoystick(const Pad& pad, int player, InputState& state) const;
    void applyAction(PadAction action, int player, InputState& state) const;

    std::vector<KeyBinding> keyBindings_;
    PadMapping              mapping_;
    std::array<Pad, 2>      pads_{};  // slot = player
};

}  // namespace snk::platform
