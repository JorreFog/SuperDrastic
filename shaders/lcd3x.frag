// lcd3x: libretro's handheld/shaders/lcd3x.glsl (github.com/libretro/glsl-shaders), as a BananaOS shader (SuperDrastic and BananaDS)
// for a DS screen drawn to the panel. Converted by tools/shaders/conv.py (SuperDrastic) (SuperDrastic): its fragment stage, libretro's names
// mapped onto u_texture, u_texture_size, u_output_size and v_texcoord; its parameters at their defaults.
// The original's top (its notice and parameters):
/*
   Author: Gigaherz
   License: Public domain
*/

// Parameter lines go here:
#pragma parameter brighten_scanlines "Brighten Scanlines" 16.0 1.0 32.0 0.5
#pragma parameter brighten_lcd "Brighten LCD" 4.0 1.0 12.0 0.1
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


// compatibility #defines
#define Source Texture
#define vTexCoord TEX0.xy

#define outsize vec4(OutputSize, 1.0 / OutputSize)

#ifdef PARAMETER_UNIFORM
// All parameter floats need to have COMPAT_PRECISION in front of them
uniform COMPAT_PRECISION float brighten_scanlines;
uniform COMPAT_PRECISION float brighten_lcd;
#else
#define brighten_scanlines 16.0
#define brighten_lcd 4.0
#endif

const vec3 offsets = vec3(3.141592654) * vec3(1.0/2.0,1.0/2.0 - 2.0/3.0,1.0/2.0-4.0/3.0);

void main()
{
    vec2 omega = vec2(3.141592654) * vec2(2.0) * OrigInputSize;
    vec3 res = COMPAT_TEXTURE(Source, vTexCoord).xyz;

    vec2 angle = vTexCoord * TextureSize / InputSize * omega;
	
    float yfactor = (brighten_scanlines + sin(angle.y)) / (brighten_scanlines + 1.0);
    vec3 xfactors = (brighten_lcd + sin(angle.x + offsets)) / (brighten_lcd + 1.0);
   
    vec3 color = yfactor * xfactors * res;
   
   FragColor = vec4(color.x, color.y, color.z, 1.0);
} 
#undef main
void main()
{
    dsf_main();
    gl_FragColor = SWIZ(gl_FragColor);
}
