// ds-integer-fill -- pixel-perfect DS screens like ds-integer, with the space around them filled by the picture.
//
// Each DS screen (256x192) is shown at the largest whole-number scale that fits the panel, centred, like
// ds-integer: 2x with a border on the RG DS (640x480), 4x filling the RG DS Plus (1024x768). The border shows
// the same picture stretched over the whole panel, blurred and dimmed, instead of a bezel. Taps outside the
// picture do nothing; libdsflip computes the same rectangle and maps touch into it (next line).
// dsflip-viewport: integer
//
// Screen pixels: one fetch at the texel centre (exact under GL_LINEAR). Fill: a 6x6 grid of bilinear fetches at
// texel corners, each the average of 2x2 texels, so 12x12 texels are averaged with no gaps (a sparser grid left
// stripes where it blurred text); 36 fetches, only for the border's pixels (36% of the panel). Dimmed to 45%,
// darker in a thin line around the picture so its edge stays clear.
// highp positions: mediump is fp16 on this Mali, too coarse for pixel positions past 256 (see ds-crisp.frag).

#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec2 v_texcoord;
uniform sampler2D u_texture;
uniform highp vec2 u_texture_size;
uniform highp vec2 u_output_size;

void main()
{
    highp vec2 p = v_texcoord * u_output_size;               // panel pixel (x right, y down), centre at +0.5
    highp float k = max(1.0, floor(min(u_output_size.x / 256.0, u_output_size.y / 192.0)));   // 2 on the RG DS, 4 on the Plus
    highp vec2 win = vec2(256.0, 192.0) * k;
    highp vec2 org = floor((u_output_size - win) * 0.5);
    highp vec2 q = p - org;
    if (q.x >= 0.0 && q.y >= 0.0 && q.x < win.x && q.y < win.y) {
        highp vec2 texel = floor(q / win * u_texture_size);  // kxk blocks at 1x, (k/2)x(k/2) at 2x
        gl_FragColor = vec4(SWIZ(texture2D(u_texture, (texel + 0.5) / u_texture_size)).rgb, 1.0);
        return;
    }
    // the picture stretched over the panel, averaged over the 12x12 texels around this point
    highp vec2 c = floor(v_texcoord * u_texture_size) - 5.0;         // texel corner at the grid's first tap
    highp vec2 inv = 1.0 / u_texture_size;
    vec3 sum = vec3(0.0);
    for (int j = 0; j < 6; j++)
        for (int i = 0; i < 6; i++)
            sum += SWIZ(texture2D(u_texture, (c + vec2(float(i), float(j)) * 2.0) * inv)).rgb;
    // distance outside the picture's edge, for the dark line around it
    vec2 d = abs(p - (org + win * 0.5)) - win * 0.5;
    float out_d = max(d.x, d.y);
    float dim = mix(0.2, 0.45, smoothstep(1.0, 3.0, out_d));
    gl_FragColor = vec4(sum * (dim / 36.0), 1.0);
}
