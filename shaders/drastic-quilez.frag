// drastic-quilez: DraStic's Quilez (Android DraStic, as bundled by Anbernic), as a BananaOS shader (SuperDrastic and BananaDS) for a DS screen drawn to the panel.
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

vec2 v_texture_coordinate;
void dsf_vert() {

  v_texture_coordinate = v_texcoord;
}

void dsf_frag(){
  vec2 s_tex = DSF_TS.zw;
  vec2 p = v_texture_coordinate.xy * s_tex + vec2(0.5);
  vec2 i = floor(p);
  vec2 f = p - i;
  f = f * f * f * (f * (f * 6.0 - vec2(15.0)) + vec2(10.0));
  p = i + f;
  p = (p - vec2(0.5)) / s_tex;
  vec4 color = texture2D(u_texture, p);
  gl_FragColor = vec4(color.rgb, 1.0);
}

void main()
{
    dsf_vert();
    dsf_frag();
    gl_FragColor = SWIZ(gl_FragColor);
}
