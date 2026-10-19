#!/usr/bin/env python3
"""Build gxstm.nds: packed display lists sent to the GX FIFO as games send them (STM bursts, DMA), with the geometry
engine's readable results logged to main RAM at 0x02200000, for bananads-difftest (see main9.c).
Needs clang + ld.lld + llvm-objcopy. The builder and header are cpubench's (ARM9 at 0x02000000, ARM7 halted).
"""
import os, struct, subprocess
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out"); os.makedirs(OUT, exist_ok=True)
LD = """ENTRY(_start)
SECTIONS {
  . = 0x02000000;
  .text : { *(.text.start) *(.text*) *(.rodata*) }
  . = ALIGN(0x10000);
  .data : { *(.data*) }
  . = ALIGN(4);
  __bss_start = .;
  .bss : { *(.bss*) *(COMMON) }
  . = ALIGN(4);
  __bss_end = .;
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) }
}
"""
CF = ["-O2", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-ffreestanding", "-fno-builtin", "-nostdlib", "-Wall"]
def run(cmd): subprocess.run(cmd, check=True, cwd=HERE)
def arm9():
    run(["clang", "--target=armv5te-none-eabi", "-mthumb", "-mfloat-abi=soft", *CF, "-c", "main9.c", "-o", "out/main9.o"])
    run(["clang", "--target=armv5te-none-eabi", "-marm", "-c", "../cpubench/crt9.s", "-o", "out/crt9.o"])
    open(os.path.join(OUT, "arm9.ld"), "w").write(LD)
    run(["ld.lld", "-T", "out/arm9.ld", "out/crt9.o", "out/main9.o", "-o", "out/arm9.elf"])
    run(["llvm-objcopy", "-O", "binary", "out/arm9.elf", "out/arm9.bin"])
    return open(os.path.join(OUT, "arm9.bin"), "rb").read()
HALT7 = struct.pack("<5I", 0xE59F0008, 0xE3A01080, 0xE5C01001, 0xEAFFFFFB, 0x04000300)   # HALTCNT <- 0x80, loop
def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b
        for _ in range(8): crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc
def rom(name, a9, a7):
    a9 = bytearray(a9) + b"\0" * (-len(a9) % 0x200)
    a9off = 0x8000; a7off = a9off + len(a9)
    body = bytearray(0x8000) + a9 + a7
    body += b"\0" * (-len(body) % 0x200)
    h = bytearray(0x200)
    h[0:12] = b"GXSTM".ljust(12, b"\0"); h[12:16] = b"GXST"; h[16:18] = b"00"
    struct.pack_into("<8I", h, 0x20, a9off, 0x02000000, 0x02000000, len(a9), a7off, 0x02380000, 0x02380000, len(a7))
    struct.pack_into("<2I", h, 0x60, 0x00586000, 0x001808F8)
    struct.pack_into("<2I", h, 0x80, len(body), 0x200)
    struct.pack_into("<H", h, 0x15E, crc16(h[:0x15E]))
    body[0:0x200] = h
    body += b"\xff" * (0x20000 - len(body)) if len(body) < 0x20000 else b""
    open(os.path.join(OUT, name), "wb").write(body)
    return name
a9 = arm9()
print("arm9", len(a9), "->", rom("gxstm.nds", a9, HALT7))
