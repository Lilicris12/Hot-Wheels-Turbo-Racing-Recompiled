// Renderer backend: hands the game's F3DEX/S2DEX display lists to RT64, which renders them
// with D3D12 (Windows), Vulkan (Linux, Steam Deck, Windows) or Metal (macOS).

#include <algorithm>
#include <cassert>
#include <memory>

#define HLSL_CPU
#include "hle/rt64_application.h"
#include "hle/rt64_present_queue.h"
#include "hle/rt64_state.h"

#include "hwtr/hwtr.hpp"
#include "ultramodern/ultramodern.hpp"
#if defined(__ANDROID__)
#   include "SDL_syswm.h"
#endif
#include "ultramodern/config.hpp"

namespace {
    uint8_t DMEM[0x1000];
    uint8_t IMEM[0x1000];

    unsigned int MI_INTR_REG = 0;
    unsigned int DPC_START_REG = 0;
    unsigned int DPC_END_REG = 0;
    unsigned int DPC_CURRENT_REG = 0;
    unsigned int DPC_STATUS_REG = 0;
    unsigned int DPC_CLOCK_REG = 0;
    unsigned int DPC_BUFBUSY_REG = 0;
    unsigned int DPC_PIPEBUSY_REG = 0;
    unsigned int DPC_TMEM_REG = 0;

    void dummy_check_interrupts() {}

    RT64::UserConfiguration::Antialiasing compute_max_supported_aa(plume::RenderSampleCounts bits) {
        if (bits & plume::RenderSampleCount::Bits::COUNT_2) {
            if (bits & plume::RenderSampleCount::Bits::COUNT_4) {
                if (bits & plume::RenderSampleCount::Bits::COUNT_8) {
                    return RT64::UserConfiguration::Antialiasing::MSAA8X;
                }
                return RT64::UserConfiguration::Antialiasing::MSAA4X;
            }
            return RT64::UserConfiguration::Antialiasing::MSAA2X;
        }
        return RT64::UserConfiguration::Antialiasing::None;
    }

    RT64::UserConfiguration::AspectRatio to_rt64(ultramodern::renderer::AspectRatio option) {
        switch (option) {
            case ultramodern::renderer::AspectRatio::Original: return RT64::UserConfiguration::AspectRatio::Original;
            case ultramodern::renderer::AspectRatio::Manual: return RT64::UserConfiguration::AspectRatio::Manual;
            case ultramodern::renderer::AspectRatio::Expand:
            default: return RT64::UserConfiguration::AspectRatio::Expand;
        }
    }

    RT64::UserConfiguration::Antialiasing to_rt64(ultramodern::renderer::Antialiasing option) {
        switch (option) {
            case ultramodern::renderer::Antialiasing::MSAA2X: return RT64::UserConfiguration::Antialiasing::MSAA2X;
            case ultramodern::renderer::Antialiasing::MSAA4X: return RT64::UserConfiguration::Antialiasing::MSAA4X;
            case ultramodern::renderer::Antialiasing::MSAA8X: return RT64::UserConfiguration::Antialiasing::MSAA8X;
            case ultramodern::renderer::Antialiasing::None:
            default: return RT64::UserConfiguration::Antialiasing::None;
        }
    }

    RT64::UserConfiguration::RefreshRate to_rt64(ultramodern::renderer::RefreshRate option) {
        switch (option) {
            case ultramodern::renderer::RefreshRate::Original: return RT64::UserConfiguration::RefreshRate::Original;
            case ultramodern::renderer::RefreshRate::Manual: return RT64::UserConfiguration::RefreshRate::Manual;
            case ultramodern::renderer::RefreshRate::Display:
            default: return RT64::UserConfiguration::RefreshRate::Display;
        }
    }

    RT64::UserConfiguration::InternalColorFormat to_rt64(ultramodern::renderer::HighPrecisionFramebuffer option) {
        switch (option) {
            case ultramodern::renderer::HighPrecisionFramebuffer::On: return RT64::UserConfiguration::InternalColorFormat::High;
            case ultramodern::renderer::HighPrecisionFramebuffer::Off: return RT64::UserConfiguration::InternalColorFormat::Standard;
            case ultramodern::renderer::HighPrecisionFramebuffer::Auto:
            default: return RT64::UserConfiguration::InternalColorFormat::Automatic;
        }
    }

    void set_application_user_config(RT64::Application* application, const ultramodern::renderer::GraphicsConfig& config) {
        switch (config.res_option) {
            default:
            case ultramodern::renderer::Resolution::Auto:
                application->userConfig.resolution = RT64::UserConfiguration::Resolution::WindowIntegerScale;
                application->userConfig.downsampleMultiplier = 1;
                break;
            case ultramodern::renderer::Resolution::Original:
                application->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
                application->userConfig.resolutionMultiplier = std::max(config.ds_option, 1);
                application->userConfig.downsampleMultiplier = std::max(config.ds_option, 1);
                break;
            case ultramodern::renderer::Resolution::Original2x:
                application->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
                application->userConfig.resolutionMultiplier = 2.0 * std::max(config.ds_option, 1);
                application->userConfig.downsampleMultiplier = std::max(config.ds_option, 1);
                break;
        }

        switch (config.hr_option) {
            default:
            case ultramodern::renderer::HUDRatioMode::Original:
                application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Original;
                break;
            case ultramodern::renderer::HUDRatioMode::Clamp16x9:
                application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Manual;
                application->userConfig.extAspectTarget = 16.0 / 9.0;
                break;
            case ultramodern::renderer::HUDRatioMode::Full:
                application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Expand;
                break;
        }

        application->userConfig.aspectRatio = to_rt64(config.ar_option);
        application->userConfig.antialiasing = to_rt64(config.msaa_option);
        application->userConfig.refreshRate = to_rt64(config.rr_option);
        application->userConfig.refreshRateTarget = config.rr_manual_value;
        application->userConfig.internalColorFormat = to_rt64(config.hpfb_option);
        application->userConfig.displayBuffering = RT64::UserConfiguration::DisplayBuffering::Triple;
    }

    ultramodern::renderer::SetupResult map_setup_result(RT64::Application::SetupResult result) {
        switch (result) {
            case RT64::Application::SetupResult::Success: return ultramodern::renderer::SetupResult::Success;
            case RT64::Application::SetupResult::DynamicLibrariesNotFound: return ultramodern::renderer::SetupResult::DynamicLibrariesNotFound;
            case RT64::Application::SetupResult::InvalidGraphicsAPI: return ultramodern::renderer::SetupResult::InvalidGraphicsAPI;
            case RT64::Application::SetupResult::GraphicsAPINotFound: return ultramodern::renderer::SetupResult::GraphicsAPINotFound;
            case RT64::Application::SetupResult::GraphicsDeviceNotFound:
            default: return ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
        }
    }

    ultramodern::renderer::GraphicsApi map_graphics_api(RT64::UserConfiguration::GraphicsAPI api) {
        switch (api) {
            case RT64::UserConfiguration::GraphicsAPI::D3D12: return ultramodern::renderer::GraphicsApi::D3D12;
            case RT64::UserConfiguration::GraphicsAPI::Vulkan: return ultramodern::renderer::GraphicsApi::Vulkan;
            case RT64::UserConfiguration::GraphicsAPI::Metal: return ultramodern::renderer::GraphicsApi::Metal;
            case RT64::UserConfiguration::GraphicsAPI::Automatic:
            default: return ultramodern::renderer::GraphicsApi::Auto;
        }
    }

    class RT64Context final : public ultramodern::renderer::RendererContext {
    public:
        RT64Context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
            static unsigned char dummy_rom_header[0x40];

            RT64::Application::Core core{};
#if defined(_WIN32)
            core.window = window_handle.window;
#elif defined(__ANDROID__)
            // plume renders to the ANativeWindow behind the SDL window.
            SDL_SysWMinfo wm_info;
            SDL_VERSION(&wm_info.version);
            SDL_GetWindowWMInfo(window_handle, &wm_info);
            core.window = wm_info.info.android.window;
#elif defined(__linux__)
            core.window = window_handle;
#elif defined(__APPLE__)
            core.window.window = window_handle.window;
            core.window.view = window_handle.view;
#endif
            core.checkInterrupts = dummy_check_interrupts;
            core.HEADER = dummy_rom_header;
            core.RDRAM = rdram;
            core.DMEM = DMEM;
            core.IMEM = IMEM;
            core.MI_INTR_REG = &MI_INTR_REG;
            core.DPC_START_REG = &DPC_START_REG;
            core.DPC_END_REG = &DPC_END_REG;
            core.DPC_CURRENT_REG = &DPC_CURRENT_REG;
            core.DPC_STATUS_REG = &DPC_STATUS_REG;
            core.DPC_CLOCK_REG = &DPC_CLOCK_REG;
            core.DPC_BUFBUSY_REG = &DPC_BUFBUSY_REG;
            core.DPC_PIPEBUSY_REG = &DPC_PIPEBUSY_REG;
            core.DPC_TMEM_REG = &DPC_TMEM_REG;

            ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
            core.VI_STATUS_REG = &vi->VI_STATUS_REG;
            core.VI_ORIGIN_REG = &vi->VI_ORIGIN_REG;
            core.VI_WIDTH_REG = &vi->VI_WIDTH_REG;
            core.VI_INTR_REG = &vi->VI_INTR_REG;
            core.VI_V_CURRENT_LINE_REG = &vi->VI_V_CURRENT_LINE_REG;
            core.VI_TIMING_REG = &vi->VI_TIMING_REG;
            core.VI_V_SYNC_REG = &vi->VI_V_SYNC_REG;
            core.VI_H_SYNC_REG = &vi->VI_H_SYNC_REG;
            core.VI_LEAP_REG = &vi->VI_LEAP_REG;
            core.VI_H_START_REG = &vi->VI_H_START_REG;
            core.VI_V_START_REG = &vi->VI_V_START_REG;
            core.VI_V_BURST_REG = &vi->VI_V_BURST_REG;
            core.VI_X_SCALE_REG = &vi->VI_X_SCALE_REG;
            core.VI_Y_SCALE_REG = &vi->VI_Y_SCALE_REG;

            RT64::ApplicationConfiguration app_config;
            app_config.useConfigurationFile = false;

            app = std::make_unique<RT64::Application>(core, app_config);

            const auto& cur_config = ultramodern::renderer::get_graphics_config();
            set_application_user_config(app.get(), cur_config);
            app->userConfig.developerMode = developer_mode;
            // Scale texture LODs with the output resolution.
            app->enhancementConfig.textureLOD.scale = true;

            switch (cur_config.api_option) {
                case ultramodern::renderer::GraphicsApi::D3D12:
                    app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::D3D12;
                    break;
                case ultramodern::renderer::GraphicsApi::Vulkan:
                    app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Vulkan;
                    break;
                case ultramodern::renderer::GraphicsApi::Metal:
                    app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Metal;
                    break;
                default:
                    app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Automatic;
                    break;
            }

            uint32_t thread_id = 0;
#ifdef _WIN32
            thread_id = window_handle.thread_id;
#endif
            setup_result = map_setup_result(app->setup(thread_id));
            chosen_api = map_graphics_api(app->chosenGraphicsAPI);
            if (setup_result != ultramodern::renderer::SetupResult::Success) {
                app = nullptr;
                return;
            }

            app->setFullScreen(cur_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);

            // Clamp MSAA to what the device supports for the formats RT64 renders with.
            if (app->device->getCapabilities().sampleLocations) {
                plume::RenderSampleCounts color = app->device->getSampleCountsSupported(plume::RenderFormat::R8G8B8A8_UNORM);
                plume::RenderSampleCounts depth = app->device->getSampleCountsSupported(plume::RenderFormat::D32_FLOAT);
                max_msaa = compute_max_supported_aa(color & depth);
            }
            if (app->userConfig.antialiasing > max_msaa) {
                app->userConfig.antialiasing = max_msaa;
                app->updateMultisampling();
            }
        }

        ~RT64Context() override = default;

        bool valid() override { return static_cast<bool>(app); }

        bool update_config(const ultramodern::renderer::GraphicsConfig& old_config, const ultramodern::renderer::GraphicsConfig& new_config) override {
            if (old_config == new_config) {
                return false;
            }
            if (new_config.wm_option != old_config.wm_option) {
                app->setFullScreen(new_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);
            }
            set_application_user_config(app.get(), new_config);
            if (app->userConfig.antialiasing > max_msaa) {
                app->userConfig.antialiasing = max_msaa;
            }
            app->updateUserConfig(true);
            if (new_config.msaa_option != old_config.msaa_option) {
                app->updateMultisampling();
            }
            return true;
        }

        void enable_instant_present() override {
            app->enhancementConfig.presentation.mode = RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
            app->updateEnhancementConfig();
        }

        void send_dl(const OSTask* task) override {
            app->state->rsp->reset();
            app->interpreter->loadUCodeGBI(task->t.ucode & 0x3FFFFFF, task->t.ucode_data & 0x3FFFFFF, true);
            app->processDisplayLists(app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF, 0, true);
        }

        void send_dummy_workload(uint32_t fb_address) override {
            // Only sent before the game starts (when a launcher UI would draw). The game is
            // started immediately after ROM selection, so there's nothing to draw.
            (void)fb_address;
        }

        void update_screen() override {
            app->updateScreen();
        }

        void shutdown() override {
            if (app != nullptr) {
                app->end();
            }
        }

        uint32_t get_display_framerate() const override {
            return app->presentQueue->ext.sharedResources->swapChainRate;
        }

        float get_resolution_scale() const override {
            constexpr int reference_height = 240;
            switch (app->userConfig.resolution) {
                case RT64::UserConfiguration::Resolution::WindowIntegerScale:
                    if (app->sharedQueueResources->swapChainHeight > 0) {
                        return std::max(float((app->sharedQueueResources->swapChainHeight + reference_height - 1) / reference_height), 1.0f);
                    }
                    return 1.0f;
                case RT64::UserConfiguration::Resolution::Manual:
                    return float(app->userConfig.resolutionMultiplier);
                case RT64::UserConfiguration::Resolution::Original:
                default:
                    return 1.0f;
            }
        }

    private:
        std::unique_ptr<RT64::Application> app;
        RT64::UserConfiguration::Antialiasing max_msaa = RT64::UserConfiguration::Antialiasing::None;
    };
}

std::unique_ptr<ultramodern::renderer::RendererContext> hwtr::renderer::create_render_context(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    return std::make_unique<RT64Context>(rdram, window_handle, developer_mode);
}
