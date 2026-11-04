#!/usr/bin/env python3
# conv-dfx-test.py: conv-dfx.py's SCREEN_RES_SCALE (NNDDSS's Dot and Retro tiles fix it at 1.0 / 2.0) must come out
# as the frame's width / 256 (1 at 1x, 2 at 2x). DSF_TS is (1/w, 1/h, w, h): .x is 1/w, which made the scale
# ~1/65536 and the dot grid a noise (BananaLinux #76). Also checks the shipped shaders/*.frag. Run with python3 -I.
import os, re, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
fails = 0
def check(ok, what):
    global fails
    if not ok: print('FAIL ' + what); fails += 1
with tempfile.TemporaryDirectory() as t:
    open(os.path.join(t, 't.dsd'), 'w').write('<vertex>\nattribute vec2 a_vertex_coordinate;\nattribute vec2 a_texture_coordinate;\n'
        'uniform vec4 u_texture_size;\nvarying vec2 pixel_no;\nconst float screen_res_scale = SCREEN_RES_SCALE;\n'
        'void main() { pixel_no = a_texture_coordinate * u_texture_size.zw / screen_res_scale; gl_Position = vec4(a_vertex_coordinate, 0.0, 1.0); }\n</vertex>\n'
        '<fragment>\nuniform sampler2D u_texture;\nvarying vec2 pixel_no;\nvoid main() { gl_FragColor = vec4(fract(pixel_no), 0.0, 1.0); }\n</fragment>\n')
    open(os.path.join(t, 't.dfx'), 'w').write('<vheader>\n#define SCREEN_RES_SCALE 2.0\n</vheader>\n<fheader>\n</fheader>\n<header>\n</header>\n'
        '<pass>\nshader=t.dsd\nsampler:u_texture=0\n</pass>\n')
    out = os.path.join(t, 'o.frag')
    r = subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools/shaders/conv-dfx.py'), os.path.join(t, 't.dfx'), out, 't', 'test'],
                       capture_output=True, text=True)
    check(r.returncode == 0, 'conv-dfx.py exited %d: %s' % (r.returncode, r.stderr.strip()[-300:]))
    if r.returncode == 0:
        src = open(out).read()
        defs = re.findall(r'#define\s+SCREEN_RES_SCALE\s+(.*)', src)
        check(bool(defs), 'no SCREEN_RES_SCALE in the converted shader')
        for d in defs: check(d.strip() == '(DSF_TS.z / 256.0)', 'converted SCREEN_RES_SCALE is %s, not (DSF_TS.z / 256.0)' % d.strip())
for f in sorted(os.listdir(os.path.join(ROOT, 'shaders'))):
    if not f.endswith('.frag'): continue
    for d in re.findall(r'#define\s+SCREEN_RES_SCALE\s+(.*)', open(os.path.join(ROOT, 'shaders', f)).read()):
        check(d.strip() == '(DSF_TS.z / 256.0)', 'shaders/%s: SCREEN_RES_SCALE %s' % (f, d.strip()))
# Retro tiles: fade() takes a distance, so pos has to be without its sign once dir has it (with it, one half of each tile
# came out up to 4x too bright: BananaLinux, the owner's "looks really bad" on the RG DS)
rt = open(os.path.join(ROOT, 'shaders', 'nnddss-retro-tiles.frag'), encoding='utf-8', errors='surrogateescape').read()
check(re.search(r'vec2 dir = sign\(pos\);\s*pos = abs\(pos\);', rt) is not None, 'nnddss-retro-tiles.frag: pos keeps its sign after dir')
print('conv-dfx-test: all passed' if not fails else 'conv-dfx-test: %d failed' % fails)
sys.exit(1 if fails else 0)
