#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hwtr/hwtr.hpp"
#include "librecomp/game.hpp"

#include "SDL.h"

#if !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
#   define HWTR_HAS_FILE_DIALOG 1
#   include "nfd.h"
#endif

namespace {
    const char* describe(recomp::RomValidationError err) {
        switch (err) {
            case recomp::RomValidationError::Good:
                return "OK";
            case recomp::RomValidationError::FailedToOpen:
                return "The file could not be opened.";
            case recomp::RomValidationError::NotARom:
                return "The file is not an N64 ROM.";
            case recomp::RomValidationError::IncorrectRom:
                return "This is not Hot Wheels Turbo Racing.";
            case recomp::RomValidationError::IncorrectVersion:
                return "This is Hot Wheels Turbo Racing, but not the supported USA release (NHWE).";
            case recomp::RomValidationError::NotYet:
                return "This game is not supported yet.";
            case recomp::RomValidationError::OtherError:
            default:
                return "Unknown error while reading the ROM.";
        }
    }

    bool try_rom(const std::filesystem::path& path) {
        recomp::RomValidationError err = recomp::select_rom(path, hwtr::game_id);
        if (err == recomp::RomValidationError::Good) {
            return true;
        }
        std::string msg = "Could not use " + path.string() + ":\n" + describe(err);
        hwtr::message_box(msg.c_str());
        return false;
    }

    // Looks for a ROM passed on the command line (--rom <path>, or a bare path, which is
    // what "Open with" and drag-and-drop onto the executable produce).
    std::filesystem::path rom_from_args(int argc, char** argv) {
        for (int i = 1; i < argc; i++) {
            if (std::strcmp(argv[i], "--rom") == 0 && i + 1 < argc) {
                return std::filesystem::u8path(argv[i + 1]);
            }
            if (argv[i][0] != '-') {
                std::filesystem::path p = std::filesystem::u8path(argv[i]);
                std::error_code ec;
                if (std::filesystem::is_regular_file(p, ec)) {
                    return p;
                }
            }
        }
        return {};
    }

    // ROM sitting next to the executable (handy for portable installs and the Steam Deck).
    std::filesystem::path rom_next_to_program() {
        for (const char* name : { "hwtr.us.z64", "baserom.z64" }) {
            std::filesystem::path p = hwtr::get_program_path() / name;
            std::error_code ec;
            if (std::filesystem::is_regular_file(p, ec)) {
                return p;
            }
        }
        return {};
    }

#if defined(HWTR_HAS_FILE_DIALOG)
    std::filesystem::path rom_from_dialog() {
        std::filesystem::path ret;
        if (NFD_Init() != NFD_OKAY) {
            return ret;
        }
        nfdu8filteritem_t filters[] = { { "N64 ROM", "z64,n64,v64" } };
        nfdu8char_t* out_path = nullptr;
        if (NFD_OpenDialogU8(&out_path, filters, 1, nullptr) == NFD_OKAY) {
            ret = std::filesystem::u8path(out_path);
            NFD_FreePathU8(out_path);
        }
        NFD_Quit();
        return ret;
    }
#endif
}

bool hwtr::ensure_rom_selected(int argc, char** argv) {
    recomp::check_all_stored_roms();

    // An explicitly provided ROM always wins so users can replace a stored one.
    if (std::filesystem::path p = rom_from_args(argc, argv); !p.empty()) {
        return try_rom(p);
    }

    if (recomp::is_rom_valid(game_id)) {
        return true;
    }

    if (std::filesystem::path p = rom_next_to_program(); !p.empty() && try_rom(p)) {
        return true;
    }

#if defined(HWTR_HAS_FILE_DIALOG)
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, program_name,
        "Please select your Hot Wheels Turbo Racing (USA) ROM.\n"
        "It will be copied to the app's data folder, so this is only needed once.", nullptr);
    while (true) {
        std::filesystem::path p = rom_from_dialog();
        if (p.empty()) {
            return false;
        }
        if (try_rom(p)) {
            return true;
        }
    }
#else
    // Mobile: Android's LauncherActivity copies the picked ROM into the app data folder;
    // on iOS the user drops it into the app's Documents folder through the Files app.
    std::vector<std::filesystem::path> candidates{ get_app_folder_path() / "import.z64" };
#if defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) {
        candidates.push_back(std::filesystem::path{ home } / "Documents" / "import.z64");
    }
#endif
    for (const auto& p : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(p, ec) && try_rom(p)) {
            std::filesystem::remove(p, ec);
            return true;
        }
    }
    message_box("No ROM found. Import your Hot Wheels Turbo Racing (USA) ROM from the app first.");
    return false;
#endif
}
