#!/usr/bin/env python3
"""Build ds2dtest.nds (main9.c says what it exercises): the 2D test ROM for render.2d_thread's exactness checks.
Needs clang + ld.lld (armv5te for the ARM9; the ARM7 is a halt stub). Writes its outputs next to this file:
    python3 build.py && bananads --frontend headless ... ds2dtest.nds
"""
import os, struct, subprocess
H = os.path.dirname(os.path.abspath(__file__))
def run(c): subprocess.run(c, check=True, cwd=H)
LD = "ENTRY(_start)\nSECTIONS {\n . = 0x02000000;\n .text : { *(.text.start) *(.text*) *(.rodata*) }\n .data : { *(.data*) }\n . = ALIGN(4);\n __bss_start = .;\n .bss : { *(.bss*) *(COMMON) }\n . = ALIGN(4);\n __bss_end = .;\n /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) }\n}\n"
open(os.path.join(H, "ld9.ld"), "w").write(LD)
T = ["--target=armv5te-none-eabi", "-marm", "-mfloat-abi=soft"]
run(["clang", *T, "-O2", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-ffreestanding", "-fno-builtin", "-nostdlib", "-Wall", "-c", "main9.c", "-o", "main9.o"])
run(["clang", *T, "-c", "crt9.s", "-o", "crt9.o"])
run(["ld.lld", "-T", "ld9.ld", "crt9.o", "main9.o", "-o", "arm9.elf"])
run(["llvm-objcopy", "-O", "binary", "arm9.elf", "arm9.bin"])
a9 = bytearray(open(os.path.join(H, "arm9.bin"), "rb").read())
arm7 = struct.pack("<5I", 0xE59F0008, 0xE3A01080, 0xE5C01001, 0xEAFFFFFB, 0x04000300)
def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b
        for _ in range(8): crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc
a9 += b"\0" * (-len(a9) % 0x200)
a9off = 0x8000; a7off = a9off + len(a9)
body = bytearray(0x8000) + a9 + arm7
body += b"\0" * (-len(body) % 0x200)
h = bytearray(0x200)
h[0:12] = b"DS2DTEST".ljust(12, b"\0"); h[12:16] = b"D2DT"; h[16:18] = b"00"
struct.pack_into("<8I", h, 0x20, a9off, 0x02000000, 0x02000000, len(a9), a7off, 0x02380000, 0x02380000, len(arm7))
struct.pack_into("<2I", h, 0x60, 0x00586000, 0x001808F8)
struct.pack_into("<2I", h, 0x80, len(body), 0x200)
struct.pack_into("<H", h, 0x15E, crc16(h[:0x15E]))
body[0:0x200] = h
body += b"\xff" * (0x20000 - len(body)) if len(body) < 0x20000 else b""
open(os.path.join(H, "ds2dtest.nds"), "wb").write(body)
print("ok", len(a9))
