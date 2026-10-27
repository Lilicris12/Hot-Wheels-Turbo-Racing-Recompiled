"""Shared ROM helpers for the Hot Wheels Turbo Racing recompilation tools."""

import hashlib
import struct
import sys

# Hot Wheels Turbo Racing (USA), NHWE, big-endian .z64
EXPECTED_SHA1 = "94913a07e49005ffd43dfdfa16ff5862bdb93748"
EXPECTED_MD5 = "4311a1aef1898678331f7e3486055307"

# Main code segment layout, recovered from the entrypoint at 0x80060400:
#   bzero(0x80101E70, 0x4CE60); sp = 0x80143840; jr 0x800CD4C0
ENTRYPOINT = 0x80060400
MAIN_ROM_START = 0x1000
BSS_START = 0x80101E70
BSS_SIZE = 0x4CE60
MAIN_VRAM_START = ENTRYPOINT
MAIN_ROM_END = MAIN_ROM_START + (BSS_START - MAIN_VRAM_START)
MAIN_FUNC = 0x800CD4C0


def load_rom(path, verify=True):
    data = open(path, "rb").read()
    magic = data[:4]
    if magic == b"\x37\x80\x40\x12":  # .v64 byteswapped
        data = b"".join(data[i + 1:i + 2] + data[i:i + 1] for i in range(0, len(data), 2))
    elif magic == b"\x40\x12\x37\x80":  # .n64 little-endian
        data = b"".join(data[i:i + 4][::-1] for i in range(0, len(data), 4))
    if data[:4] != b"\x80\x37\x12\x40":
        sys.exit(f"{path}: not an N64 ROM image")
    if verify:
        sha1 = hashlib.sha1(data).hexdigest()
        if sha1 != EXPECTED_SHA1:
            sys.exit(f"{path}: SHA1 {sha1} does not match Hot Wheels Turbo Racing (USA) "
                     f"({EXPECTED_SHA1})")
    return data


def word(data, off):
    return struct.unpack_from(">I", data, off)[0]


def main_segment(rom):
    """Returns the bytes of the main code+data segment as loaded at MAIN_VRAM_START."""
    return rom[MAIN_ROM_START:MAIN_ROM_END]


def vram_to_rom(vram):
    return vram - MAIN_VRAM_START + MAIN_ROM_START
