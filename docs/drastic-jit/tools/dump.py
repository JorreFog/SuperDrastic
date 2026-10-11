import json,subprocess,os,sys,concurrent.futures as cf
# Private-analysis helper: writes DraStic-derived decompiler output under $RE_WORK/out (never commit that output).
# Expects $RE_WORK/bin/syms.txt (llvm-nm -S -n of the binary) and a running reamcpd.py (see reaq).
S=os.environ.get("RE_WORK", os.path.expanduser("~/drastic-re"))
syms=[l.split() for l in open(S+"/bin/syms.txt")]
syms=[(int(x[0],16),int(x[1],16),x[2],x[3]) for x in syms if len(x)==4]
ranges=[(0x27d30,0x2f840),(0x8cb0c,0x96990)]
os.makedirs(S+"/out/c",exist_ok=True); os.makedirs(S+"/out/asm",exist_ok=True)
sel=[s for s in syms if s[2] in "Tt" and any(a<=s[0]<b for a,b in ranges)]
def q(tool,args):
    r=subprocess.run([os.path.join(os.path.dirname(os.path.abspath(__file__)),"reaq"),tool,json.dumps(args)],capture_output=True,text=True)
    try: d=json.loads(r.stdout); v=d["raw_result"]
    except Exception: return "// ERROR "+r.stdout[:500]+r.stderr[:500]
    return v["value"] if isinstance(v,dict) and "value" in v else json.dumps(v,indent=1)
for a,sz,t,n in sel:
    addr=hex(a+0x100000)
    if sz>0:
        out=S+f"/out/c/{n}.c"
        if os.path.exists(out): continue
        open(out,"w").write(f"// {n} @ file {a:#x} (ghidra {addr}) size {sz:#x}\n"+q("procedure_pseudo_code",{"procedure":addr}))
    else:
        out=S+f"/out/asm/{n}.s"
        if os.path.exists(out): continue
        open(out,"w").write(f"// {n} @ file {a:#x} (ghidra {addr})\n"+q("procedure_assembly",{"procedure":addr}))
    print(n,flush=True)
