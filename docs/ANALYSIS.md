# ROM analysis: Hot Wheels Turbo Racing (USA)

What we know about the ROM, and the evidence behind the recompilation configs. All addresses
are virtual (KSEG0) unless they're marked as ROM offsets.

## Header

| Field | Value |
|---|---|
| Image | 12 MiB, big-endian `.z64` |
| Name / code | `HOT WHEELS TURBO` / `NHWE` |
| Entrypoint | `0x80060400` |
| SHA-1 | `94913a07e49005ffd43dfdfa16ff5862bdb93748` |
| XXH3-64 (runtime ROM check) | `0x681827CB928E9D96` |

## Memory layout

The entrypoint at `0x80060400` (ROM `0x1000`) does the usual libultra boot:

```
bzero(0x80101E70, 0x4CE60)     ; BSS
sp = 0x80143840
jr 0x800CD4C0                  ; boot: osInitialize, create idle thread
```

So the main segment is ROM `0x1000-0xA2A70`, loaded at `0x80060400-0x80101E70`, with BSS up
to `0x8014ECD0`. That fits inside the 1 MiB that IPL3 (and the runtime's `init()`) copies,
so **no code is loaded later**. A scan for `jr $ra`/stack-frame patterns over the rest of
the ROM finds only stray matches inside asset data, so **there are no code overlays** and no
compressed code. Assets use gzip/deflate (one `1f 8b 08` stream at ROM `0x98CD00`), which
the game decompresses itself.

| Range (vram) | Contents |
|---|---|
| `0x80060400-0x800E7AA0` | CPU code (`.text`) |
| `0x800E7AA0-0x800E7B70` | `rspboot` |
| `0x800E7B70-0x800E87D0` | audio microcode text (aspMain, `0xC60` bytes) |
| `0x800E87D0-0x800EF060` | graphics microcode text: F3DEX, F3DEX.Rej, F3DEX.NoN, F3DLX.Rej, S2DEX |
| `0x800EF060-0x80101E70` | data, rodata, microcode data |

The graphics microcode IDs, from the data strings, are `F3DEX fifo 2.08`, `F3DEX.Rej fifo 2.08`,
`F3DEX.NoN fifo 2.08`, `F3DLX.Rej fifo 2.08` and `S2DEX fifo 2.08`. These are all F3DEX 1.x
family microcodes, which RT64 runs as HLE.

## Threads and boot

- `boot_main` (`0x800CD4C0`): `osInitialize()`, then creates the idle thread (id 1, pri 10,
  entry `0x800CD524`).
- `idle_thread_entry` (`0x800CD524`): sets up the VI/scheduler (`0x800D6200`, then
  `0x800D5560(2, 1)`, which indexes `osViModeTable`, so `OS_VI_NTSC_LAN1`),
  `osViSetSpecialFeatures(0x5A)`, creates the main thread (id 3, entry `0x80078A34`), drops to
  priority 0, and then **spins forever** checking a hook pointer at `0x801455B4`.
  Nothing in the ROM ever writes that pointer, and ultramodern can't preempt a thread that
  never blocks. `hwtr.us.toml` therefore patches the loop head at `0x800CD5B8` into a
  jump-to-self, which N64Recomp emits as `pause_self()`.

The code uses the libultra scheduler (`osCreateScheduler`, `__scMain`, ...), NuSystem-style
controller and pak wrappers (`0x800D64A0`...), n_audio, plus Software Creations' **libmus**
(`MusStartSong`, `MusSetMasterVolume`, ...) for music.

## Function discovery

The ROM has no symbols. `tools/find_functions.py` sweeps `.text` linearly:

- It follows branches inside each function and keeps going until the last reachable
  terminator (`jr $ra`, `j`, `b`, `eret`).
- A branch to a `jal` target is a tail call, not part of the function. libgcc's `__divdi3`
  uses conditional tail calls into `__udivdi3`, for example.
- It recovers `jr $reg` switch tables by tracking constant registers. That covers the IDO
  pattern (`lui; addu; lw %lo`) and the GCC pattern (base kept in a saved register). All 34
  switch statements resolve. The other three `jr $reg` are the entrypoint's indirect jump to
  `boot_main` and two exception-handler internals.
- It splits at `jal` targets that follow a terminator, including functions that start with
  a `nop`.

Result: **1918 functions**, every `jal` target is a function start.

## Library identification

`tools/sigmatch.py` matches the n64sym signature database. Each signature is a CRC32 of the
function bytes with relocated fields masked. The tool then follows the relocations of the
matched functions to name callees and data (`__osViNext`, `__osRunQueue`, `osViModeTable`, ...).
Short functions (< 32 bytes) that match more than once are discarded as ambiguous.
`symbols/names.toml` adds the functions the database misses for this libultra build:

| vram | Name | Evidence |
|---|---|---|
| `0x800E2E60` | `osInitialize` | Sets SR/FPCSR, polls the PIF, falls into `createSpeedParam` |
| `0x800E3FD0` | `osInvalICache` | `cache 0x10` (Hit_Invalidate_I) loop |
| `0x800DFB70` | `osViGetNextFramebuffer` | Relocation from a matched caller |
| `0x800E36DC` / `0x800E3798` | `send_mesg` / `handle_CpU` | Position inside `exceptasm.s` |

A scan for RCP register access (`lui 0xA4xx`), KSEG1 (`lui 0xA0xx`), COP0 and `cache` finds
them **only** in functions that N64Recomp reimplements or ignores, so no game code touches
hardware directly.

### Runtime coverage

Every libultra function that game code calls has a runtime implementation, except:

- `osViGetCurrentMode` and `osViGetStatus`, which N64Recomp ignores but the runtime doesn't
  define. These are implemented in `src/game/ultra_extras.cpp`.
- The `osPfs*` Controller Pak API, which the runtime only stubs as "no pak". The game saves
  **only** to the Controller Pak (there is no EEPROM, SRAM or Flash code), so these are
  renamed `hwtr_osPfs*`, ignored by the recompiler and implemented in
  `src/game/controller_pak.cpp` on top of a standard `.mpk` image.

## Audio microcode

The libmus audio task (built at `0x800D2048` from the table at `0x800D2228`) uses
`ucode_boot = 0x800E7AA0` (rspboot), `ucode = 0x800E7B70` and `ucode_data = 0x800FCD20`.
The DMEM image starts with a 16-entry command jump table:

```
10ec 139c 119c 1a64 11c8 17ec 1208 0000 0000 127c 1348 1248 1c84 12d4 02b0 1384
```

The valid IMEM entries are the `extra_indirect_branch_targets` in `aspMain.us.toml`. Entries
`0x0000` and `0x02B0` are unused commands.

## Open questions for the first build

- Whether N64Recomp accepts every function as-is. Hand-written assembly in libmus or
  libultra may need `function_sizes` overrides or stubs.
- Whether the RSPRecomp output needs more indirect branch targets. If it's missing a return
  target, RSPRecomp reports the address at runtime.
- The audio sample rate libmus requests (expected 22050 or 32000 Hz). The output path
  resamples any rate.
- Whether the game probes the pak slot in a way that conflicts with reporting a Rumble Pak
  for force feedback. Setting `"rumble": false` in `config.json` reports no Rumble Pak.
