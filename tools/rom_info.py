#!/usr/bin/env python3
"""Checks a Hot Wheels Turbo Racing ROM and optionally converts it to big-endian .z64.

Usage: rom_info.py <rom> [-o hwtr.us.z64]
"""

import argparse
import hashlib
import sys

from romutil import EXPECTED_SHA1, load_rom


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom")
    ap.add_argument("-o", "--output", help="write a big-endian copy here")
    args = ap.parse_args()

    data = load_rom(args.rom, verify=False)
    sha1 = hashlib.sha1(data).hexdigest()
    name = data[0x20:0x34].decode("ascii", "replace").rstrip()
    game_code = data[0x3B:0x3F].decode("ascii", "replace")
    print(f"Internal name: {name}")
    print(f"Game code:     {game_code}")
    print(f"Size:          {len(data)} bytes")
    print(f"SHA-1 (z64):   {sha1}")
    if sha1 != EXPECTED_SHA1:
        print("Not the supported Hot Wheels Turbo Racing (USA) ROM.")
        sys.exit(1)
    print("OK: Hot Wheels Turbo Racing (USA)")

    if args.output:
        with open(args.output, "wb") as f:
            f.write(data)
        print(f"Wrote {args.output}")


if __name__ == "__main__":
    main()
