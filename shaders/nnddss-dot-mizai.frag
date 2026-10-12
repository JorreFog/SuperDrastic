// nnddss-dot-mizai: Anbernic NNDDSS 1.0.2's Dot_MiZai, as a BananaOS shader (SuperDrastic and BananaDS) for a DS screen drawn to the panel.
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

// screen resolution scale (2.0 for current High-Resolution 3D Rendering)
#define SCREEN_RES_SCALE (DSF_TS.x / 256.0)
// dot softness in texels; 0 enables the fast path (single texel fetch)
#define SOFT 0

#ifndef SCREEN_RES_SCALE
#define SCREEN_RES_SCALE (DSF_TS.x / 256.0)
#endif

#ifndef SOFT
#define SOFT 0
#endif

vec2 v_texture_coordinate;
#if SOFT != 0
vec4 v_texture_coordinate_1;
vec4 v_texture_coordinate_2;
vec4 v_texture_coordinate_3;
vec4 v_texture_coordinate_4;
#endif

vec2 pixel_no;

vec4 v_video_size;

#define screen_res_scale SCREEN_RES_SCALE

void dsf_vert()
{
  v_video_size = vec4(DSF_TS.xy * screen_res_scale, DSF_TS.zw / screen_res_scale);

  v_texture_coordinate = v_texcoord;
#if SOFT != 0
  vec2 dx = vec2(v_video_size.x, 0.0);
  vec2 dy = vec2(0.0, v_video_size.y);
  float soft = float(SOFT);
  v_texture_coordinate_1.xy = v_texture_coordinate.xy - dx * soft - dy * soft;
  v_texture_coordinate_1.zw = v_texture_coordinate.xy - dy * soft;
  v_texture_coordinate_2.xy = v_texture_coordinate.xy + dx * soft - dy * soft;
  v_texture_coordinate_2.zw = v_texture_coordinate.xy - dx * soft;
  v_texture_coordinate_3.xy = v_texture_coordinate.xy + dx * soft;
  v_texture_coordinate_3.zw = v_texture_coordinate.xy - dx * soft + dy * soft;
  v_texture_coordinate_4.xy = v_texture_coordinate.xy + dy * soft;
  v_texture_coordinate_4.zw = v_texture_coordinate.xy + dx * soft + dy * soft;
#endif
  pixel_no = v_texture_coordinate * v_video_size.zw;
}

// dot softness in texels; keep in sync with <vheader>
#define SOFT 0

#ifndef SOFT
#define SOFT 0
#endif

#if SOFT != 0

#endif

const float gamma = 2.4;
const float shine = 0.05;
const float blend = 0.65;

float dist(vec2 coord, vec2 source)
{
  vec2 delta = coord - source;
  return sqrt(dot(delta, delta));
}

float color_bloom(vec3 color)
{
  const vec3 gray_coeff = vec3(0.30, 0.59, 0.11);
  float bright = dot(color, gray_coeff);
  return mix(1.0 + shine, 1.0 - shine, bright);
}

vec3 lookup(vec2 pixel_no, float offset_x, float offset_y, vec3 color)
{
  vec2 offset = vec2(offset_x, offset_y);
  float delta = dist(fract(pixel_no), offset + vec2(0.5, 0.5));
  return color * exp(-gamma * delta * color_bloom(color));
}

void dsf_frag()
{
#if SOFT == 0
  vec3 texel = texture2D(u_texture, v_texture_coordinate).rgb;
  vec3 c00 = texel;
  vec3 c10 = texel;
  vec3 c20 = texel;
  vec3 c01 = texel;
  vec3 c11 = texel;
  vec3 c21 = texel;
  vec3 c02 = texel;
  vec3 c12 = texel;
  vec3 c22 = texel;
#else
  vec3 c00 = texture2D(u_texture, v_texture_coordinate_1.xy).rgb;
  vec3 c10 = texture2D(u_texture, v_texture_coordinate_1.zw).rgb;
  vec3 c20 = texture2D(u_texture, v_texture_coordinate_2.xy).rgb;
  vec3 c01 = texture2D(u_texture, v_texture_coordinate_2.zw).rgb;
  vec3 c11 = texture2D(u_texture, v_texture_coordinate.xy).rgb;
  vec3 c21 = texture2D(u_texture, v_texture_coordinate_3.xy).rgb;
  vec3 c02 = texture2D(u_texture, v_texture_coordinate_3.zw).rgb;
  vec3 c12 = texture2D(u_texture, v_texture_coordinate_4.xy).rgb;
  vec3 c22 = texture2D(u_texture, v_texture_coordinate_4.zw).rgb;
#endif
  
  vec3 mid_color = lookup(pixel_no, 0.0, 0.0, c11);
  vec3 color = vec3(0.0, 0.0, 0.0);
  color += lookup(pixel_no, -1.0, -1.0, c00);
  color += lookup(pixel_no,  0.0, -1.0, c10);
  color += lookup(pixel_no,  1.0, -1.0, c20);
  color += lookup(pixel_no, -1.0,  0.0, c01);
  color += mid_color;
  color += lookup(pixel_no,  1.0,  0.0, c21);
  color += lookup(pixel_no, -1.0,  1.0, c02);
  color += lookup(pixel_no,  0.0,  1.0, c12);
  color += lookup(pixel_no,  1.0,  1.0, c22);
  gl_FragColor.rgb = mix(1.2 * mid_color, color, blend);
  gl_FragColor.a = 1.0;
}

void main()
{
    dsf_vert();
    dsf_frag();
    gl_FragColor = SWIZ(gl_FragColor);
}
