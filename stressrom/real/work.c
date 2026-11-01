/* dsreal: the game systems that fill a real game's ARM9 frame besides drawing:
 *  - tri_collide: a body against the collision triangles near it (plane distance, then an edge test), ARM code in
 *    ITCM as NitroSDK games keep their hot maths
 *  - vm_run: an event/AI script interpreter (Pokemon's script engine, enemy AI bytecode): a switch dispatch with
 *    branches, calls and memory reads, Thumb code in main RAM */
#include "game.h"

ITCM int tri_collide(const s32 *t, int n, s32 x, s32 y, s32 z, s32 *push) {
    int hits = 0;
    for (int i = 0; i < n; i++, t += 8) {                    /* nx ny nz d, x0 z0 x1 z1 (an edge to test against) */
        if (x < t[4] - 8192 || x > t[6] + 8192) continue;
        s32 d = (s32)(((s64)t[0] * x + (s64)t[1] * y + (s64)t[2] * z) >> 12) - t[3];
        if (d > 4096 || d < -4096) continue;
        s32 ex = t[6] - t[4], ez = t[7] - t[5], px = x - t[4], pz = z - t[5];
        s32 c = (s32)(((s64)ex * pz - (s64)ez * px) >> 12);
        if (c < 0) continue;
        hits++; *push += d;
    }
    return hits;
}

/* script VM: 8 registers, opcodes in bytes; budget = instructions to run this frame */
typedef struct { s32 r[8]; u32 pc, sp, stack[16], ticks; } vm_t;
static vm_t vms[4];
static u8 vcode[4][512];
void vm_init(int k, u32 seed) {
    vm_t *v = &vms[k]; u8 *c = vcode[k];
    for (int i = 0; i < 8; i++) v->r[i] = (s32)(seed * (u32)(i + 3));
    v->pc = 0; v->sp = 0; v->ticks = 0;
    rnd_seed(seed);
    for (int i = 0; i < 512; i += 4) {                       /* a program: arithmetic, compares, jumps, calls, returns */
        u32 r = rnd(); c[i] = (u8)(r % 12); c[i + 1] = (u8)((r >> 8) & 7); c[i + 2] = (u8)((r >> 11) & 7); c[i + 3] = (u8)(r >> 14);
    }
}
u32 vm_run(int k, int budget, const u8 *mem) {
    vm_t *v = &vms[k]; const u8 *c = vcode[k];
    while (budget-- > 0) {
        u32 pc = v->pc & 511 & ~3u; u32 op = c[pc], a = c[pc + 1], b = c[pc + 2], imm = c[pc + 3];
        v->pc = pc + 4;
        switch (op) {
        case 0: v->r[a] += v->r[b]; break;
        case 1: v->r[a] -= (s32)imm; break;
        case 2: v->r[a] = v->r[b] ^ (v->r[a] << 1); break;
        case 3: v->r[a] = mem[(u32)(v->r[b] + (s32)imm) & 1023]; break;
        case 4: if (v->r[a] > v->r[b]) v->pc = imm * 4; break;
        case 5: if (v->r[a] & 1) v->pc = (pc + imm * 4) & 511; break;
        case 6: if (v->sp < 16) { v->stack[v->sp++] = v->pc; v->pc = imm * 4; } break;
        case 7: if (v->sp) v->pc = v->stack[--v->sp]; break;
        case 8: v->r[a] = (v->r[a] * 3 + (s32)imm) & 0xFFFF; break;
        case 9: v->r[a] = v->r[b] >> (imm & 7); break;
        case 10: v->ticks++; if ((v->ticks & 15) == 0) v->pc = 0; break;
        default: v->r[a] = (v->r[a] < v->r[b]) ? v->r[b] : v->r[a] + 1; break;
        }
    }
    u32 h = v->pc;
    for (int i = 0; i < 8; i++) h = (h << 3 | h >> 29) ^ (u32)v->r[i];
    return h;
}
