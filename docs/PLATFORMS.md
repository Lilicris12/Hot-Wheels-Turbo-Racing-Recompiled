# Platform notes

The game logic is plain C generated from the ROM, so it compiles anywhere Clang does. What
differs per platform is the window, the graphics API RT64 uses, and packaging.

## Windows

- RT64 uses D3D12 by default; set `"api": "Vulkan"` in `config.json` to switch.
- Requires the x64 MSVC runtime. CI produces a zip with the executable, `SDL2.dll` and the
  DXC DLLs.

## Linux

- Vulkan via SDL (`RT64_SDL_WINDOW_VULKAN`). Needs a Vulkan 1.2 driver (Mesa RADV/ANV, or NVIDIA).
- The AppImage statically links libstdc++, so it runs on older distros and SteamOS.
- The desktop entry and AppStream metadata are in `platform/linux/`.

## Steam Deck

The Steam Deck runs the Linux build. Gaming Mode works when the game is added to Steam:

1. In Desktop Mode, copy `HotWheelsTurboRecompiled-x86_64.AppImage` to the Deck, for
   example to `~/Applications/`, and mark it executable (Properties → Permissions).
2. Put your ROM next to it as `hwtr.us.z64`, or pick it from the file dialog on first launch.
   It's copied to `~/.local/share/HotWheelsTurboRecompiled/`, so this only happens once.
3. In Steam, choose *Add a Non-Steam Game* and select the AppImage.
4. In the shortcut's controller settings, pick the **Gamepad** template. The built-in mapping
   is the Xbox layout from the README: the right trigger accelerates and the left trigger
   brakes.

When launched from Steam (`SteamDeck=1` in the environment), the first run defaults to
fullscreen with MSAA off, which suits the Deck's GPU. The 16:10 panel is filled by the
widescreen (`Expand`) aspect setting, and the HUD is clamped to 16:9. Controller Pak saves are in `~/.local/share/HotWheelsTurboRecompiled/saves/`, which
Steam Cloud doesn't sync for non-Steam games.

## macOS

- Metal, through RT64's plume Metal backend. Requires macOS 11+, on Apple silicon or Intel.
- Built as `HotWheelsTurboRecompiled.app`. Unsigned builds need right-click → Open the first
  time.

## Android

**Status: app shell written, blocked on RT64.** RT64's window layer
(`src/hle/rt64_application_window.cpp`) has `static_assert(false && "Android unimplemented")`.
Everything else is ready:

- `platform/android` is a Gradle project. `LauncherActivity` imports the ROM with the
  system file picker into the app's files directory, then `GameActivity` (SDL's activity)
  loads `libmain.so`, which is the root CMake project built as a shared library.
- `src/main/rt64_render_context.cpp` passes RT64 the `ANativeWindow` behind the SDL window,
  which is what plume's Android `RenderWindow` expects.
- The data folder is the app's internal storage (`SDL_GetPrefPath`), so saves and
  `config.json` live there.

Remaining work in RT64 (upstream):

1. Implement the `__ANDROID__` branches of `ApplicationWindow::setup` and the window-size
   queries, using `ANativeWindow_getWidth`/`getHeight`. The plume Vulkan backend already
   supports `ANativeWindow`.
2. Make shader compilation not depend on the desktop DXC binaries when cross-compiling.
   Use the host's DXC to build the SPIR-V blobs, as the Linux build does.
3. Add touch controls. SDL delivers touches; an on-screen overlay that feeds
   `hwtr::input` is needed for phones without a controller.

Once that's done, build with:

With the ROM at the repository root as `hwtr.us.z64` (code generation runs on the host):

```sh
cd platform/android
gradle assembleRelease   # needs the Android SDK, NDK 27 and CMake 3.22+
```

## iOS / iPadOS

**Status: plist and CMake target written, blocked on RT64.** RT64's Apple code
(`src/apple/rt64_apple.mm`) targets AppKit (`NSWindow`). iOS needs a UIKit variant. The
Metal backend itself is shared.

- `src/main/main.cpp` creates an SDL Metal view and passes the `UIWindow` and `CAMetalLayer`
  in the iOS path.
- `platform/ios/Info.plist.in` enables Files-app sharing. Copy the ROM into the app's
  Documents folder (*On My iPhone → HW Turbo Racing*) as `import.z64`.
  `src/main/launcher.cpp` validates it and imports it on the next launch.
- No JIT is needed: the game is ahead-of-time compiled, which fits App Store rules. The
  runtime's mod support uses LiveRecomp, which should be disabled for iOS builds.

Once RT64 supports UIKit:

```sh
cmake -S . -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
cmake --build build-ios --config Release
```
