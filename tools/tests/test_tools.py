#!/usr/bin/env python3
"""Unit tests for the analysis tools using small synthetic MIPS programs (no ROM needed)."""

import os
import struct
import sys
import unittest
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import find_functions as ff  # noqa: E402
import sigmatch  # noqa: E402

BASE = ff.MAIN_VRAM_START

NOP = 0x00000000
JR_RA = 0x03E00008


def jal(target):
    return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)


def j(target):
    return 0x08000000 | ((target >> 2) & 0x03FFFFFF)


def beqz(rs, pc, target):
    return (4 << 26) | (rs << 21) | (((target - pc - 4) >> 2) & 0xFFFF)


def addiu_sp(n):
    return 0x27BD0000 | (n & 0xFFFF)


def assemble(words, data_words=()):
    code = b"".join(struct.pack(">I", w) for w in words)
    data = b"".join(struct.pack(">I", w) for w in data_words)
    return code, data


class SweepTests(unittest.TestCase):
    def run_sweep(self, words, data_words=()):
        code, data = assemble(words, data_words)
        old_end = ff.TEXT_END
        ff.TEXT_END = BASE + len(code)
        try:
            seg = ff.Segment(code + data)
            calls = ff.collect_call_targets(seg)
            return ff.sweep(seg, calls)
        finally:
            ff.TEXT_END = old_end

    def test_simple_functions(self):
        f1 = BASE
        f2 = BASE + 0x10
        # f1: addiu; jal f2; jr ra; addiu (delay slot). f2: jr ra; nop. Then padding.
        words = [addiu_sp(-0x18), jal(f2), JR_RA, addiu_sp(0x18),
                 JR_RA, NOP, NOP, NOP]
        funcs = self.run_sweep(words)
        self.assertEqual(funcs[f1], f1 + 0x10)
        self.assertIn(f2, funcs)

    def test_forward_branch_keeps_function_open(self):
        # beqz over an early return: the function must extend to the later jr $ra.
        pc_branch = BASE + 0x4
        target = BASE + 0x14
        words = [
            addiu_sp(-8),
            beqz(4, pc_branch, target),
            NOP,
            JR_RA,
            NOP,
            JR_RA,      # target
            NOP,
        ]
        funcs = self.run_sweep(words)
        self.assertEqual(list(funcs.items()), [(BASE, BASE + 0x1C)])

    def test_branch_to_other_function_is_tail_call(self):
        # A conditional branch into another function's entry must not merge the two.
        other = BASE + 0x10
        words = [
            beqz(4, BASE, other),
            NOP,
            JR_RA,
            NOP,
            JR_RA,      # other: called via jal below
            NOP,
            jal(other),
            NOP,
            JR_RA,
            NOP,
        ]
        funcs = self.run_sweep(words)
        self.assertEqual(funcs[BASE], BASE + 0x10)
        self.assertEqual(funcs[other], other + 0x8)

    def test_function_starting_with_nop(self):
        callee = BASE + 0x8
        words = [JR_RA, NOP, NOP, JR_RA, NOP, jal(callee), NOP, JR_RA, NOP]
        funcs = self.run_sweep(words)
        self.assertIn(callee, funcs)

    def test_jump_table_extends_function(self):
        # lui at, hi(table); addu at, at, t0; lw t0, lo(table)(at); jr t0
        n_words = 12
        table = BASE + n_words * 4
        hi = (table + 0x8000) >> 16
        lo = table & 0xFFFF
        case0 = BASE + 0x18
        case1 = BASE + 0x20
        words = [
            0x3C010000 | hi,                 # lui at, hi
            0x00280821,                      # addu at, at, t0
            0x8C280000 | lo,                 # lw t0, lo(at)
            0x01000008,                      # jr t0
            NOP,
            NOP,
            JR_RA,                           # case0
            NOP,
            JR_RA,                           # case1
            NOP,
            NOP,
            NOP,
        ]
        funcs = self.run_sweep(words, [case0, case1])
        self.assertEqual(funcs[BASE], BASE + 0x28)

    def test_gcc_jump_table_with_saved_base(self):
        # lui t5, hi; addiu t5, t5, lo; ...; sll v0, v1, 2; addu v0, v0, t5; lw v0, 0(v0); jr v0
        n_words = 14
        table = BASE + n_words * 4
        hi = (table + 0x8000) >> 16
        lo = table & 0xFFFF
        case1 = BASE + 0x28
        words = [
            0x3C0D0000 | hi,                 # lui t5, hi
            0x25AD0000 | lo,                 # addiu t5, t5, lo
            0x2C620002,                      # sltiu v0, v1, 2
            0x00031080,                      # sll v0, v1, 2
            0x004D1021,                      # addu v0, v0, t5
            0x8C420000,                      # lw v0, 0(v0)
            0x00400008,                      # jr v0
            NOP,
            JR_RA,                           # case0
            NOP,
            JR_RA,                           # case1
            NOP,
            NOP,
            NOP,
        ]
        funcs = self.run_sweep(words, [BASE + 0x20, case1])
        self.assertEqual(funcs[BASE], BASE + 0x30)


class SigmatchTests(unittest.TestCase):
    def test_strip_and_match(self):
        body = struct.pack(">IIII", addiu_sp(-0x18), jal(0x80123454), NOP, JR_RA)
        stripped = bytearray(body)
        stripped[4] &= 0xFC
        stripped[5:8] = b"\0\0\0"
        sym = {
            "name": "foo", "size": len(body),
            "crcA": zlib.crc32(bytes(stripped[:8])), "crcB": zlib.crc32(bytes(stripped)),
            "relocs": [(4, "bar", [4])], "reloc_at": {4: 4},
        }
        seg = body + struct.pack(">I", NOP)
        results, derived = sigmatch.match(seg, [sym])
        self.assertEqual(results[BASE]["name"], "foo")
        self.assertEqual(derived["bar"], 0x80123454)


if __name__ == "__main__":
    unittest.main()
