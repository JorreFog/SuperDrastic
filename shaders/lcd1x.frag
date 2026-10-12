// lcd1x: libretro's handheld/shaders/lcd1x.glsl (github.com/libretro/glsl-shaders), as a BananaOS shader (SuperDrastic and BananaDS)
// for a DS screen drawn to the panel. Converted by tools/shaders/conv.py (SuperDrastic) (SuperDrastic): its fragment stage, libretro's names
// mapped onto u_texture, u_texture_size, u_output_size and v_texcoord; its parameters at their defaults.
// The original's top (its notice and parameters):
/*
   lcd1x shader

   A slightly tweaked version of lcd3x, original code written by Gigaherz
   and released into the public domain:

   > Omits LCD 'colour seperation' effect

   > Has 'properly' aligned scanlines

   Edited by jdgleaver

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the Free
   Software Foundation; either version 2 of the License, or (at your option)
   any later version.
*/

#pragma parameter BRIGHTEN_SCANLINES "Brighten Scanlines" 16.0 1.0 32.0 0.5
#pragma parameter BRIGHTEN_LCD "Brighten LCD" 4.0 1.0 12.0 0.1
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
precision highp float;
precision highp int;
#define COMPAT_PRECISION highp
#else
#define COMPAT_PRECISION
#endif


#ifdef PARAMETER_UNIFORM
// All parameter floats need to have COMPAT_PRECISION in front of them
uniform COMPAT_PRECISION float BRIGHTEN_SCANLINES;
uniform COMPAT_PRECISION float BRIGHTEN_LCD;
#else
#define BRIGHTEN_SCANLINES 16.0
#define BRIGHTEN_LCD 4.0
#endif

// Magic Numbers
#define PI 3.141592654

/*
   FRAGMENT SHADER
*/
void main()
{
   // Generate LCD grid effect
   // > Note the 0.25 pixel offset -> required to ensure that
   //   scanlines occur *between* pixels
   COMPAT_PRECISION vec2 angle =
      2.0 * PI * ((TEX0.xy * OrigInputSize * TextureSize / InputSize) - 0.25);

   COMPAT_PRECISION float yfactor = (BRIGHTEN_SCANLINES + sin(angle.y)) / (BRIGHTEN_SCANLINES + 1.0);
   COMPAT_PRECISION float xfactor = (BRIGHTEN_LCD + sin(angle.x)) / (BRIGHTEN_LCD + 1.0);

   // Get colour sample
   COMPAT_PRECISION vec3 colour = COMPAT_TEXTURE(Texture, TEX0.xy).rgb;

   // Apply LCD grid effect
   colour.rgb = yfactor * xfactor * colour.rgb;

   FragColor = vec4(colour.rgb, 1.0);
} 
#undef main
void main()
{
    dsf_main();
    gl_FragColor = SWIZ(gl_FragColor);
}
