import sys, angr, logging
logging.getLogger('angr').setLevel(logging.ERROR); logging.getLogger('cle').setLevel(logging.ERROR); logging.getLogger('pyvex').setLevel(logging.ERROR)
p = angr.Project(sys.argv[1], auto_load_libs=False, main_opts={'base_addr': 0})
names = sys.argv[2:]
for n in names:
    s = p.loader.find_symbol(n)
    if s is None: print("// no symbol", n); continue
    cfg = p.analyses.CFGFast(regions=[(s.rebased_addr, s.rebased_addr + max(s.size, 4))], normalize=True, function_starts=[s.rebased_addr])
    f = cfg.kb.functions.get(s.rebased_addr)
    try:
        d = p.analyses.Decompiler(f, cfg=cfg.model)
        print(f"// ===== {n} @ {s.rebased_addr:#x}"); print(d.codegen.text if d.codegen else "// no codegen")
    except Exception as e:
        print("// fail", n, e)
