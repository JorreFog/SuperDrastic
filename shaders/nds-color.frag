// nds-color: libretro's handheld/shaders/color/nds-color.glsl (github.com/libretro/glsl-shaders), as a BananaOS shader (SuperDrastic and BananaDS)
// for a DS screen drawn to the panel. Converted by tools/shaders/conv.py (SuperDrastic) (SuperDrastic): its fragment stage, libretro's names
// mapped onto u_texture, u_texture_size, u_output_size and v_texcoord; its parameters at their defaults.
// The original's top (its notice and parameters):
/*
   Shader Modified: Pokefan531
   Color Mangler
   Author: hunterk
   License: Public domain
*/
// Shader that replicates the LCD dynamics from a Nintendo DS Phat

// Compatibility #ifdefs needed for parameters
#ifdef GL_ES
#define COMPAT_PRECISION mediump
#else
#define COMPAT_PRECISION
#endif

#define target_gamma 1.91
#define display_gamma 1.91
#define sat 1.0
#define lum 0.89
#define contrast 1.0
#define blr 0.0
#define blg 0.0
#define blb 0.0
#define r 0.87
#define g 0.645
#define b 0.73
#define rg 0.10
#define rb 0.10
#define gr 0.255
#define gb 0.17
#define br -0.125
#define bg 0.255
#define overscan_percent_x 0.0
#define overscan_percent_y 0.0
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
#define texture(c, d) COMPAT_TEXTURE(c, d)
#define SourceSize vec4(TextureSize, 1.0 / TextureSize) //either TextureSize or InputSize
#define outsize vec4(OutputSize, 1.0 / OutputSize)

void main()
{
   vec4 screen = pow(texture(Source, vTexCoord), vec4(target_gamma)).rgba;
   vec4 avglum = vec4(0.5);
   screen = mix(screen, avglum, (1.0 - contrast));
   
 //				r   g    b   black
mat4 color = mat4(r,  rg,  rb, 0.0,  //red channel
			   gr,  g,   gb, 0.0,  //green channel
			   br,  bg,  b,  0.0,  //blue channel
			  blr, blg, blb,    0.0); //alpha channel; these numbers do nothing for our purposes.
			  
mat4 adjust = mat4((1.0 - sat) * 0.3086 + sat, (1.0 - sat) * 0.3086, (1.0 - sat) * 0.3086, 1.0,
(1.0 - sat) * 0.6094, (1.0 - sat) * 0.6094 + sat, (1.0 - sat) * 0.6094, 1.0,
(1.0 - sat) * 0.0820, (1.0 - sat) * 0.0820, (1.0 - sat) * 0.0820 + sat, 1.0,
0.0, 0.0, 0.0, 1.0);
	color *= adjust;
	screen = clamp(screen * lum, 0.0, 1.0);
	screen = color * screen;
	FragColor = pow(screen, vec4(1.0 / display_gamma));
} 
#undef main
void main()
{
    dsf_main();
    gl_FragColor = SWIZ(gl_FragColor);
}
