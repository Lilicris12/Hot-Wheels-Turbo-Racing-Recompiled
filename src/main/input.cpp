// Controller and keyboard input mapped to the N64 controller.
//
// Default gamepad layout (Xbox naming; Steam Deck uses the same positions):
//   Left stick / D-pad  -> analog stick / N64 D-pad
//   A                   -> A          B or X  -> B
//   Y                   -> C-Up       Right stick -> C buttons
//   LB                  -> Z          RB      -> R
//   Back                -> L          Start   -> Start
//   RT / LT             -> A (gas) / B (brake) when "triggers_as_gas_brake" is on, else R / Z
//
// Keyboard: arrows = stick, X = A, C = B, Z = Z, A/S = L/R, Enter = Start, IJKL = C buttons,
//           WASD-free so it stays usable on laptops.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

#include "hwtr/hwtr.hpp"

#include "SDL.h"

namespace {
    enum N64Button : uint16_t {
        BTN_A       = 0x8000,
        BTN_B       = 0x4000,
        BTN_Z       = 0x2000,
        BTN_START   = 0x1000,
        BTN_DUP     = 0x0800,
        BTN_DDOWN   = 0x0400,
        BTN_DLEFT   = 0x0200,
        BTN_DRIGHT  = 0x0100,
        BTN_L       = 0x0020,
        BTN_R       = 0x0010,
        BTN_CUP     = 0x0008,
        BTN_CDOWN   = 0x0004,
        BTN_CLEFT   = 0x0002,
        BTN_CRIGHT  = 0x0001,
    };

    constexpr int max_players = 4;
    constexpr float trigger_threshold = 0.3f;
    constexpr float c_stick_threshold = 0.5f;

    struct PlayerState {
        uint16_t buttons = 0;
        float x = 0.0f;
        float y = 0.0f;
        bool connected = false;
    };

    std::mutex state_mutex;
    std::array<PlayerState, max_players> player_states{};
    std::array<SDL_GameController*, max_players> controllers{};
    std::array<std::atomic_bool, max_players> rumble_requested{};

    float apply_deadzone(float v, float deadzone) {
        float mag = std::fabs(v);
        if (mag < deadzone) {
            return 0.0f;
        }
        return std::copysign((mag - deadzone) / (1.0f - deadzone), v);
    }

    float axis(SDL_GameController* gc, SDL_GameControllerAxis a) {
        return SDL_GameControllerGetAxis(gc, a) / 32767.0f;
    }

    void assign_controller(SDL_GameController* gc) {
        for (auto& slot : controllers) {
            if (slot == gc) {
                return;
            }
        }
        for (auto& slot : controllers) {
            if (slot == nullptr) {
                slot = gc;
                return;
            }
        }
    }

    void remove_controller(SDL_JoystickID id) {
        for (auto& slot : controllers) {
            if (slot != nullptr && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(slot)) == id) {
                SDL_GameControllerClose(slot);
                slot = nullptr;
            }
        }
        // Keep player 1 occupied when another pad remains, so unplugging pad 1 doesn't leave
        // the game without input.
        std::stable_partition(controllers.begin(), controllers.end(), [](SDL_GameController* c) { return c != nullptr; });
    }

    PlayerState read_controller(SDL_GameController* gc, const hwtr::Config& cfg) {
        PlayerState s{};
        s.connected = true;

        auto btn = [gc](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(gc, b) != 0; };

        if (btn(SDL_CONTROLLER_BUTTON_A)) s.buttons |= BTN_A;
        if (btn(SDL_CONTROLLER_BUTTON_B) || btn(SDL_CONTROLLER_BUTTON_X)) s.buttons |= BTN_B;
        if (btn(SDL_CONTROLLER_BUTTON_Y)) s.buttons |= BTN_CUP;
        if (btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) s.buttons |= BTN_Z;
        if (btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) s.buttons |= BTN_R;
        if (btn(SDL_CONTROLLER_BUTTON_BACK)) s.buttons |= BTN_L;
        if (btn(SDL_CONTROLLER_BUTTON_START)) s.buttons |= BTN_START;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_UP)) s.buttons |= BTN_DUP;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) s.buttons |= BTN_DDOWN;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) s.buttons |= BTN_DLEFT;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) s.buttons |= BTN_DRIGHT;

        float lt = axis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        float rt = axis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        if (cfg.triggers_as_gas_brake) {
            if (rt > trigger_threshold) s.buttons |= BTN_A;
            if (lt > trigger_threshold) s.buttons |= BTN_B;
        }
        else {
            if (rt > trigger_threshold) s.buttons |= BTN_R;
            if (lt > trigger_threshold) s.buttons |= BTN_Z;
        }

        float rx = axis(gc, SDL_CONTROLLER_AXIS_RIGHTX);
        float ry = axis(gc, SDL_CONTROLLER_AXIS_RIGHTY);
        if (rx < -c_stick_threshold) s.buttons |= BTN_CLEFT;
        if (rx > c_stick_threshold) s.buttons |= BTN_CRIGHT;
        if (ry < -c_stick_threshold) s.buttons |= BTN_CUP;
        if (ry > c_stick_threshold) s.buttons |= BTN_CDOWN;

        s.x = apply_deadzone(axis(gc, SDL_CONTROLLER_AXIS_LEFTX), cfg.analog_deadzone);
        // SDL's Y axis points down, the N64's points up.
        s.y = -apply_deadzone(axis(gc, SDL_CONTROLLER_AXIS_LEFTY), cfg.analog_deadzone);
        return s;
    }

    // A Rumble Pak is reported so the game drives force feedback. Saves still work: the
    // Controller Pak API is served separately by src/game/controller_pak.cpp.
    ultramodern::input::Pak connected_pak() {
        return hwtr::config().rumble ? ultramodern::input::Pak::RumblePak : ultramodern::input::Pak::None;
    }

    void merge_keyboard(PlayerState& s) {
        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        auto key = [keys](SDL_Scancode sc) { return keys[sc] != 0; };

        if (key(SDL_SCANCODE_X)) s.buttons |= BTN_A;
        if (key(SDL_SCANCODE_C)) s.buttons |= BTN_B;
        if (key(SDL_SCANCODE_Z)) s.buttons |= BTN_Z;
        if (key(SDL_SCANCODE_A)) s.buttons |= BTN_L;
        if (key(SDL_SCANCODE_S)) s.buttons |= BTN_R;
        if (key(SDL_SCANCODE_RETURN)) s.buttons |= BTN_START;
        if (key(SDL_SCANCODE_I)) s.buttons |= BTN_CUP;
        if (key(SDL_SCANCODE_K)) s.buttons |= BTN_CDOWN;
        if (key(SDL_SCANCODE_J)) s.buttons |= BTN_CLEFT;
        if (key(SDL_SCANCODE_L)) s.buttons |= BTN_CRIGHT;

        float kx = (key(SDL_SCANCODE_RIGHT) ? 1.0f : 0.0f) - (key(SDL_SCANCODE_LEFT) ? 1.0f : 0.0f);
        float ky = (key(SDL_SCANCODE_UP) ? 1.0f : 0.0f) - (key(SDL_SCANCODE_DOWN) ? 1.0f : 0.0f);
        if (kx != 0.0f) s.x = kx;
        if (ky != 0.0f) s.y = ky;
        s.connected = true;
    }
}

void hwtr::input::init() {
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC);

    // Optional community mappings shipped next to the executable.
    std::filesystem::path db = get_program_path() / "gamecontrollerdb.txt";
    std::error_code ec;
    if (std::filesystem::is_regular_file(db, ec)) {
        SDL_GameControllerAddMappingsFromFile(db.string().c_str());
    }
}

void hwtr::input::handle_event(const SDL_Event& event) {
    switch (event.type) {
        case SDL_CONTROLLERDEVICEADDED:
            if (SDL_GameController* gc = SDL_GameControllerOpen(event.cdevice.which)) {
                assign_controller(gc);
            }
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            remove_controller(event.cdevice.which);
            break;
        default:
            break;
    }
}

void hwtr::input::update() {
    // Runs on the main thread after events are pumped, since SDL's controller API isn't
    // meant to be used from the game threads.
    const Config& cfg = config();
    std::array<PlayerState, max_players> new_states{};
    for (int i = 0; i < max_players; i++) {
        if (controllers[i] != nullptr) {
            new_states[i] = read_controller(controllers[i], cfg);
        }
    }
    merge_keyboard(new_states[0]);

    {
        std::lock_guard lock{ state_mutex };
        player_states = new_states;
    }

    // Re-arm a short rumble each update while the game holds the motor on.
    for (int i = 0; i < max_players; i++) {
        if (controllers[i] != nullptr) {
            uint16_t strength = (cfg.rumble && rumble_requested[i]) ? 0xC000 : 0;
            SDL_GameControllerRumble(controllers[i], strength, strength, strength ? 100 : 0);
        }
    }
}

void hwtr::input::poll_inputs() {
    // The state is refreshed by update() on the main thread; nothing to do here.
}

bool hwtr::input::get_n64_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (controller_num < 0 || controller_num >= max_players) {
        return false;
    }
    std::lock_guard lock{ state_mutex };
    const PlayerState& s = player_states[controller_num];
    if (!s.connected) {
        return false;
    }
    *buttons = s.buttons;
    *x = std::clamp(s.x, -1.0f, 1.0f);
    *y = std::clamp(s.y, -1.0f, 1.0f);
    return true;
}

void hwtr::input::set_rumble(int controller_num, bool rumble) {
    if (controller_num >= 0 && controller_num < max_players) {
        rumble_requested[controller_num] = rumble;
    }
}

ultramodern::input::connected_device_info_t hwtr::input::get_connected_device_info(int controller_num) {
    if (controller_num == 0) {
        // Player 1 is always present (keyboard fallback).
        return { ultramodern::input::Device::Controller, connected_pak() };
    }
    if (controller_num > 0 && controller_num < max_players && controllers[controller_num] != nullptr) {
        return { ultramodern::input::Device::Controller, connected_pak() };
    }
    return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
}
