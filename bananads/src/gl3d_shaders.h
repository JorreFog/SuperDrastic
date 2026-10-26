// melonDS's 3D OpenGL shaders (src/OpenGL_shaders/3D*.glsl, GPL-3.0-or-later, the melonDS team), ported from
// GLSL 1.40 to GLSL ES 3.10 for render.gpu3d=gl (gl3d_backend.cpp): explicit float literals, output locations,
// the polygon attribute as uvec3 (its pointer is GL_UNSIGNED_INT), the display-capture texture paths left out
// (BananaDS's captures stay in VRAM at 1x, as with Gengis Engine).
#pragma once

namespace bananads::gl3dsh {

#define GL3D_ES_HEAD "#version 310 es\nprecision highp float;\nprecision highp int;\nprecision highp usampler2DArray;\n" \
    "precision highp usampler2D;\nprecision highp sampler2D;\n"

#define GL3D_UCONFIG \
    "layout(std140) uniform uConfig\n{\n vec2 uScreenSize;\n int uDispCnt;\n vec4 uToonColors[32];\n vec4 uEdgeColors[8];\n" \
    " vec4 uFogColor;\n float uFogDensity[34];\n int uFogOffset;\n int uFogShift;\n};\n"

static const char kClearVS[] = GL3D_ES_HEAD R"(
in vec2 vPosition;
uniform uint uDepth;
void main()
{
    float fdepth = (float(uDepth) / 8388608.0) - 1.0;
    gl_Position = vec4(vPosition, fdepth, 1.0);
}
)";

static const char kClearFS[] = GL3D_ES_HEAD R"(
uniform uvec4 uColor;
uniform uint uOpaquePolyID;
uniform uint uFogFlag;
layout(location = 0) out vec4 oColor;
layout(location = 1) out vec4 oAttr;
void main()
{
    oColor = vec4(uColor).rgba / 31.0;
    oAttr.r = float(uOpaquePolyID) / 63.0;
    oAttr.g = 0.0;
    oAttr.b = float(uFogFlag);
    oAttr.a = 1.0;
}
)";

static const char kClearBitmapVS[] = GL3D_ES_HEAD R"(
in vec2 vPosition;
out vec2 fTexcoord;
void main()
{
    fTexcoord = (vPosition + 1.0) * vec2(0.5, 0.375);
    gl_Position = vec4(vPosition, 0.0, 1.0);
}
)";

static const char kClearBitmapFS[] = GL3D_ES_HEAD R"(
uniform usampler2D ClearBitmapColor;
uniform usampler2D ClearBitmapDepth;
uniform vec2 uClearBitmapOffset;
uniform uint uOpaquePolyID;
in vec2 fTexcoord;
layout(location = 0) out vec4 oColor;
layout(location = 1) out vec4 oAttr;
void main()
{
    vec2 pos = fTexcoord + uClearBitmapOffset;
    vec4 color = vec4(texture(ClearBitmapColor, pos)) / vec4(63,63,63,31);
    uint depth = texture(ClearBitmapDepth, pos).r;
    float fdepth = float(depth & 0xFFFFFFu) / 16777216.0;
    oColor = color;
    oAttr.r = float(uOpaquePolyID) / 63.0;
    oAttr.g = 0.0;
    oAttr.b = float(depth >> 24);
    oAttr.a = 1.0;
    gl_FragDepth = fdepth;
}
)";

static const char kRenderVS[] = GL3D_ES_HEAD GL3D_UCONFIG R"(
in uvec4 vPosition;
in uvec4 vColor;
in ivec2 vTexcoord;
in uvec3 vPolygonAttr;
smooth out vec4 fColor;
smooth out vec2 fTexcoord;
flat out ivec3 fPolygonAttr;
#ifdef WBuffer
smooth out float fZ;
#endif
void main()
{
    ivec3 pa = ivec3(vPolygonAttr);
    int attr = pa.x;
    int zshift = (attr >> 16) & 0x1F;
    vec4 fpos;
    fpos.xy = ((vec2(vPosition.xy) * 2.0) / uScreenSize) - 1.0;
#ifdef WBuffer
    fZ = float(vPosition.z << uint(zshift)) / 16777216.0;
    fpos.z = 0.0;
#else
    fpos.z = (float(vPosition.z << uint(zshift)) / 8388608.0) - 1.0;
#endif
    fpos.w = float(vPosition.w) / 65536.0;
    fpos.xyz *= fpos.w;
    int texwidth = pa.z & 0xFFFF;
    int texheight = (pa.z >> 16) & 0xFFFF;
    vec2 texfactor = 1.0 / (16.0 * vec2(texwidth, texheight));
    vec2 texcoord = vec2(vTexcoord);
    fColor = vec4(vColor) / vec4(255.0,255.0,255.0,31.0);
    fTexcoord = texcoord * texfactor;
    fPolygonAttr = ivec3(pa.x, pa.y & 0xFFFF, 0);
    gl_Position = fpos;
}
)";

static const char kRenderFS[] = GL3D_ES_HEAD GL3D_UCONFIG R"(
uniform usampler2DArray CurTexture;
uniform int uRenderMode; // 0=opaque 1=translucent 2=shadowmask
smooth in vec4 fColor;
smooth in vec2 fTexcoord;
flat in ivec3 fPolygonAttr;
#ifdef WBuffer
smooth in float fZ;
#endif
layout(location = 0) out vec4 oColor;
layout(location = 1) out vec4 oAttr;
vec4 FinalColor()
{
    vec4 col;
    vec4 vcol = fColor;
    int blendmode = (fPolygonAttr.x >> 4) & 0x3;
    if (blendmode == 2)
    {
        if ((uDispCnt & (1<<1)) == 0)
            vcol.rgb = uToonColors[int(vcol.r * 31.0)].rgb;
        else
            vcol.rgb = vcol.rrr;
    }
    if (fPolygonAttr.y == 0xFFFF)
    {
        col = vcol;
    }
    else
    {
        vec3 texcoord = vec3(fTexcoord, float(fPolygonAttr.y));
        vec4 tcol = vec4(texture(CurTexture, texcoord)) / vec4(63,63,63,31);
        if ((blendmode & 1) != 0)
        {
            col.rgb = (tcol.rgb * tcol.a) + (vcol.rgb * (1.0-tcol.a));
            col.a = vcol.a;
        }
        else
            col = vcol * tcol;
    }
    if (blendmode == 2)
    {
        if ((uDispCnt & (1<<1)) != 0)
        {
            vec3 tooncolor = uToonColors[int(vcol.r * 31.0)].rgb;
            col.rgb = min(col.rgb + tooncolor, 1.0);
        }
    }
    return col.rgba;
}
void main()
{
    if (uRenderMode == 2)
    {
        oColor = vec4(0,0,0,1);
    }
    else
    {
        vec4 col = FinalColor();
        if (uRenderMode == 0)
        {
            if (col.a < 30.5/31.0) discard;
            oAttr.r = float((fPolygonAttr.x >> 24) & 0x3F) / 63.0;
            oAttr.g = 0.0;
            oAttr.b = float((fPolygonAttr.x >> 15) & 0x1);
            oAttr.a = 1.0;
        }
        else
        {
            if (col.a < 0.5/31.0) discard;
            if (col.a >= 30.5/31.0) discard;
            oAttr.b = 0.0;
            oAttr.a = 1.0;
        }
        oColor = col;
    }
#ifdef WBuffer
    gl_FragDepth = fZ;
#endif
}
)";

static const char kFinalPassVS[] = GL3D_ES_HEAD R"(
in vec2 vPosition;
void main()
{
    gl_Position = vec4(vPosition, 0.0, 1.0);
}
)";

static const char kFinalPassEdgeFS[] = GL3D_ES_HEAD GL3D_UCONFIG R"(
uniform sampler2D DepthBuffer;
uniform sampler2D AttrBuffer;
layout(location = 0) out vec4 oColor;
bool isgood(vec4 attr, float depth, int refPolyID, float refDepth)
{
    int polyid = int(attr.r * 63.0);
    return polyid != refPolyID && refDepth < depth;
}
void main()
{
    ivec2 coord = ivec2(gl_FragCoord.xy);
    int scale = 1;
    vec4 ret = vec4(0,0,0,0);
    vec4 depth = texelFetch(DepthBuffer, coord, 0);
    vec4 attr = texelFetch(AttrBuffer, coord, 0);
    int polyid = int(attr.r * 63.0);
    if (attr.g != 0.0)
    {
        vec4 depthU = texelFetch(DepthBuffer, coord + ivec2(0,-scale), 0);
        vec4 attrU = texelFetch(AttrBuffer, coord + ivec2(0,-scale), 0);
        vec4 depthD = texelFetch(DepthBuffer, coord + ivec2(0,scale), 0);
        vec4 attrD = texelFetch(AttrBuffer, coord + ivec2(0,scale), 0);
        vec4 depthL = texelFetch(DepthBuffer, coord + ivec2(-scale,0), 0);
        vec4 attrL = texelFetch(AttrBuffer, coord + ivec2(-scale,0), 0);
        vec4 depthR = texelFetch(DepthBuffer, coord + ivec2(scale,0), 0);
        vec4 attrR = texelFetch(AttrBuffer, coord + ivec2(scale,0), 0);
        if (isgood(attrU, depthU.r, polyid, depth.r) || isgood(attrD, depthD.r, polyid, depth.r) ||
            isgood(attrL, depthL.r, polyid, depth.r) || isgood(attrR, depthR.r, polyid, depth.r))
        {
            ret.rgb = uEdgeColors[polyid >> 3].rgb;
            if ((uDispCnt & (1<<4)) != 0) ret.a = 0.5;
            else ret.a = 1.0;
        }
    }
    oColor = ret;
}
)";

static const char kFinalPassFogFS[] = GL3D_ES_HEAD GL3D_UCONFIG R"(
uniform sampler2D DepthBuffer;
uniform sampler2D AttrBuffer;
layout(location = 0) out vec4 oColor;
vec4 CalculateFog(float depth)
{
    int idepth = int(depth * 16777216.0);
    int densityid, densityfrac;
    if (idepth < uFogOffset)
    {
        densityid = 0;
        densityfrac = 0;
    }
    else
    {
        uint udepth = uint(idepth);
        udepth -= uint(uFogOffset);
        udepth = (udepth >> 2) << uint(uFogShift);
        densityid = int(udepth >> 17);
        if (densityid >= 32)
        {
            densityid = 32;
            densityfrac = 0;
        }
        else
            densityfrac = int(udepth & uint(0x1FFFF));
    }
    float density = mix(uFogDensity[densityid], uFogDensity[densityid+1], float(densityfrac)/131072.0);
    return vec4(density, density, density, density);
}
void main()
{
    ivec2 coord = ivec2(gl_FragCoord.xy);
    vec4 ret = vec4(0,0,0,0);
    vec4 depth = texelFetch(DepthBuffer, coord, 0);
    vec4 attr = texelFetch(AttrBuffer, coord, 0);
    if (attr.b != 0.0) ret = CalculateFog(depth.r);
    oColor = ret;
}
)";

// the host's pack pass (gpu3d_host.cpp): the backend's picture into Gengis's hi-res row layout (each row as its
// pixels sk, then sk + 1, ... 256 each) in melonDS's 3D pixel format (r6 | g6 << 8 | b6 << 16 | a5 << 24, as bytes
// of an RGBA8 target), so the read-back is the 2D engine's input as it is
static const char kPackVS[] = GL3D_ES_HEAD R"(
void main()
{
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char kPackFS[] = GL3D_ES_HEAD R"(
uniform sampler2D uTex;
uniform int uScale;
layout(location = 0) out vec4 o;
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 s = ivec2((p.x & 255) * uScale + (p.x >> 8), p.y);
    vec4 c = texelFetch(uTex, s, 0);
    o = round(c * vec4(63.0, 63.0, 63.0, 31.0)) / 255.0;
}
)";

}  // namespace bananads::gl3dsh
