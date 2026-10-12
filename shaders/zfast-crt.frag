// zfast-crt: libretro's crt/shaders/zfast_crt.glsl (github.com/libretro/glsl-shaders), as a BananaOS shader (SuperDrastic and BananaDS)
// for a DS screen drawn to the panel. Converted by tools/shaders/conv.py (SuperDrastic) (SuperDrastic): its fragment stage, libretro's names
// mapped onto u_texture, u_texture_size, u_output_size and v_texcoord; its parameters at their defaults.
// The original's top (its notice and parameters):
/*
    zfast_crt_standard - A simple, fast CRT shader.

    Copyright (C) 2017 Greg Hogan (SoltanGris42)

    This program is free software; you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by the Free
    Software Foundation; either version 2 of the License, or (at your option)
    any later version.


Notes:  This shader does scaling with a weighted linear filter for adjustable
	sharpness on the x and y axes based on the algorithm by Inigo Quilez here:
	http://http://www.iquilezles.org/www/articles/texture/texture.htm
	but modified to be somewhat sharper.  Then a scanline effect that varies
	based on pixel brighness is applied along with a monochrome aperture mask.
	This shader runs at 60fps on the Raspberry Pi 3 hardware at 2mpix/s
	resolutions (1920x1080 or 1600x1200).
*/

//For testing compilation 
//#define FRAGMENT
//#define VERTEX

//This can't be an option without slowing the shader down
//Comment this out for a coarser 3 pixel mask...which is currently broken
//on SNES Classic Edition due to Mali 400 gpu precision
#define FINEMASK
//Some drivers don't return black with texture coordinates out of bounds
//SNES Classic is too slow to black these areas out when using fullscreen
//overlays.  But you can uncomment the below to black them out if necessary
//#define BLACK_OUT_BORDER

// Parameter lines go here:
#pragma parameter BLURSCALEX "Blur Amount X-Axis" 0.30 0.0 1.0 0.05
#pragma parameter LOWLUMSCAN "Scanline Darkness - Low" 6.0 0.0 10.0 0.5
#pragma parameter HILUMSCAN "Scanline Darkness - High" 8.0 0.0 50.0 1.0
#pragma parameter BRIGHTBOOST "Dark Pixel Brightness Boost" 1.25 0.5 1.5 0.05
#pragma parameter MASK_DARK "Mask Effect Amount" 0.25 0.0 1.0 0.05
#pragma parameter MASK_FADE "Mask/Scanline Fade" 0.8 0.0 1.0 0.05
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
#define maskFade (0.3333 * MASK_FADE)
#define invDims (1.0 / u_texture_size)
#define main dsf_main


#ifdef GL_ES
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
#ifdef GL_FRAGMENT_PRECISION_HIGH
#define COMPAT_PRECISION highp
#else 
#define COMPAT_PRECISION mediump
#endif
#else
#define COMPAT_PRECISION
#endif

#if __VERSION__ >= 130
#define COMPAT_VARYING in
#define COMPAT_TEXTURE texture
out COMPAT_PRECISION vec4 FragColor;
#else
#define COMPAT_VARYING varying
#define FragColor gl_FragColor
#define COMPAT_TEXTURE texture2D
#endif


// compatibility #defines
#define Source Texture
#define vTexCoord TEX0.xy
#define texture(c, d) COMPAT_TEXTURE(c, d)
#define SourceSize vec4(TextureSize, 1.0 / TextureSize) //either TextureSize or InputSize
#define OutSize vec4(OutputSize, 1.0 / OutputSize)

#ifdef PARAMETER_UNIFORM
// All parameter floats need to have COMPAT_PRECISION in front of them
uniform COMPAT_PRECISION float BLURSCALEX;
//uniform COMPAT_PRECISION float BLURSCALEY;
uniform COMPAT_PRECISION float LOWLUMSCAN;
uniform COMPAT_PRECISION float HILUMSCAN;
uniform COMPAT_PRECISION float BRIGHTBOOST;
uniform COMPAT_PRECISION float MASK_DARK;
uniform COMPAT_PRECISION float MASK_FADE;
#else
#define BLURSCALEX 0.45
//#define BLURSCALEY 0.20
#define LOWLUMSCAN 5.0
#define HILUMSCAN 10.0
#define BRIGHTBOOST 1.25
#define MASK_DARK 0.25
#define MASK_FADE 0.8
#endif

void main()
{

	//This is just like "Quilez Scaling" but sharper
	COMPAT_PRECISION vec2 p = vTexCoord * TextureSize;
	COMPAT_PRECISION vec2 i = floor(p) + 0.50;
	COMPAT_PRECISION vec2 f = p - i;
	p = (i + 4.0*f*f*f)*invDims;
	p.x = mix( p.x , vTexCoord.x, BLURSCALEX);
	COMPAT_PRECISION float Y = f.y*f.y;
	COMPAT_PRECISION float YY = Y*Y;
#define ratio SourceSize.x/InputSize.x	
#if defined(FINEMASK) 
	COMPAT_PRECISION float whichmask = floor(vTexCoord.x*OutputSize.x*ratio)*-0.5;
	COMPAT_PRECISION float mask = 1.0 + float(fract(whichmask) < 0.5) * -MASK_DARK;
#else
	COMPAT_PRECISION float whichmask = floor(vTexCoord.x*OutputSize.x*ratio)*-0.3333;
	COMPAT_PRECISION float mask = 1.0 + float(fract(whichmask) < 0.3333) * -MASK_DARK;
#endif
	COMPAT_PRECISION vec3 colour = COMPAT_TEXTURE(Source, p).rgb;
	
	COMPAT_PRECISION float scanLineWeight = (BRIGHTBOOST - LOWLUMSCAN*(Y - 2.05*YY));
	COMPAT_PRECISION float scanLineWeightB = 1.0 - HILUMSCAN*(YY-2.8*YY*Y);	
	
#if defined(BLACK_OUT_BORDER)
	colour.rgb*=float(tc.x > 0.0)*float(tc.y > 0.0); //why doesn't the driver do the right thing?
#endif

	FragColor.rgba = vec4(colour.rgb*mix(scanLineWeight*mask, scanLineWeightB, dot(colour.rgb,vec3(maskFade))),1.0);
	
} 
#undef main
void main()
{
    dsf_main();
    gl_FragColor = SWIZ(gl_FragColor);
}
