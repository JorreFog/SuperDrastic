// retro-v2: libretro's handheld/shaders/retro-v2.glsl (github.com/libretro/glsl-shaders), as a BananaOS shader (SuperDrastic and BananaDS)
// for a DS screen drawn to the panel. Converted by tools/shaders/conv.py (SuperDrastic) (SuperDrastic): its fragment stage, libretro's names
// mapped onto u_texture, u_texture_size, u_output_size and v_texcoord; its parameters at their defaults.
// The original's top (its notice and parameters):
/*
   Hyllian - Retro Shader - 2013

   A re-implementation from the original made by Hyllian and DOLLS!
   
   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

// Parameter lines go here:
#pragma parameter RETRO_PIXEL_SIZE "Retro Pixel Size" 0.84 0.0 1.0 0.01
// ^^This value must be between 0.0 (totally black) and 1.0 (nearest neighbor)
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


// fragment compatibility #defines
#define Source Texture
#define vTexCoord TEX0.xy

#define SourceSize vec4(TextureSize, 1.0 / TextureSize) //either TextureSize or InputSize
#define outsize vec4(OutputSize, 1.0 / OutputSize)

#ifdef PARAMETER_UNIFORM
uniform COMPAT_PRECISION float RETRO_PIXEL_SIZE;
#else
#define RETRO_PIXEL_SIZE 0.84
#endif

void main()
{
    // Reading the texel
    vec3 E = pow(COMPAT_TEXTURE(Source, vTexCoord).xyz, vec3(2.4));

    vec2 fp = fract(vTexCoord * SourceSize.xy);
    vec2 ps = InputSize.xy * outsize.zw;
    if (any(notEqual(InputSize, OrigInputSize)))
    {
        fp = fract(vTexCoord * TextureSize.xy / InputSize.xy * OrigInputSize.xy);
        ps = OrigInputSize.xy * outsize.zw;
    }

    vec2 f = clamp(clamp(fp + 0.5*ps, 0.0, 1.0) - RETRO_PIXEL_SIZE, vec2(0.0), ps)/ps;

    float max_coord =  max(f.x, f.y);

    vec3 res = mix(E*(1.04+fp.x*fp.y), E*0.36, max_coord);

    // Product interpolation
    FragColor = vec4(clamp( pow(res, vec3(1.0 / 2.2)), 0.0, 1.0 ), 1.0);
} 
#undef main
void main()
{
    dsf_main();
    gl_FragColor = SWIZ(gl_FragColor);
}
