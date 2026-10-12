// zfast-lcd: libretro's handheld/shaders/zfast_lcd.glsl (github.com/libretro/glsl-shaders), as a BananaOS shader (SuperDrastic and BananaDS)
// for a DS screen drawn to the panel. Converted by tools/shaders/conv.py (SuperDrastic) (SuperDrastic): its fragment stage, libretro's names
// mapped onto u_texture, u_texture_size, u_output_size and v_texcoord; its parameters at their defaults.
// The original's top (its notice and parameters):
/*
    zfast_lcd_standard - A very simple LCD shader meant to be used at 1080p
		on the raspberry pi 3.
		
    Copyright (C) 2017 Greg Hogan (SoltanGris42)

    This program is free software; you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by the Free
    Software Foundation; either version 2 of the License, or (at your option)
    any later version.


Notes:  This shader just does nearest neighbor scaling of the game and then
		darkens the border pixels to imitate an LCD screen. You can change the
		amount of darkening and the thickness of the borders.  You can also 
		do basic gamma adjustment.
		
*/

//For testing compilation 
//#define FRAGMENT
//#define VERTEX
//#define GBAGAMMA 

//Some drivers don't return black with texture coordinates out of bounds
//SNES Classic is too slow to black these areas out when using fullscreen
//overlays.  But you can uncomment the below to black them out if necessary

#define BLACK_OUT_BORDER

#pragma parameter BORDERMULT "Border Multiplier" 14.0 -40.0 40.0 1.0
#pragma parameter GBAGAMMA "GBA Gamma Hack" 1.0 0.0 1.0 1.0
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
#define invSize (1.0 / u_texture_size)
#define main dsf_main


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
#define SourceSize vec4(TextureSize, 1.0 / TextureSize) //either TextureSize or InputSize
#define OutSize vec4(OutputSize, 1.0 / OutputSize)

#ifdef PARAMETER_UNIFORM
uniform COMPAT_PRECISION float BORDERMULT;
uniform COMPAT_PRECISION float GBAGAMMA;
#else
#define BORDERMULT 14.0
#define GBAGAMMA 1.0
#endif

void main()
{
	COMPAT_PRECISION vec2 texcoordInPixels = TEX0.xy * TextureSize.xy / InputSize.xy * OrigInputSize.xy;
	COMPAT_PRECISION vec2 centerCoord = floor(texcoordInPixels.xy)+vec2(0.5,0.5);
	COMPAT_PRECISION vec2 sourceCoordInPixels = TEX0.xy * TextureSize.xy;
	COMPAT_PRECISION vec2 sourceCenterCoord = floor(sourceCoordInPixels.xy)+vec2(0.5,0.5);
	COMPAT_PRECISION vec2 distFromCenter = abs(centerCoord - texcoordInPixels);

	COMPAT_PRECISION float Y = max(distFromCenter.x,(distFromCenter.y));

	Y=Y*Y;
	COMPAT_PRECISION float YY = Y*Y;
	COMPAT_PRECISION float YYY = YY*Y;

	COMPAT_PRECISION float LineWeight = YY - 2.7*YYY;
	LineWeight = 1.0 - BORDERMULT*LineWeight;

	COMPAT_PRECISION vec3 colour = COMPAT_TEXTURE(Texture, invSize*sourceCenterCoord).rgb*LineWeight;

//#if defined(GBAGAMMA)
//	//colour.rgb = pow(colour.rgb, vec3(1.35));
//	colour.rgb*=0.6+0.4*(colour.rgb); //fake gamma because the pi is too slow!
//#endif
	if (GBAGAMMA > 0.5)
		colour.rgb*=0.6+0.4*(colour.rgb); //fake gamma because the pi is too slow!
		
	FragColor = vec4(colour.rgb , 1.0);
}
#undef main
void main()
{
    dsf_main();
    gl_FragColor = SWIZ(gl_FragColor);
}
