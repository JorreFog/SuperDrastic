// sharp-bilinear-scanlines: libretro's interpolation/shaders/sharp-bilinear-scanlines.glsl (github.com/libretro/glsl-shaders), as a BananaOS shader (SuperDrastic and BananaDS)
// for a DS screen drawn to the panel. Converted by tools/shaders/conv.py (SuperDrastic) (SuperDrastic): its fragment stage, libretro's names
// mapped onto u_texture, u_texture_size, u_output_size and v_texcoord; its parameters at their defaults.
// The original's top (its notice and parameters):
/*
   Author: rsn8887 (based on TheMaister)
   License: Public domain

   This is an integer prescale filter that should be combined
   with a bilinear hardware filtering (GL_BILINEAR filter or some such) to achieve
   a smooth scaling result with minimum blur. This is good for pixelgraphics
   that are scaled by non-integer factors.
   
   The prescale factor and texel coordinates are precalculated
   in the vertex shader for speed.
*/

// Parameter lines go here:
#pragma parameter SCANLINE_BASE_BRIGHTNESS "Scanline Base Brightness" 0.60 0.0 1.0 0.01
#pragma parameter SCANLINE_HORIZONTAL_MODULATION "Scanline Horizontal Modulation" 0.0 0.0 2.00 0.01
#pragma parameter SCANLINE_VERTICAL_MODULATION "Scanline Vertical Modulation" 0.75 0.0 2.0 0.01

#define pi 3.141592654
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
#define COL0 vec4(1.0)
#define omega (vec2(3.141592654 * u_output_size.x, 2.0 * 3.141592654 * u_texture_size.y))
#define precalc_texel (v_texcoord * u_texture_size)
#define precalc_scale (max(floor(u_output_size / u_texture_size), vec2(1.0, 1.0)))
#define main dsf_main


#if __VERSION__ >= 130
#define COMPAT_VARYING in
#define COMPAT_TEXTURE texture
#else
#define COMPAT_VARYING varying
#define FragColor gl_FragColor
#define COMPAT_TEXTURE texture2D
#endif

#ifdef GL_ES
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
#define COMPAT_PRECISION mediump
#else
#define COMPAT_PRECISION
#endif


// fragment compatibility #defines
#define Source Texture
#define vTexCoord TEX0.xy

#define SourceSize vec4(TextureSize, 1.0 / TextureSize) //either TextureSize or InputSize
#define outsize vec4(OutputSize, 1.0 / OutputSize)

#ifdef PARAMETER_UNIFORM
// All parameter floats need to have COMPAT_PRECISION in front of them
uniform COMPAT_PRECISION float SCANLINE_BASE_BRIGHTNESS;
uniform COMPAT_PRECISION float SCANLINE_HORIZONTAL_MODULATION;
uniform COMPAT_PRECISION float SCANLINE_VERTICAL_MODULATION;
#else
#define SCANLINE_BASE_BRIGHTNESS 0.60
#define SCANLINE_HORIZONTAL_MODULATION 0.0
#define SCANLINE_VERTICAL_MODULATION 0.75
#endif


void main()
{
   vec2 texel = precalc_texel;
   vec2 scale = precalc_scale;

   vec2 texel_floored = floor(texel);
   vec2 s = fract(texel);
   vec2 region_range = 0.5 - 0.5 / scale;

   // Figure out where in the texel to sample to get correct pre-scaled bilinear.
   // Uses the hardware bilinear interpolator to avoid having to sample 4 times manually.

   vec2 center_dist = s - 0.5;
   vec2 f = (center_dist - clamp(center_dist, -region_range, region_range)) * scale + 0.5;

   vec2 mod_texel = texel_floored + f;

   vec3 res = COMPAT_TEXTURE(Source, mod_texel / SourceSize.xy).xyz;

   // thick scanlines (thickness pre-calculated in vertex shader based on source resolution)
   vec2 sine_comp = vec2(SCANLINE_HORIZONTAL_MODULATION, SCANLINE_VERTICAL_MODULATION);

   vec3 scanline = res * (SCANLINE_BASE_BRIGHTNESS + dot(sine_comp * sin(vTexCoord * omega), vec2(1.0, 1.0)));
   FragColor = vec4(scanline.rgb, 1.0);
} 
#undef main
void main()
{
    dsf_main();
    gl_FragColor = SWIZ(gl_FragColor);
}
