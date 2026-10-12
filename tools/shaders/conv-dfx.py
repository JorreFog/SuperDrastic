# conv-dfx.py <shader.dfx> <out.frag> <title> <origin>: a single-pass DraStic post-FX shader (.dfx + .dsd, DraStic's and
# Anbernic NNDDSS's format) as a SuperDrastic/BananaDS fragment shader. Its vertex stage runs per pixel at the top of
# the fragment shader (its outputs are linear in the texture coordinate, so the result is the same); DraStic's
# built-ins are mapped: u_texture_size (1/w, 1/h, w, h), u_target_size -> u_output_size, u_time -> 0.
import sys, re, os
dfx, out, title, origin = sys.argv[1:5]
d = open(dfx, encoding='latin-1').read().replace('\r', '')
def tag(t, txt):
    return '\n'.join(re.findall(r'<%s>\n?(.*?)</%s>' % (t, t), txt, re.S))
passes = re.findall(r'<pass>(.*?)</pass>', d, re.S)
if len(passes) != 1: sys.exit('%d passes' % len(passes))
p = passes[0]
dsd_name = re.search(r'shader=(\S+)', p).group(1)
samplers = re.findall(r'sampler:(\w+)=(\d+)', p)
dsd = open(os.path.join(os.path.dirname(dfx), dsd_name), encoding='latin-1').read().replace('\r', '')
vh, fh, hd = tag('vheader', d), tag('fheader', d), tag('header', d)
inc = ''.join(open(os.path.join(os.path.dirname(dfx), f), encoding='latin-1').read().replace('\r','') for f in re.findall(r'<include>.*?file=(\S+).*?</include>', d, re.S))
vs, fs = tag('vertex', dsd), tag('fragment', dsd)
def strip_decls(code, keep_varyings_as_globals):
    outl, glob = [], []
    for l in code.split('\n'):
        s = l.strip()
        if re.match(r'(attribute|uniform)\s', s): continue
        m = re.match(r'varying\s+(?:(?:lowp|mediump|highp)\s+)?(\w+\s+\w+\s*;)', s)
        if m:
            if keep_varyings_as_globals: glob.append(m.group(1))
            outl.append('// (varying, a global here) ' + s if keep_varyings_as_globals else '')
            if keep_varyings_as_globals: outl[-1] = m.group(1)
            continue
        if re.match(r'#version', s) or re.match(r'precision\s', s): continue
        outl.append(l)
    return '\n'.join(outl)
vcode = strip_decls(vs, True)
vcode = re.sub(r'^\s*gl_Position\s*=.*$', '', vcode, flags=re.M)
vcode = re.sub(r'\bvoid\s+main\s*\(\s*(void)?\s*\)', 'void dsf_vert()', vcode)
vcode = re.sub(r'\ba_texture_coordinate\b', 'v_texcoord', vcode)
vcode = re.sub(r'\ba_vertex_coordinate\b', '(v_texcoord * 2.0 - 1.0)', vcode)
fcode = strip_decls(fs, False)
fcode = re.sub(r'\bvoid\s+main\s*\(\s*(void)?\s*\)', 'void dsf_frag()', fcode)
# the screen scale some shaders fix at 1.0 (NNDDSS's Dot: 2.0 at 2x): from the frame's size instead
pre_all = vh + '\n' + hd + '\n' + inc
pre_all = re.sub(r'#define\s+SCREEN_RES_SCALE\s+[0-9.]+', '#define SCREEN_RES_SCALE (u_texture_size.x / 256.0)', pre_all)
vcode = re.sub(r'#define\s+SCREEN_RES_SCALE\s+[0-9.]+', '#define SCREEN_RES_SCALE (u_texture_size.x / 256.0)', vcode)
vcode = re.sub(r'const\s+float\s+screen_res_scale\s*=\s*SCREEN_RES_SCALE\s*;', '#define screen_res_scale SCREEN_RES_SCALE', vcode)
fhead = re.sub(r'#if GL_ES\s*\n\s*(#ifdef GL_FRAGMENT_PRECISION_HIGH.*?#endif|precision \w+ float;)\s*\n#endif', '', fh, flags=re.S)
body = pre_all + '\n' + vcode + '\n' + fhead + '\n' + hd + '\n' + fcode
body = re.sub(r'#define\s+SCREEN_RES_SCALE\s+[0-9.]+', '#define SCREEN_RES_SCALE (u_texture_size.x / 256.0)', body)
body = re.sub(r'^\s*uniform\s+[^;]*;\s*$', '', body, flags=re.M)   # DraStic's built-ins and samplers: ours
body = re.sub(r'^\s*precision\s+\w+\s+\w+\s*;\s*$', '', body, flags=re.M)
body = re.sub(r'\bu_texture_size\b', 'DSF_TS', body)
body = re.sub(r'\bu_target_size\b', 'u_output_size', body)
body = re.sub(r'\bu_time\b', '0.0', body)
# the framebuffer's sampler under DraStic's name
sdefs = ''.join('#define %s u_texture\n' % n for n, i in samplers if n != 'u_texture')
src = '''// %s: %s, as a BananaOS shader (SuperDrastic and BananaDS) for a DS screen drawn to the panel.
// Converted by tools/shaders/conv-dfx.py from DraStic's post-FX format (.dfx + .dsd): its vertex stage computed per
// pixel, DraStic's built-ins mapped onto u_texture, u_texture_size, u_output_size and v_texcoord.
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
varying vec2 v_texcoord;
uniform sampler2D u_texture;
uniform vec2 u_texture_size;
uniform vec2 u_output_size;
#define DSF_TS vec4(1.0 / u_texture_size, u_texture_size)
%s
%s
void main()
{
    dsf_vert();
    dsf_frag();
    gl_FragColor = SWIZ(gl_FragColor);
}
''' % (title, origin, sdefs, body)
src = re.sub(r'\n{3,}', '\n\n', src)
open(out, 'w').write(src)
