#!/usr/bin/env python3
"""Recovers function boundaries in the main segment and writes an N64Recomp symbol file.

The game ships without symbols, so functions are found by a linear sweep over .text:
starting at each function start, control flow is followed (branches, jump tables) until
the function's last reachable terminator. Names come from the signature matcher output
(sigmatch.py) and from symbols/names.toml; everything else is named func_XXXXXXXX.

Usage: find_functions.py <rom.z64> --sigs sigs.txt [--names symbols/names.toml]
                         -o symbols/hwtr.syms.toml
"""

import argparse
import struct
import sys

try:
    import tomllib
except ImportError:  # Python < 3.11
    tomllib = None

from romutil import MAIN_FUNC, MAIN_ROM_START, MAIN_VRAM_START, ENTRYPOINT, load_rom, main_segment

# CPU code ends where the rspboot microcode text begins.
TEXT_END = 0x800E7AA0

# Signature hits smaller than this are only trusted when the name is unique, since
# tiny leaf functions (jr $ra; nop) match many unrelated places.
MIN_UNAMBIGUOUS_SIZE = 0x20


def s16(x):
    return x - 0x10000 if x & 0x8000 else x


class Segment:
    def __init__(self, data):
        self.data = data
        self.n_words = len(data) // 4

    def in_text(self, addr):
        return MAIN_VRAM_START <= addr < TEXT_END and addr % 4 == 0

    def word(self, addr):
        return struct.unpack_from(">I", self.data, addr - MAIN_VRAM_START)[0]

    def in_seg(self, addr):
        return MAIN_VRAM_START <= addr < MAIN_VRAM_START + len(self.data) - 3


def decode_branch(w, pc):
    """Returns (kind, target) for control-flow instructions, else None.

    kind: 'cond' (conditional branch), 'uncond' (b / beq $0,$0), 'j', 'jal',
          'jr_ra', 'jr' (indirect jump), 'jalr', 'eret'.
    """
    op = w >> 26
    rs = (w >> 21) & 31
    rt = (w >> 16) & 31
    off = pc + 4 + (s16(w & 0xFFFF) << 2)
    if op == 0:
        funct = w & 0x3F
        if funct == 8:
            return ("jr_ra" if rs == 31 else "jr", None)
        if funct == 9:
            return ("jalr", None)
        return None
    if op == 1:  # REGIMM
        if rt in (0x10, 0x11, 0x12, 0x13):  # bltzal etc.
            return ("cond", off)
        if rt in (0, 1, 2, 3):
            return ("uncond" if rt == 1 and rs == 0 else "cond", off)  # bgez $0 = b
        return None
    if op == 2:
        return ("j", ((pc + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2))
    if op == 3:
        return ("jal", ((pc + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2))
    if op in (4, 20):  # beq / beql
        return ("uncond" if rs == 0 and rt == 0 else "cond", off)
    if op in (5, 6, 7, 21, 22, 23):
        return ("cond", off)
    if op == 0x10 and (w & 0x3F) == 0x18 and (w >> 25) & 1:
        return ("eret", None)
    if op in (0x11, 0x12) and rs == 8:  # bc1x / bc2x
        return ("cond", off)
    return None


# Opcodes (besides SPECIAL) whose rt is a GPR destination.
_RT_WRITERS = {0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x18, 0x19,
               0x20, 0x21, 0x23, 0x24, 0x25, 0x27, 0x37}


def find_jump_table(seg, jr_pc, func_start):
    """Recovers the targets of a `jr $reg` switch.

    Tracks constant register values (lui/addiu/ori) linearly from the function start, which
    covers both the IDO pattern (lui; addu; lw lo(at)) and the GCC one where the table base
    is materialized once into a saved register and later added to the scaled index.
    """
    known = {0: 0}      # reg -> constant value
    based = {}          # reg -> constant table base plus an unknown index
    loaded_from = {}    # reg -> address it was loaded from
    count = None
    for pc in range(func_start, jr_pc, 4):
        iw = seg.word(pc)
        op = iw >> 26
        rs = (iw >> 21) & 31
        rt = (iw >> 16) & 31
        imm = iw & 0xFFFF
        funct = iw & 0x3F

        if op == 0:
            dest = (iw >> 11) & 31
        elif op in _RT_WRITERS:
            dest = rt
        else:
            continue
        if dest == 0:
            continue

        new_known = new_based = new_loaded = None
        if op == 0x0F:                                          # lui
            new_known = imm << 16
        elif op == 0x09 and rs in known:                        # addiu
            new_known = (known[rs] + s16(imm)) & 0xFFFFFFFF
        elif op == 0x0D and rs in known:                        # ori
            new_known = known[rs] | imm
        elif op == 0x0B:                                        # sltiu gives the case count
            count = imm
        elif op == 0 and funct in (0x21, 0x2D):                 # addu / daddu
            if (rs in known) != (rt in known):
                new_based = known[rs] if rs in known else known[rt]
            elif rs in based or rt in based:
                new_based = based.get(rs, based.get(rt))
        elif op == 0x23:                                        # lw
            if rs in based:
                new_loaded = (based[rs] + s16(imm)) & 0xFFFFFFFF
            elif rs in known:
                new_loaded = (known[rs] + s16(imm)) & 0xFFFFFFFF

        for table, value in ((known, new_known), (based, new_based), (loaded_from, new_loaded)):
            table.pop(dest, None)
            if value is not None:
                table[dest] = value

    reg = (seg.word(jr_pc) >> 21) & 31
    table = loaded_from.get(reg)
    if table is None:
        return []
    targets = []
    limit = count if count else 512
    for i in range(limit):
        addr = table + i * 4
        if not seg.in_seg(addr):
            break
        t = seg.word(addr)
        if not seg.in_text(t) or t < func_start or t > jr_pc + 0x8000:
            break
        targets.append(t)
    return targets


def sweep(seg, call_targets):
    """Linear sweep over .text returning {start: end} for every function."""
    funcs = {}
    pc = MAIN_VRAM_START
    while pc < TEXT_END:
        # Skip inter-function padding.
        while pc < TEXT_END and seg.word(pc) == 0 and pc not in call_targets:
            pc += 4
        if pc >= TEXT_END:
            break
        start = pc
        furthest = pc
        after_terminal_prev = False
        while pc < TEXT_END:
            w = seg.word(pc)
            br = decode_branch(w, pc)
            terminal = False
            if br is not None:
                kind, target = br
                # Branches to another function's entry are tail calls (N64Recomp emits them
                # as calls), so they don't extend this function.
                if kind in ("cond", "uncond") and target > furthest and target not in call_targets:
                    furthest = target
                if kind == "jr":
                    for t in find_jump_table(seg, pc, start):
                        furthest = max(furthest, t)
                    terminal = True
                elif kind == "j":
                    if start <= target < pc + 0x8000 and target not in call_targets and target > start:
                        furthest = max(furthest, target)  # used as a plain branch
                    terminal = True
                elif kind in ("jr_ra", "uncond", "eret"):
                    terminal = True
            if terminal and furthest <= pc + 4:
                end = pc + (4 if br[0] == "eret" else 8)
                break
            # A call target in the middle of what looks like one function means we missed
            # the boundary (e.g. code falling through into the next function); split there.
            pc += 4
            after_terminal = terminal and pc - 4 > start
            if pc in call_targets and pc > start and (pc > furthest or after_terminal_prev):
                end = pc
                break
            after_terminal_prev = after_terminal
        else:
            end = TEXT_END
        funcs[start] = min(end, TEXT_END)
        pc = funcs[start]
    return funcs


def collect_call_targets(seg):
    targets = {ENTRYPOINT, MAIN_FUNC}
    for pc in range(MAIN_VRAM_START, TEXT_END, 4):
        br = decode_branch(seg.word(pc), pc)
        if br and br[0] == "jal" and seg.in_text(br[1]):
            targets.add(br[1])
    return targets


def collect_data_pointers(seg):
    """Words outside .text that point into .text (callbacks, thread entries, vtables)."""
    ptrs = set()
    for addr in range(TEXT_END, MAIN_VRAM_START + len(seg.data) - 3, 4):
        w = seg.word(addr)
        if seg.in_text(w):
            ptrs.add(w)
    return ptrs


def load_sig_names(path):
    by_addr = {}
    counts = {}
    rows = []
    for line in open(path):
        parts = line.split()
        if len(parts) != 3:
            continue
        addr, size, name = int(parts[0], 16), parts[1], parts[2]
        size = None if size.startswith("?") else int(size, 16)
        rows.append((addr, size, name))
        counts[name] = counts.get(name, 0) + 1
    for addr, size, name in rows:
        if size is not None and size < MIN_UNAMBIGUOUS_SIZE and counts[name] > 1:
            continue
        by_addr.setdefault(addr, name)
    return by_addr


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom")
    ap.add_argument("--sigs", help="output of sigmatch.py")
    ap.add_argument("--names", help="TOML of manual names: [names] 0x80000000 = \"name\"")
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()

    seg = Segment(main_segment(load_rom(args.rom)))
    call_targets = collect_call_targets(seg)
    data_ptrs = collect_data_pointers(seg)
    funcs = sweep(seg, call_targets)

    names = {ENTRYPOINT: "entrypoint", MAIN_FUNC: "boot_main"}
    if args.sigs:
        names.update(load_sig_names(args.sigs))
    if args.names:
        if tomllib is None:
            sys.exit("--names requires Python 3.11+")
        for k, v in tomllib.load(open(args.names, "rb")).get("names", {}).items():
            names[int(k, 16)] = v

    # Disambiguate duplicate names (static functions with the same name in different files).
    used = {}
    final = {}
    for start in sorted(funcs):
        name = names.get(start, f"func_{start:08X}")
        if name in used:
            name = f"{name}_{start:08X}"
        used[name] = start
        final[start] = name

    starts = set(funcs)
    missed_calls = sorted(t for t in call_targets if t not in starts)
    missed_ptrs = sorted(t for t in data_ptrs if t not in starts and any(
        s < t < e for s, e in funcs.items()))
    unmatched_names = sorted(a for a in names if a < TEXT_END and a >= MAIN_VRAM_START and a not in starts)

    with open(args.output, "w") as f:
        f.write("# Generated by tools/find_functions.py - do not edit by hand.\n")
        f.write("# Hot Wheels Turbo Racing (USA) main segment.\n\n")
        f.write("[[section]]\n")
        f.write('name = ".text"\n')
        f.write(f"rom = 0x{MAIN_ROM_START:08X}\n")
        f.write(f"vram = 0x{MAIN_VRAM_START:08X}\n")
        f.write(f"size = 0x{TEXT_END - MAIN_VRAM_START:X}\n\n")
        f.write("functions = [\n")
        for start in sorted(funcs):
            f.write(f'    {{ name = "{final[start]}", vram = 0x{start:08X}, size = 0x{funcs[start] - start:X} }},\n')
        f.write("]\n")

    print(f"{len(funcs)} functions, {sum(1 for a in final if not final[a].startswith('func_'))} named")
    if missed_calls:
        print(f"warning: {len(missed_calls)} jal targets are not function starts:",
              " ".join(f"{a:08X}" for a in missed_calls[:20]))
    if unmatched_names:
        print(f"warning: {len(unmatched_names)} named addresses are not function starts:",
              " ".join(f"{a:08X}={names[a]}" for a in unmatched_names[:20]))
    if missed_ptrs:
        print(f"note: {len(missed_ptrs)} data words point inside functions (likely jump tables or data)")


if __name__ == "__main__":
    main()
