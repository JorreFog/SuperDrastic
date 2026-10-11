#!/usr/bin/env python3
"""mkrom.py <name> <arm9.s> <arm7.s> [-DSYM ...]: assemble an ARM9 program (load/run 0x02000000) and an ARM7 program
(load/run 0x02380000) with clang/ld.lld and wrap them in an NDS ROM (header as in ../rom/build.py). Output out/<name>.nds"""
import os, struct, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__)); OUT = os.path.join(HERE, "out"); os.makedirs(OUT, exist_ok=True)
name, s9, s7 = sys.argv[1:4]; defs = sys.argv[4:]
def run(cmd): subprocess.run(cmd, check=True, cwd=HERE)
def build(src, addr, tag):
    open(os.path.join(OUT, "ld%s.ld" % tag), "w").write(
        "ENTRY(_start)\nSECTIONS { . = 0x%08x; .text : { *(.text.start) *(.text*) *(.rodata*) } }\n" % addr)
    run(["clang", "--target=armv5te-none-eabi", *["-Wa,-defsym,%s=1" % d for d in defs], "-c", src, "-o", "out/%s_%s.o" % (name, tag)])
    run(["ld.lld", "-T", "out/ld%s.ld" % tag, "out/%s_%s.o" % (name, tag), "-o", "out/%s_%s.elf" % (name, tag)])
    run(["llvm-objcopy", "-O", "binary", "out/%s_%s.elf" % (name, tag), "out/%s_%s.bin" % (name, tag)])
    return bytearray(open(os.path.join(OUT, "%s_%s.bin" % (name, tag)), "rb").read())
arm9 = build(s9, 0x02000000, "9"); arm7 = build(s7, 0x02380000, "7")
def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b
        for _ in range(8): crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc
a9 = arm9 + b"\0" * (-len(arm9) % 0x200)
a9off = 0x8000; a7off = a9off + len(a9)
body = bytearray(0x8000) + a9 + arm7
body += b"\0" * (-len(body) % 0x200)
hd = bytearray(0x200)
hd[0:12] = name.upper()[:12].encode().ljust(12, b"\0"); hd[12:16] = b"JSMC"; hd[16:18] = b"00"
chip = 0x20000
while chip < len(body): chip *= 2
hd[0x14] = (chip // 0x20000).bit_length() - 1
struct.pack_into("<8I", hd, 0x20, a9off, 0x02000000, 0x02000000, len(a9), a7off, 0x02380000, 0x02380000, len(arm7))
struct.pack_into("<2I", hd, 0x60, 0x00586000, 0x001808F8)
struct.pack_into("<2I", hd, 0x80, len(body), 0x200)
struct.pack_into("<H", hd, 0x15E, crc16(hd[:0x15E]))
body[0:0x200] = hd
body += b"\xff" * (chip - len(body))
open(os.path.join(OUT, name + ".nds"), "wb").write(body)
print("%s: arm9 %d bytes, arm7 %d bytes" % (name, len(arm9), len(arm7)))
