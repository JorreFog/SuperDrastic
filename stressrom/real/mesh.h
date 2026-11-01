/* dsreal: small mesh builders that emit packed GX commands (dl_cmd) */
#pragma once
#include "game.h"
#define FX(v) ((s32)((v) * 4096))
static inline u32 v16a(s32 x, s32 y) { return ((u32)x & 0xFFFF) | ((u32)y << 16); }
static inline u32 v10(s32 x, s32 y, s32 z) { return ((u32)(x >> 6) & 0x3FF) | (((u32)(y >> 6) & 0x3FF) << 10) | (((u32)(z >> 6) & 0x3FF) << 20); }
static inline u32 nrm(s32 x, s32 y, s32 z) { return ((u32)(x >> 3) & 0x3FF) | (((u32)(y >> 3) & 0x3FF) << 10) | (((u32)(z >> 3) & 0x3FF) << 20); }
static inline u32 tc(s32 s, s32 t) { return ((u32)(s << 4) & 0xFFFF) | ((u32)(t << 4) << 16); }   /* texels */
static inline void vtx16(s32 x, s32 y, s32 z) { dl_cmd(G_VTX16, 2, v16a(x, y), (u32)z & 0xFFFF); }
/* an axis box, quads, a normal per face; `tsz` texels across a face; fmt alternates the vertex formats */
static void mesh_box(s32 x0, s32 y0, s32 z0, s32 x1, s32 y1, s32 z1, u32 tp, s32 tsz, int fmt) {
    static const s8 f[6][4][3] = {
        {{0,0,1},{1,0,1},{1,1,1},{0,1,1}}, {{1,0,0},{0,0,0},{0,1,0},{1,1,0}}, {{0,1,1},{1,1,1},{1,1,0},{0,1,0}},
        {{0,0,0},{1,0,0},{1,0,1},{0,0,1}}, {{1,0,1},{1,0,0},{1,1,0},{1,1,1}}, {{0,0,0},{0,0,1},{0,1,1},{0,1,0}} };
    static const s8 n[6][3] = { {0,0,1}, {0,0,-1}, {0,1,0}, {0,-1,0}, {1,0,0}, {-1,0,0} };
    static const s8 uv[4][2] = { {0,1}, {1,1}, {1,0}, {0,0} };
    dl_cmd(G_TEXPARAM, 1, tp);
    dl_cmd(G_BEGIN, 1, 1);
    for (int i = 0; i < 6; i++) {
        dl_cmd(G_NORMAL, 1, nrm(n[i][0] * 4088, n[i][1] * 4088, n[i][2] * 4088));
        for (int k = 0; k < 4; k++) {
            s32 x = f[i][k][0] ? x1 : x0, y = f[i][k][1] ? y1 : y0, z = f[i][k][2] ? z1 : z0;
            dl_cmd(G_TEXCOORD, 1, tc(uv[k][0] * tsz, uv[k][1] * tsz));
            if (fmt == 1) dl_cmd(G_VTX10, 1, v10(x, y, z)); else vtx16(x, y, z);
        }
    }
}
