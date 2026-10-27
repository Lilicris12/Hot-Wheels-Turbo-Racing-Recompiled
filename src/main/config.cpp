#include <fstream>

#include "hwtr/hwtr.hpp"
#include "json/json.hpp"

namespace {
    constexpr const char* config_filename = "config.json";

    hwtr::Config current_config{};

    ultramodern::renderer::GraphicsConfig default_graphics_config() {
        ultramodern::renderer::GraphicsConfig ret{};
        ret.developer_mode = false;
        ret.res_option = ultramodern::renderer::Resolution::Auto;
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
        ret.wm_option = ultramodern::renderer::WindowMode::Fullscreen;
#else
        ret.wm_option = ultramodern::renderer::WindowMode::Windowed;
#endif
        ret.hr_option = ultramodern::renderer::HUDRatioMode::Clamp16x9;
        ret.api_option = ultramodern::renderer::GraphicsApi::Auto;
        ret.ar_option = ultramodern::renderer::AspectRatio::Expand;
        ret.msaa_option = ultramodern::renderer::Antialiasing::MSAA2X;
        ret.rr_option = ultramodern::renderer::RefreshRate::Display;
        ret.hpfb_option = ultramodern::renderer::HighPrecisionFramebuffer::Auto;
        ret.rr_manual_value = 60;
        ret.ds_option = 1;
        return ret;
    }

    bool running_on_steam_deck() {
        // SteamOS sets SteamDeck=1 for games launched from Steam (including non-Steam shortcuts).
        const char* deck = std::getenv("SteamDeck");
        return deck != nullptr && deck[0] == '1';
    }

    template <typename T>
    void read_value(const nlohmann::json& j, const char* key, T& out) {
        auto it = j.find(key);
        if (it != j.end()) {
            try {
                out = it->get<T>();
            }
            catch (const nlohmann::json::exception&) {
                // Keep the default for malformed entries.
            }
        }
    }
}

hwtr::Config& hwtr::config() {
    return current_config;
}

void hwtr::load_config() {
    current_config = Config{};
    current_config.graphics = default_graphics_config();

    // Steam Deck: start fullscreen at the panel's native refresh with a lighter MSAA level.
    if (running_on_steam_deck()) {
        current_config.graphics.wm_option = ultramodern::renderer::WindowMode::Fullscreen;
        current_config.graphics.msaa_option = ultramodern::renderer::Antialiasing::None;
    }

    std::ifstream config_file{ get_app_folder_path() / config_filename };
    if (config_file.good()) {
        nlohmann::json j = nlohmann::json::parse(config_file, nullptr, false);
        if (!j.is_discarded()) {
            auto& g = current_config.graphics;
            if (auto it = j.find("graphics"); it != j.end() && it->is_object()) {
                read_value(*it, "resolution", g.res_option);
                read_value(*it, "window_mode", g.wm_option);
                read_value(*it, "hud_ratio", g.hr_option);
                read_value(*it, "api", g.api_option);
                read_value(*it, "aspect_ratio", g.ar_option);
                read_value(*it, "msaa", g.msaa_option);
                read_value(*it, "refresh_rate", g.rr_option);
                read_value(*it, "refresh_rate_manual", g.rr_manual_value);
                read_value(*it, "high_precision_framebuffer", g.hpfb_option);
                read_value(*it, "downsample", g.ds_option);
                read_value(*it, "developer_mode", g.developer_mode);
            }
            read_value(j, "master_volume", current_config.master_volume);
            read_value(j, "rumble", current_config.rumble);
            read_value(j, "analog_deadzone", current_config.analog_deadzone);
            read_value(j, "triggers_as_gas_brake", current_config.triggers_as_gas_brake);
        }
    }

    ultramodern::renderer::set_graphics_config(current_config.graphics);
    save_config();
}

void hwtr::save_config() {
    const auto& g = current_config.graphics;
    nlohmann::json j = {
        {"graphics", {
            {"resolution", g.res_option},
            {"window_mode", g.wm_option},
            {"hud_ratio", g.hr_option},
            {"api", g.api_option},
            {"aspect_ratio", g.ar_option},
            {"msaa", g.msaa_option},
            {"refresh_rate", g.rr_option},
            {"refresh_rate_manual", g.rr_manual_value},
            {"high_precision_framebuffer", g.hpfb_option},
            {"downsample", g.ds_option},
            {"developer_mode", g.developer_mode},
        }},
        {"master_volume", current_config.master_volume},
        {"rumble", current_config.rumble},
        {"analog_deadzone", current_config.analog_deadzone},
        {"triggers_as_gas_brake", current_config.triggers_as_gas_brake},
    };

    std::ofstream config_file{ get_app_folder_path() / config_filename };
    config_file << j.dump(4) << std::endl;
}
