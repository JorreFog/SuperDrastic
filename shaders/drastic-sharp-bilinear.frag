// drastic-sharp-bilinear: DraStic's sharp_bilinear (Android DraStic, as bundled by Anbernic), as a BananaOS shader (SuperDrastic and BananaDS) for a DS screen drawn to the panel.
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

vec2 precalc_texel;
vec2 precalc_scale;

void dsf_vert()
{

	
	precalc_texel = v_texcoord * DSF_TS.zw;
	precalc_scale = floor(u_output_size.xy / DSF_TS.zw) + 1.0;
}

void dsf_frag()
{
	vec2 texel_floored = floor(precalc_texel);
	vec2 s = fract(precalc_texel);
	vec2 region_range = 0.5 - 0.5 / precalc_scale;
	
	// Figure out where in the texel to sample to get correct pre-scaled bilinear.
	// Uses the hardware bilinear interpolator to avoid having to sample 4 times manually.
	
	vec2 center_dist = s - 0.5;
	vec2 f = (center_dist - clamp(center_dist, -region_range, region_range)) * precalc_scale + 0.5;
	
	vec2 mod_texel = texel_floored + f;
	
	vec3 colour = texture2D(u_texture, mod_texel / DSF_TS.zw).rgb;
	
	gl_FragColor = vec4(colour.rgb, 1.0);
}

void main()
{
    dsf_vert();
    dsf_frag();
    gl_FragColor = SWIZ(gl_FragColor);
}
