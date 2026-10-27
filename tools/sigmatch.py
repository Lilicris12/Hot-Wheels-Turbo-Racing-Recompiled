#!/usr/bin/env python3
"""Identifies library functions in the main segment using n64sym-format signature files.

Matching follows n64sym: a symbol matches when the CRC32 of its bytes (with relocated
fields stripped) equals the signature's CRCs. Relocations of matched symbols are then
followed to name further functions and data symbols.

Usage: sigmatch.py <rom.z64> <signatures.sig> [-o out.txt]
"""

import argparse
import struct
import zlib

from romutil import MAIN_VRAM_START, load_rom, main_segment

RELOC_TYPES = {".targ26": 4, ".hi16": 5, ".lo16": 6}


def parse_sig(path):
    symbols = []
    for line in open(path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if parts[0] in RELOC_TYPES:
            symbols[-1]["relocs"].append(
                (RELOC_TYPES[parts[0]], parts[1], [int(o, 16) for o in parts[2:]]))
        else:
            symbols.append({"name": parts[0], "size": int(parts[1], 16),
                            "crcA": int(parts[2], 16), "crcB": int(parts[3], 16),
                            "relocs": []})
    for sym in symbols:
        sym["reloc_at"] = {o: t for t, _, offs in sym["relocs"] for o in offs}
    return symbols


def stripped(buf, sym):
    out = bytearray(buf[:sym["size"]])
    for off, typ in sym["reloc_at"].items():
        if off + 4 > len(out):
            continue
        if typ == 4:
            out[off] &= 0xFC
            out[off + 1:off + 4] = b"\0\0\0"
        else:
            out[off + 2:off + 4] = b"\0\0"
    return bytes(out)


def candidate_offsets(seg):
    """Likely function starts: after jr $ra + delay slot, stack allocations, and jal targets."""
    cands = set()
    for i in range(0, len(seg) - 12, 4):
        w = struct.unpack_from(">I", seg, i)[0]
        if w == 0x03E00008:
            j = i + 8
            while j < len(seg) and seg[j:j + 4] == b"\0\0\0\0":
                j += 4
            cands.add(j)
        elif (w & 0xFFFF0000) == 0x27BD0000 and (w & 0x8000):
            cands.add(i)
        elif (w >> 26) == 3:
            t = ((w & 0x03FFFFFF) << 2) | (MAIN_VRAM_START & 0xF0000000)
            if MAIN_VRAM_START <= t < MAIN_VRAM_START + len(seg):
                cands.add(t - MAIN_VRAM_START)
    return sorted(cands)


def match(seg, symbols):
    cands = candidate_offsets(seg)
    by_crcA = {}
    for sym in symbols:
        by_crcA.setdefault(sym["crcA"], []).append(sym)
    results = {}
    derived = {}
    for off in cands:
        head = seg[off:off + 8]
        tried = set()
        for sym in _symbols_for(head, by_crcA, symbols):
            if id(sym) in tried or off + sym["size"] > len(seg):
                continue
            tried.add(id(sym))
            if zlib.crc32(stripped(seg[off:off + sym["size"]], sym)) != sym["crcB"]:
                continue
            addr = MAIN_VRAM_START + off
            if addr in results and results[addr]["size"] >= sym["size"]:
                continue
            results[addr] = {"name": sym["name"], "size": sym["size"]}
            _derive(seg, off, sym, derived)
    return results, derived


_head_cache = {}


def _symbols_for(head, by_crcA, symbols):
    # Symbols with no relocation in the first 8 bytes can be looked up by crcA directly.
    found = by_crcA.get(zlib.crc32(head), [])
    if not _head_cache:
        _head_cache["early"] = [s for s in symbols if any(o < 8 for o in s["reloc_at"])]
        for s in symbols:
            if s["size"] < 8:
                _head_cache.setdefault("short", []).append(s)
    out = list(found)
    for sym in _head_cache["early"] + _head_cache.get("short", []):
        n = min(sym["size"], 8)
        if zlib.crc32(stripped(head[:n], sym)) == sym["crcA"]:
            out.append(sym)
    return out


def _derive(seg, off, sym, derived):
    hi = {}
    for typ, name, offs in sym["relocs"]:
        for o in offs:
            if off + o + 4 > len(seg):
                continue
            w = struct.unpack_from(">I", seg, off + o)[0]
            if typ == 4:
                derived.setdefault(name, ((w & 0x03FFFFFF) << 2) | (MAIN_VRAM_START & 0xF0000000))
            elif typ == 5:
                hi[name] = (w & 0xFFFF) << 16
            elif typ == 6 and name in hi:
                imm = w & 0xFFFF
                derived.setdefault(name, (hi[name] + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom")
    ap.add_argument("sig")
    ap.add_argument("-o", "--output")
    args = ap.parse_args()

    seg = main_segment(load_rom(args.rom))
    results, derived = match(seg, parse_sig(args.sig))
    names = {v["name"] for v in results.values()}
    lines = [f"{a:08X} {v['size']:06X} {v['name']}" for a, v in sorted(results.items())]
    for name, addr in sorted(derived.items(), key=lambda kv: kv[1]):
        # Local labels (e.g. foo_text_0090) are not useful as names.
        if name in names or "_text_" in name or "_rodata_" in name or "_data_" in name or "_bss_" in name:
            continue
        lines.append(f"{addr:08X} ?????? {name}")
    out = "\n".join(lines) + "\n"
    if args.output:
        open(args.output, "w").write(out)
    else:
        print(out, end="")


if __name__ == "__main__":
    main()
