#!/usr/bin/env python3
"""Build jitloops .nds ROMs: loop shapes for the JIT's loop blocks, with IRQs (see main9.c); cpubench's builder.

    build.py -> out/jitloops-thumb.nds and jitloops-arm.nds (the ARM9 as Thumb / ARM, the ARM7 halted), and
                out/jitloops-thumb-arm7.nds (Thumb, with an ARM7 that polls IPCSYNC and its own IRQ: main7.c)
Needs clang + ld.lld + llvm-objcopy. The header layout is stressrom's (ARM9 at 0x02000000, ARM7 at 0x02380000).
"""
import os, struct, subprocess
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out"); os.makedirs(OUT, exist_ok=True)
# data in pages of its own, away from the code (games keep them apart: a page holding compiled code makes
# its stores take the JIT's invalidation check)
LD = """ENTRY(%s)
SECTIONS {
  . = %s;
  .text : { *(.text.start) *(.text*) *(.rodata*) }
  . = ALIGN(4);
  __itcm_start = .;
  .itcm : { *(.itcmcode) }
  __itcm_end = .;
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
def link(name, objs, entry, base):
    ld = os.path.join(OUT, name + ".ld")
    open(ld, "w").write(LD % (entry, base))
    run(["ld.lld", "-T", ld, *objs, "-o", f"out/{name}.elf"])
    run(["llvm-objcopy", "-O", "binary", f"out/{name}.elf", f"out/{name}.bin"])
    return open(os.path.join(OUT, name + ".bin"), "rb").read()
def arm9(mode):
    run(["clang", "--target=armv5te-none-eabi", "-m" + mode, "-mfloat-abi=soft", *CF, "-c", "main9.c", "-o", f"out/main9-{mode}.o"])
    run(["clang", "--target=armv5te-none-eabi", "-marm", "-c", "crt9.s", "-o", "out/crt9.o"])
    run(["clang", "--target=armv5te-none-eabi", "-marm", "-c", "irq.s", "-o", "out/irq.o"])
    return link("arm9-" + mode, ["out/crt9.o", "out/irq.o", f"out/main9-{mode}.o"], "_start", "0x02000000")
def arm7loops():
    run(["clang", "--target=armv4t-none-eabi", "-mthumb", "-mfloat-abi=soft", *CF, "-c", "main7.c", "-o", "out/main7.o"])
    run(["clang", "--target=armv4t-none-eabi", "-marm", "-c", "crt7.s", "-o", "out/crt7.o"])
    return link("arm7", ["out/crt7.o", "out/main7.o"], "_start7", "0x02380000")
def arm7busy():
    run(["clang", "--target=armv4t-none-eabi", "-mthumb", "-mfloat-abi=soft", *CF, "-c", "main7.c", "-o", "out/main7.o"])
    run(["clang", "--target=armv4t-none-eabi", "-marm", "-c", "crt7.s", "-o", "out/crt7.o"])
    return link("arm7", ["out/crt7.o", "out/main7.o"], "_start7", "0x02380000")
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
    h[0:12] = b"JITLOOPS".ljust(12, b"\0"); h[12:16] = b"JITL"; h[16:18] = b"00"
    struct.pack_into("<8I", h, 0x20, a9off, 0x02000000, 0x02000000, len(a9), a7off, 0x02380000, 0x02380000, len(a7))
    struct.pack_into("<2I", h, 0x60, 0x00586000, 0x001808F8)
    struct.pack_into("<2I", h, 0x80, len(body), 0x200)
    struct.pack_into("<H", h, 0x15E, crc16(h[:0x15E]))
    body[0:0x200] = h
    body += b"\xff" * (0x20000 - len(body)) if len(body) < 0x20000 else b""
    open(os.path.join(OUT, name), "wb").write(body)
    return name
t, a = arm9("thumb"), arm9("arm")
print("arm9", len(t), len(a), "->", rom("jitloops-thumb.nds", t, HALT7), rom("jitloops-arm.nds", a, HALT7), rom("jitloops-thumb-arm7.nds", t, arm7loops()))
