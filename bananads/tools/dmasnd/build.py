#!/usr/bin/env python3
"""dmasnd.nds: DMA to the geometry FIFO and the sound channels, for checking changes to melonDS's DMA and SPU.

    build.py [outdir]   (default out/; needs clang, ld.lld, llvm-objcopy, as stressrom's build.py)

The ARM9 (dmasnd9.c) draws a few hundred triangles a frame, sent by DMA in GXFIFO mode and as an immediate DMA
to 0x04000400: the top screen shows them. The ARM7 (dmasnd7.c) plays all 16 channels in every format and keys
them on and off; both capture units write the mixers' output to main RAM at 0x02300000, so the mixing shows in
    bananads-difftest --every --frames 120 --a <k=v> --b <k=v> out/dmasnd.nds
"""
import os, struct, subprocess, sys
H = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(H, "out")
os.makedirs(OUT, exist_ok=True); os.chdir(H)
LD = """ENTRY(%s)
SECTIONS { . = %s; .text : { *(.text.start) *(.text*) *(.rodata*) } .data : { *(.data*) } . = ALIGN(4);
 .bss : { *(.bss*) *(COMMON) } /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) } }
"""
open(os.path.join(OUT, "ld9.ld"),"w").write(LD % ("_start","0x02000000")); open(os.path.join(OUT, "ld7.ld"),"w").write(LD % ("_start7","0x02380000"))
F = ["-O2","-fno-unwind-tables","-fno-asynchronous-unwind-tables","-ffreestanding","-fno-builtin","-nostdlib","-Wall","-mfloat-abi=soft"]
def r(c): subprocess.run(c, check=True)
r(["clang","--target=armv5te-none-eabi","-marm",*F,"-c","dmasnd9.c","-o",os.path.join(OUT, "dmasnd9.o")]); r(["clang","--target=armv5te-none-eabi","-marm","-c","crt9.s","-o",os.path.join(OUT, "crt9.o")])
r(["clang","--target=armv4t-none-eabi","-marm",*F,"-c","dmasnd7.c","-o",os.path.join(OUT, "dmasnd7.o")]); r(["clang","--target=armv4t-none-eabi","-marm","-c","crt7.s","-o",os.path.join(OUT, "crt7.o")])
r(["ld.lld","-T",os.path.join(OUT, "ld9.ld"),os.path.join(OUT, "crt9.o"),os.path.join(OUT, "dmasnd9.o"),"-o",os.path.join(OUT, "a9.elf")]); r(["ld.lld","-T",os.path.join(OUT, "ld7.ld"),os.path.join(OUT, "crt7.o"),os.path.join(OUT, "dmasnd7.o"),"-o",os.path.join(OUT, "a7.elf")])
r(["llvm-objcopy","-O","binary",os.path.join(OUT, "a9.elf"),os.path.join(OUT, "a9.bin")]); r(["llvm-objcopy","-O","binary",os.path.join(OUT, "a7.elf"),os.path.join(OUT, "a7.bin")])
arm9 = bytearray(open(os.path.join(OUT, "a9.bin"),"rb").read()); arm7 = open(os.path.join(OUT, "a7.bin"),"rb").read()
def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b
        for _ in range(8): crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc
arm9 += b"\0" * (-len(arm9) % 0x200)
a9off = 0x8000; a7off = a9off + len(arm9)
body = bytearray(0x8000) + arm9 + arm7; body += b"\0" * (-len(body) % 0x200)
h = bytearray(0x200); h[0:12] = b"DMASND".ljust(12, b"\0"); h[12:16] = b"DMAS"; h[16:18] = b"00"
struct.pack_into("<8I", h, 0x20, a9off, 0x02000000, 0x02000000, len(arm9), a7off, 0x02380000, 0x02380000, len(arm7))
struct.pack_into("<2I", h, 0x60, 0x00586000, 0x001808F8); struct.pack_into("<2I", h, 0x80, len(body), 0x200)
struct.pack_into("<H", h, 0x15E, crc16(h[:0x15E])); body[0:0x200] = h
body += b"\xff" * (0x20000 - len(body))
open(os.path.join(OUT, "dmasnd.nds"),"wb").write(body); print(os.path.join(OUT, "dmasnd.nds"), "ARM9", len(arm9), "ARM7", len(arm7), "bytes")
