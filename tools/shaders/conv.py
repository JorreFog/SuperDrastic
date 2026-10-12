# conv.py <libretro .glsl> <out .frag> <title>: libretro's single-pass GLSL (VERTEX/FRAGMENT in one file) as a
# SuperDrastic/BananaDS fragment shader (u_texture, u_texture_size, u_output_size, v_texcoord, SWIZ)
import sys, re
src, out, title = sys.argv[1], sys.argv[2], sys.argv[3]
t = open(src, encoding='latin-1').read()
head = t.split('#if defined(VERTEX)')[0]
m = re.search(r'#elif defined\(FRAGMENT\)(.*)#endif\s*$', t, re.S)
if not m: sys.exit('no fragment section')
frag = m.group(1)
vert = t.split('#if defined(VERTEX)')[1].split('#elif defined(FRAGMENT)')[0]
# varyings the vertex stage computes, besides TEX0: known ones become macros
extra = []
known = {
  'InvInputHeight': '(1.0 / u_texture_size.y)',
  'invDims': '(1.0 / u_texture_size)',
  'precalc_texel': '(v_texcoord * u_texture_size)',
  'precalc_scale': '(max(floor(u_output_size / u_texture_size), vec2(1.0, 1.0)))',
  'omega': '(vec2(3.141592654 * u_output_size.x, 2.0 * 3.141592654 * u_texture_size.y))',
  'ps': '(1.0 / u_texture_size)',
  'dx': '(1.0 / u_texture_size.x)', 'dy': '(1.0 / u_texture_size.y)',
  'scale': '(u_output_size / u_texture_size)',
  'OGL2Pos': '(v_texcoord * u_texture_size)',
  'TextureSize_': 'u_texture_size',
  'COL0': 'vec4(1.0)',
  'invSize': '(1.0 / u_texture_size)',
  'maskFade': '(0.3333 * MASK_FADE)',
}
for v in re.findall(r'(?:COMPAT_VARYING|varying)\s+(?:COMPAT_PRECISION\s+)?\w+\s+(\w+)\s*;', vert):
    if v == 'TEX0': continue
    if v not in known: sys.exit('unknown varying ' + v)
    extra.append('#define %s %s' % (v, known[v]))
# drop declarations the adapter provides
drop = re.compile(r'^\s*(uniform\s+(COMPAT_PRECISION\s+)?\w+\s+(FrameDirection|FrameCount|OutputSize|TextureSize|InputSize|OrigInputSize|SourceSize|OutSize|Texture|Source|OrigTexture|Prev\w*)\s*;|(COMPAT_VARYING|varying)\s+.*;|out\s+vec4\s+FragColor\s*;)')
lines = [l for l in frag.splitlines() if not drop.match(l)]
frag = '\n'.join(lines)
pre = '''// %s: libretro's %s (github.com/libretro/glsl-shaders), as a BananaOS shader (SuperDrastic and BananaDS)
// for a DS screen drawn to the panel. Converted by tools/shaders/conv.py: its fragment stage, libretro's names
// mapped onto u_texture, u_texture_size, u_output_size and v_texcoord; its parameters at their defaults.
// The original's top (its notice and parameters):
%s
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
varying vec2 v_texcoord;
uniform sampler2D u_texture;
uniform vec2 u_texture_size;
uniform vec2 u_output_size;
#define Texture u_texture
#define TextureSize u_texture_size
#define InputSize u_texture_size
#define OrigInputSize u_texture_size
#define OutputSize u_output_size
#define FrameCount 0
#define FrameDirection 1
#define TEX0 vec4(v_texcoord, 0.0, 0.0)
%s
#define main dsf_main
''' % (title, src.split('glslsh/')[-1], head.strip(), '\n'.join(extra))
post = '''
#undef main
void main()
{
    dsf_main();
    gl_FragColor = SWIZ(gl_FragColor);
}
'''
# the version and precision lines in the fragment section are fine (ES 1.00 skips the >= 130 branch)
full = pre + frag + post
full = re.sub(r'^\s*#version.*$', '', full, flags=re.M)
open(out, 'w').write(full)
