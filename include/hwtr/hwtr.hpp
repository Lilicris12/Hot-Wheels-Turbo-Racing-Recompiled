#ifndef __HWTR_HPP__
#define __HWTR_HPP__

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

#if defined(__APPLE__)
#   include <TargetConditionals.h>
#endif

#include "ultramodern/ultramodern.hpp"
#include "ultramodern/config.hpp"
#include "ultramodern/input.hpp"
#include "ultramodern/renderer_context.hpp"

union SDL_Event;

namespace hwtr {
    inline constexpr const char* program_name = "Hot Wheels Turbo Racing: Recompiled";
    inline constexpr const char* program_id = "HotWheelsTurboRecompiled";
    inline constexpr const char8_t* game_id = u8"hwtr.n64.us";
    inline constexpr const char* mod_game_id = "hwtr";

    // Platform paths (support.cpp).
    std::filesystem::path get_app_folder_path();
    std::filesystem::path get_program_path();

    // Persistent settings (config.cpp).
    struct Config {
        ultramodern::renderer::GraphicsConfig graphics;
        int master_volume = 100;           // 0-100
        bool rumble = true;
        float analog_deadzone = 0.12f;     // 0-1
        bool triggers_as_gas_brake = true; // Right trigger = A (gas), left trigger = B (brake)
    };
    void load_config();
    void save_config();
    Config& config();

    // ROM selection (launcher.cpp). Returns false if no valid ROM could be found.
    bool ensure_rom_selected(int argc, char** argv);

    // Recompiled code registration (register_overlays.cpp).
    void register_overlays();

    namespace audio {
        void init();
        void queue_samples(int16_t* audio_data, size_t sample_count);
        size_t get_frames_remaining();
        void set_frequency(uint32_t freq);
    }

    namespace input {
        void init();
        void handle_event(const SDL_Event& event);
        void update();
        void poll_inputs();
        bool get_n64_input(int controller_num, uint16_t* buttons, float* x, float* y);
        void set_rumble(int controller_num, bool rumble);
        ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num);
    }

    namespace renderer {
        std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(
            uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);
    }

    void message_box(const char* msg);
}

#endif
