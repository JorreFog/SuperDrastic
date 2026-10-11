# JIT selection, entry and memory window (lead's notes)

Addresses are file VAs (the Ghidra view adds 0x100000). nds_system offsets are given as true offsets from the
nds_system symbol (the decompiler view shows them reduced by 0x4fc000, e.g. 0x362e9a8 for 0x3b2a9a8).

- **main** (0x0e3d0). Order of work [confirmed, disassembly of main]:
  1. initialize_system, process_arguments, load_nds, reset_system;
  2. `_setjmp` on the jump buffer at nds_system+0x3b2a840;
  3. test the use_recompiler byte at nds_system+0x3b2a9a8 (load at 0xe4b0): non-zero -> recompiler_entry(nds_system,
     memory_map_base) (call at 0xe500); zero -> the interpreter loop cpu_next_action_arm7_to_event_update(nds_system).

  main also logs "Memory map offset %p, translate cache pointer %p", passing the constant 0x588000 as the cache
  pointer.
- **Command line** (getopt_long table long_options.11199 at 0x160010, decoded): breakpoint-arm7/arm9 HEX,
  countdown-arm7/arm9 HEX, debug-arm7/arm9, recompiler, interpreter, color-depth N, benchmark N, bench-display N,
  bench-full N, fast-forward, input-record FILE, input-playback FILE. --recompiler / --interpreter set or clear the
  use_recompiler byte at nds_system+0x3b2a9a8; process_arguments (0x0f0e0) receives the nds_system pointer as its
  first argument and writes the byte relative to it [confirmed].
- **initialize_memory** (0x1be80) [confirmed]:
  1. creates a 0x414000-byte shared-memory backing object (shm_open + ftruncate): main RAM (4 MB) plus 0x14000
     bytes more. The first four pointers of the memory struct are base, base+0x400000, base+0x408000 and
     base+0x410000, i.e. main RAM followed by 32 KB, 32 KB and 16 KB regions (likely shared WRAM and similar);
  2. reserves a 64 MB "dynamic memory map low buffer" and maps main RAM into it in 16 KB pages over
     0x2000000..0x3000000 (four mirrors of the 4 MB), logging "Direct mapping main RAM to %x".

  This is a fastmem window: for directly mapped regions, host address = map base + guest address (the base the
  JIT keeps in x10). VRAM has its own 8 MB shared-memory mappings.
