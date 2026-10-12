// nnddss-retro-tiles: Anbernic NNDDSS 1.0.2's retro-tiles-rgds+ (Hyllian's, ported by MiZai), as a BananaOS shader (SuperDrastic and BananaDS) for a DS screen drawn to the panel.
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

#define SCREEN_RES_SCALE (DSF_TS.x / 256.0)

#if GL_ES

#endif
// Configuration
#define TILE_FORMAT 0.0   // "Tile Format [ Normal | Deep]" (0.0-1.0)
#define TILE_DEPTH 1.0    // "Tile Depth" (0.0-1.0)
#define TILE_SIZE 0.5     // "Tile Size" (0.4-0.6)
#define COLOR_BOOST 1.0   // "Color Boost" (1.0-2.0)
#define OVERSCAN 0.0      // "Over Scan" (-3.0-3.0)
#define InputGamma 2.4    // "INPUT GAMMA" (0.0-4.0)
#define OutputGamma 2.2   // "OUTPUT GAMMA" (0.0-3.0)

#define GAMMA_IN(color)     pow(color, vec3(InputGamma))
#define GAMMA_OUT(color)    pow(color, vec3(1.0 / OutputGamma))

vec2 vTexCoord;

void dsf_vert()
{

    // æ­¥éª¤1ï¼èªå¨è®¡ç®ZhengShuBeiï¼NDSåå§åè¾¨ççæ´æ°åï¼
    float integer_scale_x = u_output_size.x / 256.0;
    float integer_scale_y = u_output_size.y / 192.0;
    float integer_scale = floor(min(integer_scale_x, integer_scale_y));  // åå®½é«åæ°æå°å¼å¹¶åä¸åæ´
    integer_scale = max(integer_scale, 1.0);  // ç¡®ä¿ZhengShuBeiè³å°ä¸º1

    // æ­¥éª¤2ï¼è®¡ç®åºåç®æ å°ºå¯¸ï¼256*integer_scale, 192*integer_scaleï¼
    vec2 base_target_size = vec2(256.0 * integer_scale, 192.0 * integer_scale);

    // æ­¥éª¤3ï¼åºäºåºåç®æ å°ºå¯¸è®¡ç®scale_factor
    vec2 scale_factor = base_target_size / DSF_TS.zw;

    // åºç¨è¿æ«æå¹¶éå¶æå°ç¼©æ¾
    vec2 box_scale = scale_factor + OVERSCAN;
    box_scale = vec2(min(box_scale.x, box_scale.y));
    box_scale = max(box_scale, vec2(1.0 / SCREEN_RES_SCALE));

    // è®¡ç®çº¹çåæ ç¼©æ¾æ¯ä¾
    vec2 scale = scale_factor / box_scale;
    vec2 middle = vec2(0.5);
    vec2 diff = v_texcoord * 1.0001 - middle;  // é¿åç²¾åº¦ä¸¢å¤±
    vTexCoord = middle + diff * scale;  // å±ä¸­å¹¶ç¼©æ¾çº¹çåæ 
}

#define SCREEN_RES_SCALE (DSF_TS.x / 256.0)

#if GL_ES

#endif
// Configuration
#define TILE_FORMAT 0.0   // "Tile Format [ Normal | Deep]" (0.0-1.0)
#define TILE_DEPTH 1.0    // "Tile Depth" (0.0-1.0)
#define TILE_SIZE 0.5     // "Tile Size" (0.4-0.6)
#define COLOR_BOOST 1.0   // "Color Boost" (1.0-2.0)
#define OVERSCAN 0.0      // "Over Scan" (-3.0-3.0)
#define InputGamma 2.4    // "INPUT GAMMA" (0.0-4.0)
#define OutputGamma 2.2   // "OUTPUT GAMMA" (0.0-3.0)

#define GAMMA_IN(color)     pow(color, vec3(InputGamma))
#define GAMMA_OUT(color)    pow(color, vec3(1.0 / OutputGamma))

float fade(float size) {
    size = clamp(size, -TILE_DEPTH, 1.0);
    size = 1.0 - size * size * size * mix(size * size, 1.0, TILE_FORMAT);
    return size * size;
}

void dsf_frag() {
    // æå¨æ£æ¥çº¹çåæ æ¯å¦è¶åºææèå´ï¼è¶åºåè¾åºé»è²è¾¹æ¡
    if (vTexCoord.x < 0.0 || vTexCoord.x > 1.0 || vTexCoord.y < 0.0 || vTexCoord.y > 1.0) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);  // è¾¹æ¡é¢è²ï¼é»è²ï¼
        return;
    }
    
    // ç·ç ææè®¡ç®
    vec2 pos = fract(vTexCoord * DSF_TS.zw / SCREEN_RES_SCALE) - vec2(0.5);
    vec2 dir = sign(pos);
    vec2 g1 = dir * vec2(DSF_TS.x, 0.0);  // 1/w step
    vec2 g2 = dir * vec2(0.0, DSF_TS.y);  // 1/h step

    // éæ ·ç¸é»åç´ (åå§é¢è²ï¼æ··åååç»ä¸å gamma)
    vec3 A = texture2D(u_texture, vTexCoord).xyz;
    vec3 B = texture2D(u_texture, vTexCoord + g1).xyz;
    vec3 C = texture2D(u_texture, vTexCoord + g2).xyz;
    vec3 D = texture2D(u_texture, vTexCoord + g1 + g2).xyz;

    // ç·ç è¾¹ç¼æ··å
    vec2 dx = vec2(pos.x, 1.0 - pos.x) / TILE_SIZE;
    vec2 dy = vec2(pos.y, 1.0 - pos.y) / TILE_SIZE;
    vec2 wx = vec2(fade(dx.x), fade(dx.y));
    vec2 wy = vec2(fade(dy.x), fade(dy.y));
    vec3 color = (A * wx.x + B * wx.y) * wy.x + (C * wx.x + D * wx.y) * wy.y;
    color *= COLOR_BOOST;

    // åæ¬¡åå¹¶ gammaï¼pow(color, InputGamma/OutputGamma)
    // æ¿ä»£ååæ¯ä¸ªéæ ·å GAMMA_IN + æç» GAMMA_OUT ç 15 æ¬¡ pow
    gl_FragColor = vec4(pow(color, vec3(InputGamma / OutputGamma)), 1.0);
}

void main()
{
    dsf_vert();
    dsf_frag();
    gl_FragColor = SWIZ(gl_FragColor);
}
