// bevel: libretro's handheld/shaders/bevel.glsl (github.com/libretro/glsl-shaders), as a BananaOS shader (SuperDrastic and BananaDS)
// for a DS screen drawn to the panel. Converted by tools/shaders/conv.py (SuperDrastic) (SuperDrastic): its fragment stage, libretro's names
// mapped onto u_texture, u_texture_size, u_output_size and v_texcoord; its parameters at their defaults.
// The original's top (its notice and parameters):
/*
   Hyllian's Bevel Shader
  
   Copyright (C) 2011-2014 Hyllian/Jararaca - sergiogdb@gmail.com

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
#pragma parameter BEVEL_LEVEL "Bevel Level" 0.2 0.0 0.5 0.01
#pragma parameter InputGamma "Input Gamma" 2.4 0.1 5.0 0.1
#pragma parameter OutputGamma "Output Gamma" 2.2 0.1 5.0 0.1
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
uniform COMPAT_PRECISION float BEVEL_LEVEL;
uniform COMPAT_PRECISION float InputGamma;
uniform COMPAT_PRECISION float OutputGamma;
#else
#define BEVEL_LEVEL 0.2
#define InputGamma 2.4
#define OutputGamma 2.2
#endif

#define GAMMA_IN(color)     pow(color, vec3(InputGamma, InputGamma, InputGamma))
#define GAMMA_OUT(color)    pow(color, vec3(1.0 / OutputGamma, 1.0 / OutputGamma, 1.0 / OutputGamma))

vec3 bevel(vec2 pos, vec3 color)
{
    vec3 weight;

    float r = sqrt(dot(pos, vec2(1.0)));

    vec3 delta = mix(vec3(BEVEL_LEVEL), vec3(1.0-BEVEL_LEVEL), color);

    weight = delta*(1.-r);

    return color+weight;
}

void main()
{
    vec2 position = fract(vTexCoord * SourceSize.xy);
    if (any(notEqual(InputSize, OrigInputSize)))
        position = fract(vTexCoord * TextureSize.xy / InputSize.xy * OrigInputSize.xy);

    vec3 color = GAMMA_IN(COMPAT_TEXTURE(Source, vTexCoord).rgb);

    color = clamp(bevel(position, color), 0.0, 1.0);

   FragColor = vec4(GAMMA_OUT(color), 1.0);
} 
#undef main
void main()
{
    dsf_main();
    gl_FragColor = SWIZ(gl_FragColor);
}
