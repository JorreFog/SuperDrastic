// drastic-zfast-lcd: DraStic's zfast_lcd (Android DraStic, as bundled by Anbernic), as a BananaOS shader (SuperDrastic and BananaDS) for a DS screen drawn to the panel.
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

void dsf_vert()
{
	v_texture_coordinate = v_texcoord;

}

// Configuration: BORDERMULT "Border Multiplier" 14.0 -40.0 40.0 1.0
#define BORDERMULT 14.0

#ifndef BORDERMULT
#define BORDERMULT 14.0
#endif

const float NDS_SCREEN_HEIGHT = 192.0;

void dsf_frag()
{
	// Generate grid pattern (in native NDS coordinate space)
	// - Have to divide by the current scale factor...
	//   There can only be two values here:
	//   > 'High-Resolution 3D Rendering' OFF - scale factor == 1
	//   > 'High-Resolution 3D Rendering' ON  - scale factor == 2
	//   But it's easiest to assume the general case...
	//    scale_factor == texture_height / NDS_SCREEN_HEIGHT
	vec2 texcoordInPixels = v_texture_coordinate.xy * DSF_TS.zw * (NDS_SCREEN_HEIGHT * DSF_TS.y);
	vec2 centerCoord = floor(texcoordInPixels.xy) + vec2(0.5, 0.5);
	vec2 distFromCenter = abs(centerCoord - texcoordInPixels);
	
	float Y = max(distFromCenter.x, distFromCenter.y);
	
	Y = Y * Y;
	float YY = Y * Y;
	float YYY = YY * Y;
	
	float LineWeight = YY - 2.7 * YYY;
	LineWeight = 1.0 - BORDERMULT * LineWeight;
	
	// Get colour sample and apply grid effect
	vec3 colour = texture2D(u_texture, v_texture_coordinate.xy).rgb * LineWeight;
	
	gl_FragColor = vec4(colour.rgb, 1.0);
}

void main()
{
    dsf_vert();
    dsf_frag();
    gl_FragColor = SWIZ(gl_FragColor);
}
