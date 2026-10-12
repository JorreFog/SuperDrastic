// drastic-lcd1x: DraStic's lcd1x (Android DraStic, as bundled by Anbernic), as a BananaOS shader (SuperDrastic and BananaDS) for a DS screen drawn to the panel.
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

// Configuration
#define BRIGHTEN_SCANLINES 16.0 // "Brighten Scanlines" - default: 16.0, min: 1.0, max: 32.0, step: 0.5
										  // (brightness of horizontal lines)
#define BRIGHTEN_LCD 4.0        // "Brighten LCD"       - default:  4.0, min: 1.0, max: 12.0, step: 0.1
										  // (brightness of vertical lines)

#ifndef BRIGHTEN_SCANLINES
#define BRIGHTEN_SCANLINES 16.0
#endif
#ifndef BRIGHTEN_LCD
#define BRIGHTEN_LCD 4.0
#endif

#define PI 3.141592654

const float NDS_SCREEN_HEIGHT = 192.0;
const float INV_BRIGHTEN_SCANLINES_INC = 1.0 / (BRIGHTEN_SCANLINES + 1.0);
const float INV_BRIGHTEN_LCD_INC = 1.0 / (BRIGHTEN_LCD + 1.0);

void dsf_frag()
{
	// Note to self: uniform vec4 DSF_TS(1.0/w, 1.0/h, w, h)
	
	// Generate LCD grid effect
	// > Note the 0.25 pixel offset -> required to ensure that
	//   scanlines occur *between* pixels
	// > Divide pixel coordinate by current scale factor (texture_height / NDS_SCREEN_HEIGHT)
	vec2 angle = 2.0 * PI * (((v_texture_coordinate.xy * DSF_TS.zw) * NDS_SCREEN_HEIGHT * DSF_TS.y) - 0.25);
	
	float yfactor = (BRIGHTEN_SCANLINES + sin(angle.y)) * INV_BRIGHTEN_SCANLINES_INC;
	float xfactor = (BRIGHTEN_LCD + sin(angle.x)) * INV_BRIGHTEN_LCD_INC;
	
	// Get colour sample
	vec3 colour = texture2D(u_texture, v_texture_coordinate.xy).rgb;
	
	// Apply LCD grid effect
	colour.rgb = yfactor * xfactor * colour.rgb;
	
	gl_FragColor = vec4(colour.rgb, 1.0);
}

void main()
{
    dsf_vert();
    dsf_frag();
    gl_FragColor = SWIZ(gl_FragColor);
}
