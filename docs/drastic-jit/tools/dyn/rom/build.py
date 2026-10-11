#!/usr/bin/env python3
"""Build jittest.nds: a homebrew NDS ROM whose ARM9 program (jit9.s) exercises DraStic's JIT.
Header layout copied from SuperDrastic/stressrom/build.py (ARM9 at ROM 0x8000 -> RAM 0x02000000; ARM7 halt stub).
Needs clang + ld.lld + llvm-objcopy. Output: out/jittest.nds, out/arm9.elf (for llvm-objdump -d)."""
import os, struct, subprocess, sys
DEFS = sys.argv[2:]; NAME = sys.argv[1] if len(sys.argv) > 1 else "jittest"   # build.py [name] [NO_SMC] [NO_MCR]
HERE = os.path.dirname(os.path.abspath(__file__)); OUT = os.path.join(HERE, "out"); os.makedirs(OUT, exist_ok=True)
LD = """ENTRY(_start)
SECTIONS { . = 0x02000000; .text : { *(.text.start) *(.text*) *(.rodata*) } .data 0x02100000 (NOLOAD) : { *(.data*) } }
"""
open(os.path.join(OUT, "ld9.ld"), "w").write(LD)
def run(cmd): subprocess.run(cmd, check=True, cwd=HERE)
run(["clang", "--target=armv5te-none-eabi", *["-Wa,-defsym,%s=1" % d for d in DEFS], "-c", "jit9.s", "-o", "out/jit9.o"])
run(["ld.lld", "-T", "out/ld9.ld", "out/jit9.o", "-o", "out/arm9.elf"])
run(["llvm-objcopy", "-O", "binary", "out/arm9.elf", "out/arm9.bin"])
arm9 = bytearray(open(os.path.join(OUT, "arm9.bin"), "rb").read())
arm7 = struct.pack("<5I", 0xE59F0008, 0xE3A01080, 0xE5C01001, 0xEAFFFFFB, 0x04000300)  # HALTCNT loop
def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b
        for _ in range(8): crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc
a9 = arm9 + b"\0" * (-len(arm9) % 0x200)
a9off = 0x8000; a7off = a9off + len(a9)
body = bytearray(0x8000) + a9 + arm7
body += b"\0" * (-len(body) % 0x200)
h = bytearray(0x200)
h[0:12] = b"JITTEST".ljust(12, b"\0"); h[12:16] = b"JITT"; h[16:18] = b"00"
struct.pack_into("<8I", h, 0x20, a9off, 0x02000000, 0x02000000, len(a9), a7off, 0x02380000, 0x02380000, len(arm7))
struct.pack_into("<2I", h, 0x60, 0x00586000, 0x001808F8)
struct.pack_into("<2I", h, 0x80, len(body), 0x200)
struct.pack_into("<H", h, 0x15E, crc16(h[:0x15E]))
body[0:0x200] = h
body += b"\xff" * (0x20000 - len(body))
open(os.path.join(OUT, NAME + ".nds"), "wb").write(body)
print("arm9 %d bytes -> out/%s.nds" % (len(arm9), NAME))
