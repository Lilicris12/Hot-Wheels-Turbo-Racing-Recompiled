# Building

## Requirements (all platforms)

- Git (with submodules), CMake 3.21+, Ninja, Python 3.11+ (only for the analysis tools)
- Clang 15+ (on Windows: `clang-cl` from Visual Studio's "C++ Clang tools" component)
- Your Hot Wheels Turbo Racing (USA) ROM, copied to `hwtr.us.z64` in the repository root
  (or pass `-DHWTR_ROM=/path/to/rom.z64`). Check it with `python3 tools/rom_info.py`.

```sh
git clone --recursive https://github.com/lilicris12/Hot-Wheels-Turbo-Racing-Recompiled
cd Hot-Wheels-Turbo-Racing-Recompiled
git submodule update --init --recursive   # if you cloned without --recursive
```

## What the first configure does

`cmake/GenerateRecompiledCode.cmake` runs during configure:

1. Verifies the ROM's SHA-1.
2. Builds `N64Recomp` and `RSPRecomp` from `lib/N64ModernRuntime/N64Recomp` for the host.
3. Runs `N64Recomp hwtr.us.toml`, which writes the game code as C into `RecompiledFuncs/`.
4. Runs `RSPRecomp aspMain.us.toml`, which writes the audio microcode as C++ into `rsp/aspMain.cpp`.

Both output folders are gitignored, since they are derived from the ROM. Generation reruns
automatically when `hwtr.us.toml`, `aspMain.us.toml` or `symbols/hwtr.syms.toml` change, or
when you pass `-DHWTR_REGENERATE=ON`. To use recompilers you built yourself, pass
`-DN64RECOMP_EXECUTABLE=... -DRSPRECOMP_EXECUTABLE=...`.

## Linux and Steam Deck

```sh
# Debian/Ubuntu
sudo apt install clang lld ninja-build cmake libsdl2-dev libgtk-3-dev libdbus-1-dev
# Arch / SteamOS (desktop mode, after `sudo steamos-readonly disable`)
sudo pacman -S clang lld ninja cmake sdl2 gtk3

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
./build/HotWheelsTurboRecompiled
```

It's easier to build on a desktop PC and copy the AppImage to the Deck. The CI workflow
builds one, and so do these commands:

```sh
DESTDIR=AppDir cmake --install build --prefix /usr
linuxdeploy-x86_64.AppImage --appdir AppDir --output appimage
```

See [docs/PLATFORMS.md](docs/PLATFORMS.md#steam-deck) for adding it to Steam.

## Windows

From a "x64 Native Tools Command Prompt for VS 2022":

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build build
build\HotWheelsTurboRecompiled.exe
```

SDL2 is downloaded automatically. `SDL2.dll`, `dxil.dll` and `dxcompiler.dll` are copied next
to the executable. Pass `--show-console` to see log output in release builds.

## macOS

```sh
xcode-select --install
brew install cmake ninja sdl2
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
open build/HotWheelsTurboRecompiled.app
```

## Android and iOS

Both app shells exist (`platform/android`, `platform/ios`), but RT64 can't open a window on
these platforms yet, so the build stops inside RT64. See
[docs/PLATFORMS.md](docs/PLATFORMS.md#android) for the remaining work and the commands to use
once that's done.

## Regenerating symbols (maintainers)

`symbols/hwtr.syms.toml` is generated from the ROM and is checked in. To rebuild it after
changing names or the analysis:

```sh
# libultra/libaudio/libmus identification (signatures from n64sym's database)
curl -LO https://raw.githubusercontent.com/shygoo/n64sym/master/src/builtin_signatures.sig
python3 tools/sigmatch.py hwtr.us.z64 builtin_signatures.sig -o symbols/libultra_sigs.txt

# function boundaries + names
python3 tools/find_functions.py hwtr.us.z64 --sigs symbols/libultra_sigs.txt \
    --names symbols/names.toml -o symbols/hwtr.syms.toml

python3 tools/tests/test_tools.py
```

Manual names and overrides go in `symbols/names.toml`.
