# Hot Wheels Turbo Racing: Recompiled

A static recompilation of **Hot Wheels Turbo Racing** (Nintendo 64, USA, `NHWE`) into a native
program. The game's MIPS code is translated to C with
[N64Recomp](https://github.com/N64Recomp/N64Recomp), runs on
[N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime), and is drawn with the
[RT64](https://github.com/rt64/rt64) renderer. There is no emulation of the CPU at runtime.

**No game data is included.** You need your own ROM of the USA release:

| | |
|---|---|
| Internal name | `HOT WHEELS TURBO` |
| Game code | `NHWE` |
| SHA-1 (`.z64`) | `94913a07e49005ffd43dfdfa16ff5862bdb93748` |

`python3 tools/rom_info.py <your rom>` checks a dump and converts `.n64`/`.v64` byte orders.

## Status

This repository contains a complete recompilation setup for the game. **It has not yet been
built and played end to end.** The recompilers and the C++ runtime haven't been run against
the ROM yet, so expect fixes to be needed on the first build. See
[docs/ANALYSIS.md](docs/ANALYSIS.md) for what is known about the ROM and
[docs/PLATFORMS.md](docs/PLATFORMS.md) for per-platform status.

| Piece | State |
|---|---|
| ROM layout, entrypoint, BSS, microcode locations | Done ([docs/ANALYSIS.md](docs/ANALYSIS.md)) |
| Function boundaries (1918 functions, no overlays) | Done: `symbols/hwtr.syms.toml` |
| libultra / libaudio / libmus identification (300 named) | Done: signature matching + manual names |
| N64Recomp config, idle-loop patch | Done: `hwtr.us.toml` |
| Audio microcode (aspMain) RSP recompilation config | Done: `aspMain.us.toml` |
| Runtime glue (window, RT64, audio, input, config, ROM import) | Written, compile-checked on Linux |
| Controller Pak saves (`.mpk`, emulator compatible) | Written: `src/game/controller_pak.cpp` |
| Windows / Linux / macOS / Steam Deck builds | CMake + CI ready; needs a first build |
| Android / iOS | App shells written; **blocked on RT64 mobile support** |

## Platforms

| Platform | Graphics API | Notes |
|---|---|---|
| Windows 10/11 x64 | D3D12 or Vulkan | |
| Linux x64 / arm64 | Vulkan | AppImage built by CI |
| Steam Deck | Vulkan | Linux build; starts fullscreen when run from Steam |
| macOS 11+ (Apple silicon, Intel) | Metal | `.app` bundle |
| Android 8+ (arm64) | Vulkan | Needs RT64 Android window support first |
| iOS / iPadOS 15+ | Metal | Needs RT64 UIKit window support first |

## Building

See [BUILDING.md](BUILDING.md). In short:

```sh
git clone --recursive https://github.com/lilicris12/Hot-Wheels-Turbo-Racing-Recompiled
cd Hot-Wheels-Turbo-Racing-Recompiled
cp /path/to/your/rom.z64 hwtr.us.z64
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```

The first configure builds N64Recomp/RSPRecomp and recompiles the game from your ROM.

## Controls

| N64 | Gamepad (Xbox layout) | Keyboard |
|---|---|---|
| Stick | Left stick | Arrow keys |
| A | A, Right trigger (gas) | X |
| B | B / X, Left trigger (brake) | C |
| Z | LB | Z |
| R | RB | S |
| L | Back | A |
| Start | Start | Enter |
| C buttons | Right stick, Y = C-Up | I / J / K / L |
| D-pad | D-pad | |

Alt+Enter or F11 toggles fullscreen. Settings live in `config.json` in the app data folder
(`%LOCALAPPDATA%\HotWheelsTurboRecompiled`, `~/.local/share/HotWheelsTurboRecompiled`,
`~/Library/Application Support/HotWheelsTurboRecompiled`). Set `triggers_as_gas_brake` to
`false` to map the triggers to R/Z instead.

Saves are written to `saves/controller_pak_1.mpk` in the same folder. These are standard
`.mpk` Controller Pak images, so saves from emulators can be copied in.

## Repository layout

```
hwtr.us.toml            N64Recomp config (game code)
aspMain.us.toml         RSPRecomp config (audio microcode)
symbols/                Function symbols, manual names and libultra signature matches
tools/                  ROM analysis tools that produced symbols/ (Python, no dependencies)
src/main/               Runtime glue: entry point, RT64 renderer, audio, input, config
src/game/               Game-specific native code (Controller Pak, missing libultra calls)
platform/               Windows/macOS/Linux packaging, Android and iOS app shells
lib/                    N64ModernRuntime and RT64 submodules
```

## Credits

Built on N64Recomp, N64ModernRuntime and RT64 by their contributors, and uses the libultra
signature database from [n64sym](https://github.com/shygoo/n64sym).
Hot Wheels is a trademark of Mattel. This project is not affiliated with Mattel, Nintendo or
the original developers.
