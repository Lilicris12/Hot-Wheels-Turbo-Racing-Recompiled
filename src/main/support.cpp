#include <cstdlib>

#include "hwtr/hwtr.hpp"

#include "SDL.h"

#if defined(_WIN32)
#   define WIN32_LEAN_AND_MEAN
#   include <Windows.h>
#   include <Shlobj.h>
#endif

std::filesystem::path hwtr::get_app_folder_path() {
    std::filesystem::path ret;

#if defined(_WIN32)
    PWSTR known_path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &known_path))) {
        ret = std::filesystem::path{ known_path } / program_id;
    }
    CoTaskMemFree(known_path);
#elif defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    // Sandboxed platforms: SDL picks the app's private writable directory.
    if (char* pref = SDL_GetPrefPath("", program_id)) {
        ret = std::filesystem::path{ pref };
        SDL_free(pref);
    }
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) {
        ret = std::filesystem::path{ home } / "Library" / "Application Support" / program_id;
    }
#else
    // Linux / Steam Deck: follow the XDG base directory spec.
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && xdg[0] != '\0') {
        ret = std::filesystem::path{ xdg } / program_id;
    }
    else if (const char* home = std::getenv("HOME")) {
        ret = std::filesystem::path{ home } / ".local" / "share" / program_id;
    }
#endif

    if (ret.empty()) {
        ret = std::filesystem::current_path() / "userdata";
    }

    std::error_code ec;
    std::filesystem::create_directories(ret, ec);
    return ret;
}

std::filesystem::path hwtr::get_program_path() {
    if (char* base = SDL_GetBasePath()) {
        std::filesystem::path ret{ base };
        SDL_free(base);
        return ret;
    }
    return std::filesystem::current_path();
}

void hwtr::message_box(const char* msg) {
    fprintf(stderr, "%s\n", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, program_name, msg, nullptr);
}
