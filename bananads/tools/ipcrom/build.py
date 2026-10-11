#!/usr/bin/env python3
"""ipctest.nds: an ARM9/ARM7 handshake ROM, for the scheduler slice (core.slice, docs/SETTINGS.md).

    build.py [outdir]   (default out/; needs clang, ld.lld, llvm-objcopy, as stressrom's build.py)

The ARM9 loops: a word through the IPC FIFO (the ARM7, halted until the FIFO wakes it, answers 3v+1), a main-RAM
mailbox the ARM7 copies back, IPCSYNC with an IRQ to the ARM7 which mirrors it, then a little work without IPC.
Results in main RAM at 0x02300000: [0] round trips, [1] FIFO errors, [2] IPCSYNC timeouts, [3] mailbox timeouts,
[4] frames, [16+f] round trips in frame f. The errors must stay 0 at any slice; the round trips show how much a
slice slows a handshake:
    bananads-difftest --frames 120 --a core.slice=auto --b core.slice=64 --dump 0x02300000:0x14 out/ipctest.nds
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
r(["clang","--target=armv5te-none-eabi","-marm",*F,"-c","ipc9.c","-o",os.path.join(OUT, "ipc9.o")]); r(["clang","--target=armv5te-none-eabi","-marm","-c","crt9.s","-o",os.path.join(OUT, "crt9.o")])
r(["clang","--target=armv4t-none-eabi","-marm",*F,"-c","ipc7.c","-o",os.path.join(OUT, "ipc7.o")]); r(["clang","--target=armv4t-none-eabi","-marm","-c","crt7.s","-o",os.path.join(OUT, "crt7.o")])
r(["ld.lld","-T",os.path.join(OUT, "ld9.ld"),os.path.join(OUT, "crt9.o"),os.path.join(OUT, "ipc9.o"),"-o",os.path.join(OUT, "a9.elf")]); r(["ld.lld","-T",os.path.join(OUT, "ld7.ld"),os.path.join(OUT, "crt7.o"),os.path.join(OUT, "ipc7.o"),"-o",os.path.join(OUT, "a7.elf")])
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
h = bytearray(0x200); h[0:12] = b"IPCTEST".ljust(12, b"\0"); h[12:16] = b"IPCT"; h[16:18] = b"00"
struct.pack_into("<8I", h, 0x20, a9off, 0x02000000, 0x02000000, len(arm9), a7off, 0x02380000, 0x02380000, len(arm7))
struct.pack_into("<2I", h, 0x60, 0x00586000, 0x001808F8); struct.pack_into("<2I", h, 0x80, len(body), 0x200)
struct.pack_into("<H", h, 0x15E, crc16(h[:0x15E])); body[0:0x200] = h
body += b"\xff" * (0x20000 - len(body))
open(os.path.join(OUT, "ipctest.nds"),"wb").write(body); print(os.path.join(OUT, "ipctest.nds"), "ARM9", len(arm9), "ARM7", len(arm7), "bytes")
