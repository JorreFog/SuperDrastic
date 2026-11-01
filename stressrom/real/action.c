/* dsreal scene 2, "action": 2D action on both screens (a tall playfield across the two, as Contra 4 / Castlevania).
 * Each engine: 4 text BGs (512x256, 4bpp) with parallax, 128 (A) / 96 (B) sprites incl. affine and semi-transparent
 * ones, OBJ tiles streamed by DMA each frame (animation frames), palette cycling, alpha blending, mosaic; raster
 * effects: engine A's BG2 scroll by HBlank DMA from a per-line table, engine B's window (a spotlight) by an HBlank
 * IRQ per line. Entities: player, enemies, bullets, particles; tile-map collision and a grid for hits.
 * Spikes: every 4 s an explosion (40 particles, shake, flash, three sounds) and a 16 KB BG tile upload (a new zone). */
#include "game.h"

#define MAXE 320
typedef struct { s32 x, y, vx, vy; u16 type, hp, anim, cell; } ent_t;     /* type 0 free, 1 enemy, 2 bullet, 3 particle */
static ent_t ent[MAXE];
static u8 col[64 * 32];                                     /* collision tiles */
static s16 grid_head[16 * 8], grid_next[MAXE];
static u16 oam[2][128 * 4] __attribute__((aligned(4)));
static u16 hdma_tab[2][193];
static u32 tiles_stage[0x4000 / 4];
static u16 objframe[0x1000] __attribute__((aligned(4)));
static s32 scroll_x, shake, flash, score, nent, kills, px, py;
static volatile s32 spot_cx, spot_r;
static int cur;
static u32 vmh;

static void hblank_spot(void) {                             /* engine B's WIN0 per line: a circle */
    s32 y = VCOUNT; if (y >= 192) y = 0;
    s32 dy = y - 96, r2 = spot_r * spot_r - dy * dy;
    if (r2 <= 0) { WIN0H(1) = 0; return; }
    s32 w = (s32)hw_sqrt((u32)r2), l = spot_cx - w, r = spot_cx + w;
    if (l < 0) l = 0; if (r > 255) r = 255;
    WIN0H(1) = (u16)((l << 8) | r);
}

static void tiles_make(u32 seed, volatile u16 *dst, u32 bytes) {
    rnd_seed(seed);
    for (u32 i = 0; i < bytes / 4; i++) { u32 r = rnd(); tiles_stage[i] = (r & 0x0F0F0F0F) | ((r >> 4 & 0x07070707) << 4); }
    dma_copy32((void *)dst, tiles_stage, bytes);
}

static void action_enter(void) {
    rnd_seed(0xAC71);
    /* VRAM: A -> engine A OBJ (128 KB), B -> engine A BG 2nd half (0x06020000)... keep E for BG; C sub BG, I sub OBJ */
    VRAMCNT(0) = 0x82; VRAMCNT(1) = 0x80; VRAMCNT(6) = 0x80; VRAMCNT(5) = 0x80;
    for (int e = 0; e < 2; e++) {
        DISPCNT(e) = 0x10000 | 0 | (0xF << 8) | (1 << 12) | (1 << 4) | (e ? (1 << 13) : 0);
        u32 base = e ? 0x06200000 : 0x06000000;
        BGCNT(e, 0) = 0 | (0 << 2) | (31 << 8);              /* HUD text: font chars at block 0 */
        BGCNT(e, 1) = 1 | (2 << 2) | (26 << 8) | (1 << 14);  /* 512x256: map blocks 26-27 */
        BGCNT(e, 2) = 2 | (2 << 2) | (28 << 8) | (1 << 14);
        BGCNT(e, 3) = 3 | (2 << 2) | (24 << 8) | (1 << 14) | (1 << 6);   /* mosaic */
        tiles_make(0x100 + e, (volatile u16 *)(base + 0x8000), 0x4000);
        volatile u16 *m = (volatile u16 *)(base + 24 * 0x800);
        for (int i = 0; i < 3 * 2048; i++) { u32 r = rnd(); m[i] = (u16)((r & 0x1FF) | (r & 0xC00) | ((1 + (r >> 12) % 7) << 12)); }
        for (int i = 16; i < 128; i++) PAL_BG(e)[i] = (u16)(rnd() & 0x7FFF);
        for (int i = 0; i < 256; i++) PAL_OBJ(e)[i] = (u16)(rnd() & 0x7FFF);
        BLDCNT(e) = (1 << 2) | (1 << 6) | (1 << 11) | (1 << 12); BLDALPHA(e) = 10 | (6 << 8);
    }
    WININ(1) = 0x3F; WINOUT(1) = 0x13; WIN0V(1) = 191;      /* inside the spot: everything; outside: BG0, BG1, OBJ */
    MOSAIC(0) = 0x11; MOSAIC(1) = 0x22;
    for (int y = 0; y < 32; y++) for (int x = 0; x < 64; x++) col[y * 64 + x] = (u8)((y > 26) || ((rnd() & 31) == 0 && y > 8));
    for (int i = 0; i < MAXE; i++) ent[i].type = 0;
    nent = 40 + (s32)level * 28; if (nent > MAXE) nent = MAXE;
    for (int i = 0; i < nent / 3; i++) ent[i] = (ent_t){ (s32)(rnd() & 511) << 8, (s32)(rnd() % 200) << 8, 0, 0, 1, 3, 0, 0 };
    scroll_x = 0; shake = 0; flash = 0; score = 0; kills = 0; px = 40 << 8; py = 150 << 8; cur = 0;
    spot_cx = 128; spot_r = 70; vm_init(0, 0x5555); vmh = 0;
    hblank_hook = hblank_spot;
    DISPSTAT |= (1 << 4);
    IE |= IRQ_HBLANK;
    fifo_send((4u << 28) | 1);
}

static ent_t *spawn(int type) {
    for (int i = 0; i < nent; i++) if (!ent[i].type) { ent[i].type = (u16)type; return &ent[i]; }
    return 0;
}

static void action_frame(u32 f) {
    u32 t0 = timer_now();
    /* player: scripted run and gun */
    px += 300; if (px > (400 << 8)) px = 40 << 8;
    py = (150 << 8) + (sn((s32)f * 9) * 40 >> 4);
    if ((f & 3) == 0) { ent_t *b = spawn(2); if (b) *b = (ent_t){ px, py, 1200, (s32)(rnd() & 255) - 128, 2, 1, 0, 0 }; }
    if ((f & 15) == 0) { ent_t *e = spawn(1); if (e) *e = (ent_t){ (s32)(scroll_x + 300) << 8, (s32)(rnd() % 180) << 8, -200, 0, 1, 3, 0, 0 }; }
    /* move + tile collision, then bucket into the grid */
    for (int i = 0; i < 16 * 8; i++) grid_head[i] = -1;
    for (int i = 0; i < nent; i++) {
        ent_t *e = &ent[i];
        if (!e->type) continue;
        e->x += e->vx; e->y += e->vy;
        if (e->type == 1) { e->vy += (e->y < py) ? 8 : -8; e->vx -= (e->x > px) ? 2 : -2; }
        if (e->type == 3) { e->vy += 20; if (!--e->hp) { e->type = 0; continue; } }
        s32 tx = (e->x >> 11) & 63, ty = (e->y >> 11) & 31;
        if (col[ty * 64 + tx]) { if (e->type == 2) { e->type = 0; continue; } e->vy = -e->vy / 2; e->y -= 4 << 8; }
        if (e->x < (scroll_x - 64) << 8 || e->x > (scroll_x + 600) << 8 || e->y < -(64 << 8) || e->y > (400 << 8)) { e->type = 0; continue; }
        e->anim++;
        s32 c = ((e->y >> 14) & 7) * 16 + ((e->x >> 13) & 15);
        e->cell = (u16)c; grid_next[i] = grid_head[c]; grid_head[c] = (s16)i;
    }
    /* bullets against enemies in the same cell */
    for (int i = 0; i < nent; i++) {
        ent_t *b = &ent[i];
        if (b->type != 2) continue;
        for (s32 j = grid_head[b->cell]; j >= 0; j = grid_next[j]) {
            ent_t *e = &ent[j];
            if (e->type != 1) continue;
            s32 dx = (b->x - e->x) >> 8, dy = (b->y - e->y) >> 8;
            if (dx > -12 && dx < 12 && dy > -12 && dy < 12) {
                b->type = 0; score += 10;
                if (!--e->hp) { e->type = 0; kills++; for (int k = 0; k < 4; k++) { ent_t *p = spawn(3); if (p) *p = (ent_t){ e->x, e->y, (s32)(rnd() & 1023) - 512, -(s32)(rnd() & 1023), 3, 20, 0, 0 }; } }
                break;
            }
        }
    }
    if (f % 240 == 120) {                                    /* the explosion spike */
        for (int k = 0; k < 40; k++) { ent_t *p = spawn(3); if (p) *p = (ent_t){ px + (100 << 8), py, (s32)(rnd() & 2047) - 1024, -(s32)(rnd() & 2047), 3, 30, 0, 0 }; }
        shake = 16; flash = 8;
        fifo_send((3u << 28) | 1 | (40 << 8) | (100 << 16)); fifo_send((3u << 28) | 7 | (90 << 8) | (100 << 16)); fifo_send((3u << 28) | 3 | (64 << 8) | (80 << 16));
    }
    vmh ^= vm_run(0, nent * 30, col);
    scroll_x += 2;
    prof[0] = timer_now() - t0;
    /* OAM for both engines: entities split by screen (y < 192: A, else B) */
    int na = 0, nb = 0;
    for (int i = 0; i < 128 * 4; i += 4) { oam[0][i] = 0x0200; oam[1][i] = 0x0200; }
    for (int i = 0; i < nent; i++) {
        ent_t *e = &ent[i]; if (!e->type) continue;
        s32 sx = (e->x >> 8) - scroll_x, sy = e->y >> 8;
        int scr = sy >= 192; u16 *o;
        if (!scr) { if (na >= 120) continue; o = &oam[0][na++ * 4]; } else { if (nb >= 92) continue; o = &oam[1][nb++ * 4]; sy -= 192; }
        u16 a0 = (u16)(sy & 255), a1 = (u16)(sx & 511), a2 = (u16)((e->type * 32 + (e->anim >> 3 & 3) * 4) | ((e->type & 3) << 12));
        if (e->type == 1 && (i & 7) == 0) { a0 |= (1 << 8) | (1 << 9); a1 |= (u16)((i >> 3 & 3) << 9) | (1 << 14); }   /* affine 16x16 */
        else if (e->type == 3) a0 |= (1 << 10);              /* semi-transparent */
        else a1 |= (1 << 14);
        o[0] = a0; o[1] = a1; o[2] = a2;
    }
    /* player sprites (both screens show part of it as it crosses) */
    oam[0][124 * 4] = (u16)((py >> 8) & 255); oam[0][124 * 4 + 1] = (u16)((((px >> 8) - scroll_x) & 511) | (2 << 14)); oam[0][124 * 4 + 2] = 0;
    /* per-line BG2 scroll table for the HBlank DMA (a wave) */
    u16 *t = hdma_tab[cur ^ 1];
    for (int y = 0; y <= 192; y++) t[y] = (u16)(scroll_x / 2 + (sn(y * 12 + (s32)f * 10) >> 9));
    /* animation frames to stream: 4 KB for A, 2 KB for B */
    for (int i = 0; i < 0x1000; i += 8) { u32 r = (u32)f * 2654435761u + (u32)i; objframe[i] = (u16)r; objframe[i + 4] = (u16)(r >> 16); }
}

static void action_vblank(u32 f) {
    dma_copy32((void *)OAM(0), oam[0], 1024); dma_copy32((void *)OAM(1), oam[1], 1024);
    volatile s16 *aff = (volatile s16 *)0x07000000;
    for (int g = 0; g < 4; g++) {
        s32 a = (s32)f * (4 + g), c = cs(a) >> 4, s = sn(a) >> 4;
        aff[g * 16 + 3] = (s16)c; aff[g * 16 + 7] = (s16)-s; aff[g * 16 + 11] = (s16)s; aff[g * 16 + 15] = (s16)c;
    }
    dma_copy32((void *)(0x06400000 + 0x4000), objframe, 0x2000);
    dma_copy32((void *)(0x06600000 + 0x2000), objframe, 0x1000);
    /* palette cycling: 32 colours per engine */
    static u16 cyc[32] __attribute__((aligned(4)));
    for (int i = 0; i < 32; i++) cyc[i] = (u16)RGB((i + f) & 31, (i * 2 + f) & 31, 16);
    dma_copy32((void *)(PAL_BG(0) + 32), cyc, 64); dma_copy32((void *)(PAL_BG(1) + 32), cyc, 64);
    s32 sh = shake ? ((s32)(rnd() & 7) - 4) : 0; if (shake) shake--;
    for (int e = 0; e < 2; e++) {
        BGHOFS(e, 1) = (u16)(scroll_x + sh); BGHOFS(e, 3) = (u16)(scroll_x / 4); BGVOFS(e, 1) = (u16)(e * 192 + sh);
        MASTER_BRIGHT(e) = flash ? (u16)((1 << 14) | (flash * 2)) : 0;
    }
    if (flash) flash--;
    BGHOFS(1, 2) = (u16)(scroll_x / 2);
    /* engine A BG2: the HBlank DMA walks this frame's table (line 0 now, then one entry a line) */
    cur ^= 1;
    DMACNT(0) = 0;
    BGHOFS(0, 2) = hdma_tab[cur][0];
    DMASAD(0) = (u32)&hdma_tab[cur][1]; DMADAD(0) = 0x04000018;
    DMACNT(0) = DMA_EN | DMA_HBL | DMA_REPEAT | DMA_DST_FIX | 1;
    spot_cx = 128 + (sn((s32)f * 5) >> 6); spot_r = 60 + (sn((s32)f * 3) >> 8);
    if (f % 240 == 200) tiles_make(0x200 + f, (volatile u16 *)0x06008000, 0x4000);   /* a new zone's tiles */
    volatile u16 *hud = (volatile u16 *)0x0600F800;
    text(hud, 1, 0, "SCORE"); text_dec(hud, 7, 0, (u32)score, 6); text(hud, 20, 0, "KILLS"); text_dec(hud, 26, 0, (u32)kills, 4);
}

static void action_leave(void) {
    DMACNT(0) = 0;
    IE &= ~IRQ_HBLANK; DISPSTAT &= ~(1 << 4); hblank_hook = 0;
    for (int e = 0; e < 2; e++) { MASTER_BRIGHT(e) = 0; BLDCNT(e) = 0; MOSAIC(e) = 0; BGHOFS(e, 1) = 0; BGHOFS(e, 2) = 0; BGHOFS(e, 3) = 0; BGVOFS(e, 1) = 0; }
    DISPCNT(1) &= ~(1u << 13);
}

static u32 action_hash(void) {
    u32 h = (u32)score * 31u + (u32)kills + vmh;
    for (int i = 0; i < nent; i++) if (ent[i].type) h = (h ^ (u32)ent[i].x ^ ((u32)ent[i].y << 3)) * 16777619u;
    return h;
}

const scene_t scene_action = { "ACTION", action_enter, action_frame, action_vblank, action_leave, action_hash, 0 };
