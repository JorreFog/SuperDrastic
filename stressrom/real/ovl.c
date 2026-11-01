/* dsreal overlays: NPC logic loaded from the card into RAM at 0x02200000 (two variants, swapped at every door) */
#include "game.h"
#ifndef OVLN
#define OVLN 0
#endif
static void npc_update(npc_t *n, int count, u32 frame, const u8 *col) {
    static const s8 dx[4] = { 1, 0, -1, 0 }, dz[4] = { 0, 1, 0, -1 };
    for (int i = 0; i < count; i++) {
        npc_t *p = &n[i];
        p->anim++;
        if (p->wait) { p->wait--; continue; }
        s32 tx = (p->x >> 12) + dx[p->dir & 3], tz = (p->z >> 12) + dz[p->dir & 3];
        int blocked = tx < 0 || tz < 0 || tx > 31 || tz > 31 || col[tz * 32 + tx];
        for (int j = 0; j < count && !blocked; j++)          /* NPCs don't walk into each other */
            if (j != i && (n[j].x >> 12) == tx && (n[j].z >> 12) == tz) blocked = 1;
        if (blocked) { p->dir = (p->dir + 1 + OVLN + (s32)((frame >> 3) & 1)) & 3; p->wait = 8 + OVLN * 8; continue; }
        p->x += dx[p->dir & 3] * (256 + OVLN * 128); p->z += dz[p->dir & 3] * (256 + OVLN * 128);
        if (((p->x | p->z) & 0xFFF) == 0 && ((frame + (u32)i) & 7) == 0) p->dir = (p->dir + (s32)(frame >> 4)) & 3;
    }
}
static u32 hash(const npc_t *n, int count) {
    u32 h = 0xC0FFEE + OVLN;
    for (int i = 0; i < count; i++) h = (h ^ (u32)n[i].x ^ ((u32)n[i].z << 11) ^ (u32)n[i].dir) * 16777619u;
    return h;
}
__attribute__((section(".text.start"), used)) const ovl_t ovl_table = { 0x4C564F00 + OVLN, npc_update, hash };
