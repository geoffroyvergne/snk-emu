#include "platform/InputManager.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

namespace snk::platform {

namespace {

constexpr std::array<const char*, kPadActionCount> kActionNames = {"a", "b", "c", "d", "start", "coin", "select"};

constexpr uint16_t kStart[2]  = {SystemButton::Start1, SystemButton::Start2};
constexpr uint16_t kSelect[2] = {SystemButton::Select1, SystemButton::Select2};
constexpr uint16_t kCoin[2]   = {SystemButton::Coin1, SystemButton::Coin2};

std::string trim(std::string s) {
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction / configuration files
// ---------------------------------------------------------------------------
InputManager::InputManager(const std::filesystem::path& configDir) : keyBindings_(defaultKeyBindings()) {
    if (configDir.empty()) {
        return;
    }
    std::error_code ec;

    // Community controller mappings: recognise far more pads as game controllers.
    const auto db = configDir / "gamecontrollerdb.txt";
    if (std::filesystem::is_regular_file(db, ec)) {
        const int added = SDL_GameControllerAddMappingsFromFile(db.string().c_str());
        if (added >= 0) {
            SDL_Log("Input: %d controller mappings from %s", added, db.string().c_str());
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "Input: %s: %s", db.string().c_str(), SDL_GetError());
        }
    }

    // Button mapping: read controls.ini, or write it with the defaults so it can be edited.
    const auto ini = configDir / "controls.ini";
    if (std::ifstream in{ini}) {
        std::stringstream text;
        text << in.rdbuf();
        std::vector<std::string> warnings;
        mapping_ = parseControls(text.str(), &warnings);
        for (const auto& w : warnings) SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "Input: %s: %s", ini.string().c_str(), w.c_str());
        SDL_Log("Input: pad mapping from %s", ini.string().c_str());
    } else {
        std::filesystem::create_directories(configDir, ec);
        if (std::ofstream out{ini}) {
            out << formatControls(mapping_);
            SDL_Log("Input: wrote default pad mapping to %s", ini.string().c_str());
        }
    }
}

InputManager::~InputManager() {
    for (Pad& pad : pads_) pad.close();
}

void InputManager::Pad::close() noexcept {
    if (controller != nullptr) {
        SDL_GameControllerClose(controller);  // also closes its joystick
    } else if (joystick != nullptr) {
        SDL_JoystickClose(joystick);
    }
    controller = nullptr;
    joystick   = nullptr;
    id         = -1;
}

// Format:  [controller] / [joystick] sections, "action = value" lines, '#' comments.
// Controller values are SDL button names (a b x y back guide start leftstick
// rightstick leftshoulder rightshoulder dpup dpdown dpleft dpright misc1
// paddle1-4 touchpad); joystick values are button numbers; "none" unassigns.
PadMapping InputManager::parseControls(const std::string& text, std::vector<std::string>* warnings) {
    PadMapping mapping;
    std::istringstream in(text);
    std::string        line, section;
    int                lineNo = 0;
    const auto warn = [&](const std::string& msg) {
        if (warnings) warnings->push_back("line " + std::to_string(lineNo) + ": " + msg);
    };
    while (std::getline(in, line)) {
        ++lineNo;
        if (const auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = lower(trim(line.substr(1, line.size() - 2)));
            if (section != "controller" && section != "joystick") warn("unknown section [" + section + "]");
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) { warn("expected 'action = value'"); continue; }
        const std::string key   = lower(trim(line.substr(0, eq)));
        const std::string value = lower(trim(line.substr(eq + 1)));

        if (key == "deadzone") {
            const long dz = std::strtol(value.c_str(), nullptr, 10);
            if (dz >= 0 && dz <= 32767) mapping.deadzone = static_cast<int16_t>(dz);
            else warn("deadzone must be 0..32767");
            continue;
        }
        const auto it = std::find(kActionNames.begin(), kActionNames.end(), key);
        if (it == kActionNames.end()) { warn("unknown action '" + key + "'"); continue; }
        const auto action = static_cast<size_t>(it - kActionNames.begin());

        if (section == "controller") {
            if (value == "none") { mapping.controller[action] = SDL_CONTROLLER_BUTTON_INVALID; continue; }
            const SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(value.c_str());
            if (b == SDL_CONTROLLER_BUTTON_INVALID) warn("unknown controller button '" + value + "'");
            else mapping.controller[action] = b;
        } else if (section == "joystick") {
            if (value == "none") { mapping.joystick[action] = -1; continue; }
            char* end = nullptr;
            const long n = std::strtol(value.c_str(), &end, 10);
            if (end == value.c_str() || *end != '\0' || n < 0 || n > 127) warn("joystick button must be a number 0..127");
            else mapping.joystick[action] = static_cast<int>(n);
        } else {
            warn("'" + key + "' outside of [controller] or [joystick]");
        }
    }
    return mapping;
}

std::string InputManager::formatControls(const PadMapping& mapping) {
    std::ostringstream out;
    out << "# snkemu gamepad mapping (both players). Edit and restart to apply.\n"
           "# Directions always come from the D-pad / hat and the left stick.\n"
           "# 'coin' inserts a coin in that player's slot (pad 1 -> coin 1, pad 2 -> coin 2).\n"
           "\n"
           "# Recognised game controllers, SDL standard layout (Xbox names: a = bottom,\n"
           "# b = right, x = left, y = top). Values: a b x y back guide start leftstick\n"
           "# rightstick leftshoulder rightshoulder dpup dpdown dpleft dpright misc1\n"
           "# paddle1 paddle2 paddle3 paddle4 touchpad, or none.\n"
           "[controller]\n";
    for (size_t i = 0; i < kPadActionCount; ++i) {
        const char* name = mapping.controller[i] == SDL_CONTROLLER_BUTTON_INVALID
                               ? "none" : SDL_GameControllerGetStringForButton(mapping.controller[i]);
        out << kActionNames[i] << " = " << (name ? name : "none") << "\n";
    }
    out << "deadzone = " << mapping.deadzone << "   # left stick, 0..32767\n"
           "\n"
           "# Other joysticks (arcade sticks, generic USB pads): raw button numbers\n"
           "# (0-based, as reported by the device), or none.\n"
           "[joystick]\n";
    for (size_t i = 0; i < kPadActionCount; ++i) {
        out << kActionNames[i] << " = ";
        if (mapping.joystick[i] < 0) out << "none"; else out << mapping.joystick[i];
        out << "\n";
    }
    return out.str();
}

// Defaults follow MAME's long-standing layout where possible.
std::vector<KeyBinding> InputManager::defaultKeyBindings() {
    return {
        // Player 1: arrows + Z X C V
        {SDL_SCANCODE_UP, 0, JoyBit::Up},       {SDL_SCANCODE_DOWN, 0, JoyBit::Down},
        {SDL_SCANCODE_LEFT, 0, JoyBit::Left},   {SDL_SCANCODE_RIGHT, 0, JoyBit::Right},
        {SDL_SCANCODE_Z, 0, JoyBit::A},         {SDL_SCANCODE_X, 0, JoyBit::B},
        {SDL_SCANCODE_C, 0, JoyBit::C},         {SDL_SCANCODE_V, 0, JoyBit::D},
        // Player 2: R F D G + A S Q W
        {SDL_SCANCODE_R, 1, JoyBit::Up},        {SDL_SCANCODE_F, 1, JoyBit::Down},
        {SDL_SCANCODE_D, 1, JoyBit::Left},      {SDL_SCANCODE_G, 1, JoyBit::Right},
        {SDL_SCANCODE_A, 1, JoyBit::A},         {SDL_SCANCODE_S, 1, JoyBit::B},
        {SDL_SCANCODE_Q, 1, JoyBit::C},         {SDL_SCANCODE_W, 1, JoyBit::D},
        // System
        {SDL_SCANCODE_1, -1, SystemButton::Start1},  {SDL_SCANCODE_2, -1, SystemButton::Start2},
        {SDL_SCANCODE_5, -1, SystemButton::Coin1},   {SDL_SCANCODE_6, -1, SystemButton::Coin2},
        {SDL_SCANCODE_7, -1, SystemButton::Coin3},   {SDL_SCANCODE_8, -1, SystemButton::Coin4},
        {SDL_SCANCODE_TAB, -1, SystemButton::Select1},
        {SDL_SCANCODE_9, -1, SystemButton::Service},
        {SDL_SCANCODE_F2, -1, SystemButton::Test},
    };
}

// ---------------------------------------------------------------------------
// Hotplug
// ---------------------------------------------------------------------------
void InputManager::handleEvent(const SDL_Event& event) {
    switch (event.type) {
        case SDL_CONTROLLERDEVICEADDED:
            openDevice(event.cdevice.which, true);  // device index
            break;
        case SDL_JOYDEVICEADDED:
            // Recognised controllers also get CONTROLLERDEVICEADDED; take the
            // rest here as raw joysticks.
            if (!SDL_IsGameController(event.jdevice.which)) {
                openDevice(event.jdevice.which, false);
            }
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            closeDevice(event.cdevice.which);  // instance id
            break;
        case SDL_JOYDEVICEREMOVED:
            closeDevice(event.jdevice.which);
            break;
        default:
            break;
    }
}

void InputManager::openDevice(int deviceIndex, bool asController) {
    const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(deviceIndex);
    for (const Pad& pad : pads_) {
        if (pad.joystick != nullptr && pad.id == id) return;  // already open (SDL reports both event kinds)
    }
    for (size_t player = 0; player < pads_.size(); ++player) {
        Pad& pad = pads_[player];
        if (pad.joystick != nullptr) continue;
        if (asController) {
            pad.controller = SDL_GameControllerOpen(deviceIndex);
            pad.joystick   = pad.controller ? SDL_GameControllerGetJoystick(pad.controller) : nullptr;
        } else {
            pad.joystick = SDL_JoystickOpen(deviceIndex);
        }
        if (pad.joystick == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "Input: cannot open device %d: %s", deviceIndex, SDL_GetError());
            pad.close();
            return;
        }
        pad.id = SDL_JoystickInstanceID(pad.joystick);
        SDL_Log("Input: player %zu <- %s", player + 1, padName(static_cast<int>(player)).c_str());
        return;
    }
    SDL_Log("Input: \"%s\" ignored (both players already have a pad)", SDL_JoystickNameForIndex(deviceIndex));
}

void InputManager::closeDevice(SDL_JoystickID instanceId) {
    for (size_t player = 0; player < pads_.size(); ++player) {
        if (pads_[player].joystick != nullptr && pads_[player].id == instanceId) {
            SDL_Log("Input: player %zu pad disconnected", player + 1);
            pads_[player].close();
        }
    }
}

int InputManager::connectedPads() const noexcept {
    int count = 0;
    for (const Pad& pad : pads_) count += pad.joystick != nullptr ? 1 : 0;
    return count;
}

std::string InputManager::padName(int player) const {
    if (player < 0 || player >= static_cast<int>(pads_.size())) return {};
    const Pad& pad = pads_[static_cast<size_t>(player)];
    if (pad.joystick == nullptr) return {};
    const char* name = pad.controller ? SDL_GameControllerName(pad.controller) : SDL_JoystickName(pad.joystick);
    return std::string(name ? name : "unnamed pad") + (pad.controller ? " (controller)" : " (raw joystick)");
}

// ---------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------
InputState InputManager::sample() const {
    InputState state;

    // Keyboard. SDL keeps this array current through the event pump and
    // clears it when the window loses focus, so keys can't get stuck.
    int            keyCount = 0;
    const uint8_t* keys     = SDL_GetKeyboardState(&keyCount);
    for (const KeyBinding& b : keyBindings_) {
        if (b.key >= keyCount || !keys[b.key]) continue;
        if (b.player >= 0) {
            auto& joy = state.joystick[static_cast<size_t>(b.player)];
            joy = static_cast<uint8_t>(joy | b.mask);
        } else {
            state.system = static_cast<uint16_t>(state.system | b.mask);
        }
    }

    for (size_t player = 0; player < pads_.size(); ++player) {
        const Pad& pad = pads_[player];
        if (pad.controller != nullptr) {
            sampleController(pad, static_cast<int>(player), state);
        } else if (pad.joystick != nullptr) {
            sampleJoystick(pad, static_cast<int>(player), state);
        }
    }
    return state;
}

void InputManager::applyAction(PadAction action, int player, InputState& state) const {
    const auto p = static_cast<size_t>(player);
    auto&      joy = state.joystick[p];
    switch (action) {
        case PadAction::A:      joy = static_cast<uint8_t>(joy | JoyBit::A); break;
        case PadAction::B:      joy = static_cast<uint8_t>(joy | JoyBit::B); break;
        case PadAction::C:      joy = static_cast<uint8_t>(joy | JoyBit::C); break;
        case PadAction::D:      joy = static_cast<uint8_t>(joy | JoyBit::D); break;
        case PadAction::Start:  state.system = static_cast<uint16_t>(state.system | kStart[p]); break;
        case PadAction::Coin:   state.system = static_cast<uint16_t>(state.system | kCoin[p]); break;
        case PadAction::Select: state.system = static_cast<uint16_t>(state.system | kSelect[p]); break;
        case PadAction::Count:  break;
    }
}

void InputManager::sampleController(const Pad& pad, int player, InputState& state) const {
    SDL_GameController* c   = pad.controller;
    auto&               joy = state.joystick[static_cast<size_t>(player)];
    const auto          dz  = mapping_.deadzone;

    // Directions: D-pad plus left stick.
    const int x = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    const int y = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP) || y < -dz)    joy = static_cast<uint8_t>(joy | JoyBit::Up);
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || y > dz)   joy = static_cast<uint8_t>(joy | JoyBit::Down);
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || x < -dz)  joy = static_cast<uint8_t>(joy | JoyBit::Left);
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || x > dz)  joy = static_cast<uint8_t>(joy | JoyBit::Right);

    for (size_t a = 0; a < kPadActionCount; ++a) {
        const SDL_GameControllerButton b = mapping_.controller[a];
        if (b != SDL_CONTROLLER_BUTTON_INVALID && SDL_GameControllerGetButton(c, b)) {
            applyAction(static_cast<PadAction>(a), player, state);
        }
    }
}

void InputManager::sampleJoystick(const Pad& pad, int player, InputState& state) const {
    SDL_Joystick* j   = pad.joystick;
    auto&         joy = state.joystick[static_cast<size_t>(player)];
    const auto    dz  = mapping_.deadzone;

    // Directions: hat 0 (most arcade sticks and D-pads) plus axes 0/1.
    const Uint8 hat = SDL_JoystickNumHats(j) > 0 ? SDL_JoystickGetHat(j, 0) : SDL_HAT_CENTERED;
    const int   x   = SDL_JoystickNumAxes(j) > 0 ? SDL_JoystickGetAxis(j, 0) : 0;
    const int   y   = SDL_JoystickNumAxes(j) > 1 ? SDL_JoystickGetAxis(j, 1) : 0;
    if ((hat & SDL_HAT_UP) || y < -dz)    joy = static_cast<uint8_t>(joy | JoyBit::Up);
    if ((hat & SDL_HAT_DOWN) || y > dz)   joy = static_cast<uint8_t>(joy | JoyBit::Down);
    if ((hat & SDL_HAT_LEFT) || x < -dz)  joy = static_cast<uint8_t>(joy | JoyBit::Left);
    if ((hat & SDL_HAT_RIGHT) || x > dz)  joy = static_cast<uint8_t>(joy | JoyBit::Right);

    const int buttons = SDL_JoystickNumButtons(j);
    for (size_t a = 0; a < kPadActionCount; ++a) {
        const int b = mapping_.joystick[a];
        if (b >= 0 && b < buttons && SDL_JoystickGetButton(j, b)) {
            applyAction(static_cast<PadAction>(a), player, state);
        }
    }
}

}  // namespace snk::platform
