#!/bin/sh
# Checks BananaJIT's encoder against LLVM's assembler. Usage: jit_encodings.sh <jit-encodings binary>
# Skips (exit 0 with a note) when llvm-mc isn't installed.
set -eu
BIN=$1
MC=$(command -v llvm-mc || ls /usr/bin/llvm-mc-* 2>/dev/null | head -1 || true)
if [ -z "$MC" ]; then echo "llvm-mc not found: skipped"; exit 0; fi
fails=0; n=0
"$BIN" > /tmp/bananajit-enc.$$
# Each record may hold several instructions separated by literal newlines; read records by '|'.
python3 - "$MC" /tmp/bananajit-enc.$$ <<'EOF'
import subprocess, sys, re
mc, path = sys.argv[1], sys.argv[2]
text = open(path).read()
recs = re.findall(r'(.*?)\|([0-9a-f,]+)\n', text, re.S)
fails = 0
for asm, words in recs:
    out = subprocess.run([mc, "-triple=aarch64", "-show-encoding"], input=asm + "\n",
                         capture_output=True, text=True)
    enc = re.findall(r'encoding: \[([^\]]*)\]', out.stdout)
    got = []
    for e in enc:
        b = [int(x, 16) for x in e.split(',')]
        got.append("%08x" % (b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24))
    want = words.split(',')
    if got != want:
        fails += 1
        print("FAIL %r: llvm-mc %s, ours %s %s" % (asm, got, want, out.stderr.strip()))
print("%d encodings checked, %d wrong" % (len(recs), fails))
sys.exit(1 if fails else 0)
EOF
rc=$?
rm -f /tmp/bananajit-enc.$$
exit $rc
