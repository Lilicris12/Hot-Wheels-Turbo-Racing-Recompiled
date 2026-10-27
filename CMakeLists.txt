cmake_minimum_required(VERSION 3.21)

if (APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

project(HotWheelsTurboRecompiled VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (APPLE)
    enable_language(OBJC OBJCXX)
endif()

if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING "Clang (or clang-cl on Windows) is the supported compiler; "
                    "the recompiled code relies on Clang behaviour for floating point and aliasing.")
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-everything")
endif()

if (WIN32)
    # Avoid the constexpr mutex constructor, which breaks on older VC runtimes.
    add_compile_definitions(_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR NOMINMAX)
endif()

if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
    cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

set(HWTR_ROM "${CMAKE_SOURCE_DIR}/hwtr.us.z64" CACHE FILEPATH "Hot Wheels Turbo Racing (USA) ROM")
option(HWTR_REGENERATE "Re-run N64Recomp/RSPRecomp even if output exists" OFF)

set(HWTR_IS_MOBILE OFF)
if (ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(HWTR_IS_MOBILE ON)
endif()

# --- Generated code -------------------------------------------------------------------------

include(cmake/GenerateRecompiledCode.cmake)

# --- SDL2 -------------------------------------------------------------------------------------

set(HWTR_SDL2_VERSION "2.30.3")
if (WIN32)
    include(FetchContent)
    FetchContent_Declare(sdl2
        URL https://github.com/libsdl-org/SDL/releases/download/release-${HWTR_SDL2_VERSION}/SDL2-devel-${HWTR_SDL2_VERSION}-VC.zip)
    FetchContent_MakeAvailable(sdl2)
    add_library(SDL2::SDL2 SHARED IMPORTED)
    set_target_properties(SDL2::SDL2 PROPERTIES
        IMPORTED_LOCATION "${sdl2_SOURCE_DIR}/lib/x64/SDL2.dll"
        IMPORTED_IMPLIB "${sdl2_SOURCE_DIR}/lib/x64/SDL2.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${sdl2_SOURCE_DIR}/include")
    set(SDL2_INCLUDE_DIRS "${sdl2_SOURCE_DIR}/include")
elseif (HWTR_IS_MOBILE)
    # Mobile platforms build SDL from source (Android also needs its Java glue, see platform/android).
    include(FetchContent)
    FetchContent_Declare(sdl2
        URL https://github.com/libsdl-org/SDL/releases/download/release-${HWTR_SDL2_VERSION}/SDL2-${HWTR_SDL2_VERSION}.tar.gz)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(sdl2)
    set(SDL2_INCLUDE_DIRS "${sdl2_SOURCE_DIR}/include")
    if (NOT TARGET SDL2::SDL2)
        add_library(SDL2::SDL2 ALIAS SDL2-static)
    endif()
else()
    find_package(SDL2 REQUIRED)
endif()

# --- RT64 and the runtime -----------------------------------------------------------------------

set(RT64_STATIC TRUE)
if (CMAKE_SYSTEM_NAME MATCHES "Linux" OR ANDROID)
    set(RT64_SDL_WINDOW_VULKAN TRUE)
    add_compile_definitions(RT64_SDL_WINDOW_VULKAN)
endif()
add_compile_definitions(HLSL_CPU)

add_subdirectory(${CMAKE_SOURCE_DIR}/lib/rt64 ${CMAKE_BINARY_DIR}/rt64)
target_include_directories(rt64 PRIVATE ${CMAKE_BINARY_DIR}/rt64/src)

add_subdirectory(${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime)

set(HWTR_RUNTIME_INCLUDES
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/ultramodern/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/librecomp/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/thirdparty
)

# --- Recompiled game code -------------------------------------------------------------------

add_library(RecompiledFuncs STATIC)
file(GLOB HWTR_FUNC_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/RecompiledFuncs/*.c
    ${CMAKE_SOURCE_DIR}/RecompiledFuncs/*.cpp)
target_sources(RecompiledFuncs PRIVATE ${HWTR_FUNC_SOURCES})
target_include_directories(RecompiledFuncs PRIVATE ${HWTR_RUNTIME_INCLUDES})
if (MSVC AND NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(RecompiledFuncs PRIVATE /bigobj)
else()
    target_compile_options(RecompiledFuncs PRIVATE
        -fno-strict-aliasing
        -Wno-unused-variable
        -Wno-unused-but-set-variable
        -Wno-implicit-function-declaration
    )
endif()

# --- Executable -------------------------------------------------------------------------------

set(HWTR_SOURCES
    src/main/main.cpp
    src/main/support.cpp
    src/main/config.cpp
    src/main/launcher.cpp
    src/main/audio.cpp
    src/main/input.cpp
    src/main/register_overlays.cpp
    src/main/rt64_render_context.cpp
    src/game/controller_pak.cpp
    src/game/ultra_extras.cpp
    rsp/aspMain.cpp
)

if (ANDROID)
    # SDL's Java activity loads libmain.so and calls SDL_main.
    add_library(HotWheelsTurboRecompiled SHARED ${HWTR_SOURCES})
    set_target_properties(HotWheelsTurboRecompiled PROPERTIES OUTPUT_NAME main)
else()
    add_executable(HotWheelsTurboRecompiled ${HWTR_SOURCES})
endif()

target_compile_definitions(HotWheelsTurboRecompiled PRIVATE HWTR_VERSION="${PROJECT_VERSION}")

target_include_directories(HotWheelsTurboRecompiled PRIVATE
    ${HWTR_RUNTIME_INCLUDES}
    ${SDL2_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/lib/rt64/src
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/plume
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/hlslpp/include
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/inc
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/nativefiledialog-extended/src/include
    ${CMAKE_BINARY_DIR}/rt64/src
)

if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    target_compile_options(HotWheelsTurboRecompiled PRIVATE -march=nehalem -fno-strict-aliasing)
else()
    target_compile_options(HotWheelsTurboRecompiled PRIVATE -fno-strict-aliasing)
endif()

target_link_libraries(HotWheelsTurboRecompiled PRIVATE
    RecompiledFuncs
    librecomp
    ultramodern
    rt64
    SDL2::SDL2
)
if (NOT HWTR_IS_MOBILE)
    target_link_libraries(HotWheelsTurboRecompiled PRIVATE nfd)
endif()

# --- Platform specifics -----------------------------------------------------------------------

if (WIN32)
    target_sources(HotWheelsTurboRecompiled PRIVATE platform/windows/app.rc)
    target_link_libraries(HotWheelsTurboRecompiled PRIVATE Winmm.lib)
    set_target_properties(HotWheelsTurboRecompiled PROPERTIES
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_RELEASE "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
        LINK_FLAGS_RELWITHDEBINFO "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
        LINK_FLAGS_MINSIZEREL "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
    add_custom_command(TARGET HotWheelsTurboRecompiled POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${sdl2_SOURCE_DIR}/lib/x64/SDL2.dll"
            "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxil.dll"
            "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxcompiler.dll"
            $<TARGET_FILE_DIR:HotWheelsTurboRecompiled>)
endif()

if (CMAKE_SYSTEM_NAME MATCHES "Linux")
    set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
    set(THREADS_PREFER_PTHREAD_FLAG TRUE)
    find_package(Threads REQUIRED)
    target_link_libraries(HotWheelsTurboRecompiled PRIVATE "-latomic" ${CMAKE_DL_LIBS} Threads::Threads)
    # Keep the AppImage portable across distros (Steam Deck runs SteamOS/Arch).
    target_link_options(HotWheelsTurboRecompiled PRIVATE -static-libstdc++)
endif()

if (APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set_target_properties(HotWheelsTurboRecompiled PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/platform/macos/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "Hot Wheels Turbo Racing Recompiled"
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.hwtrrecomp"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
    find_package(Threads REQUIRED)
    target_link_libraries(HotWheelsTurboRecompiled PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set_target_properties(HotWheelsTurboRecompiled PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/platform/ios/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.hwtrrecomp"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
    target_link_libraries(HotWheelsTurboRecompiled PRIVATE
        "-framework UIKit -framework Metal -framework QuartzCore -framework GameController -framework AVFoundation -framework CoreHaptics")
endif()

if (ANDROID)
    target_link_libraries(HotWheelsTurboRecompiled PRIVATE android log)
endif()

# --- Install / packaging -----------------------------------------------------------------------

include(GNUInstallDirs)
if (CMAKE_SYSTEM_NAME MATCHES "Linux")
    install(TARGETS HotWheelsTurboRecompiled RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES platform/linux/io.github.hwtrrecomp.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES platform/linux/io.github.hwtrrecomp.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
elseif (WIN32)
    install(TARGETS HotWheelsTurboRecompiled RUNTIME DESTINATION .)
    install(FILES
        "${sdl2_SOURCE_DIR}/lib/x64/SDL2.dll"
        "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxil.dll"
        "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxcompiler.dll"
        DESTINATION .)
elseif (APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    install(TARGETS HotWheelsTurboRecompiled BUNDLE DESTINATION .)
endif()
if (CMAKE_SYSTEM_NAME MATCHES "Linux")
    install(FILES platform/icons/io.github.hwtrrecomp.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)
endif()
