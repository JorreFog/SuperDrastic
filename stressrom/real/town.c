/* dsreal scene 3, "town": an RPG town in 3D with 2D on top and a touch menu below, modelled on Pokemon HeartGold /
 * Platinum's overworld (Gen 4): the map blocks around the player as 3D ground, buildings with outline lines,
 * NPCs and trees as textured billboards (NPC animation frames uploaded into texture VRAM in the vblank, as Gen 4
 * does), a narrow camera so most of the submitted polygons are clipped away (Origin HeartGold's first grass area:
 * 568 polygons, 81% clipped entirely, ~2,150 vertices, docs/PERF.md), a text box printed by the CPU into BG tiles,
 * map chunks streamed from the card as the player walks, and every 8 s a door: fade, overlay + texture set + map
 * loaded from the card (code loaded into RAM: the JIT's invalidation path), fade back. NPC logic runs in the overlay. */
#include "mesh.h"

#define BLK 6                       /* quads per block side */
#define BLKU FX(2)                  /* one quad = 2 units */
#define MAXN 48
static npc_t npc[MAXN];
static u32 dl_ground[4][256], dl_house[256], dl_bill[64], dl_lines[64];
static dl_t g_ground[4], g_house, g_bill, g_lines;
static u8 chunk[3][3][0x2000] __attribute__((aligned(4)));  /* the 3x3 map chunks around the player */
static u8 colmap[32 * 32];
static s32 pl_x, pl_z, pl_dir, cam_x, cam_z, nnpc, ntree, door_t, fade, ovl_cur, tex_cur, blk_x, blk_z, loads;
static u32 text_tiles[2][28][16] __attribute__((aligned(4)));   /* 2 lines x 28 cells x (2 tiles of 4bpp 8x8) */
static u32 msg_pos, msg_n, vmh, ohg, glyph_reads;
static u8 glyph_buf[0x200] __attribute__((aligned(4)));
static u16 oamb[128 * 4] __attribute__((aligned(4)));
static u16 npc_up[4][128] __attribute__((aligned(4)));
static const char *const msgs[4] = { "WELCOME TO NEW BARK TOWN. THE WIND BLOWS", "PROF. ELM IS LOOKING FOR YOU. GO AND SEE",
                                     "A WILD POKEMON APPEARED IN THE TALL GRASS", "YOUR POKEMON ARE IN GOOD HEALTH. COME BACK" };

static void build(void) {
    for (int g = 0; g < 4; g++) {                            /* ground blocks: BLK strips of BLK quads, 4 textures */
        dl_start(dl_ground[g]);
        dl_cmd(G_POLYATTR, 1, 0xC0 | (31 << 16));
        dl_cmd(G_TEXPARAM, 1, g & 1 ? texparam(TEX_GRASS, 3, 3, 3, 3 << 16) : texparam(TEX_C4X4, 5, 4, 4, 3 << 16));
        dl_cmd(G_PLTT, 1, (g & 1 ? PAL_GRASS : PAL_C4X4) >> 4);
        dl_cmd(G_COLOR, 1, g & 2 ? RGB(28, 31, 26) : 0x7FFF);
        for (int r = 0; r < BLK; r++) {
            dl_cmd(G_BEGIN, 1, 3);
            for (int c = 0; c <= BLK; c++) {
                s32 x = c * BLKU - BLK * BLKU / 2;
                for (int k = 0; k < 2; k++) {
                    s32 z = (r + k) * BLKU - BLK * BLKU / 2;
                    dl_cmd(G_TEXCOORD, 1, tc(c * 32, (r + k) * 32));
                    dl_cmd(G_VTXXZ, 1, ((u32)(x >> 1) & 0xFFFF) | ((u32)(z >> 1) << 16));   /* half: blocks scaled x2 */
                }
            }
        }
        g_ground[g] = dl_finish();
    }
    dl_start(dl_house);                                      /* house: 4x4-compressed walls + a roof */
    dl_cmd(G_POLYATTR, 1, 0xC0 | (31 << 16) | (1 << 24));
    dl_cmd(G_DIFAMB, 1, 0x7FFF | (1 << 15) | (0x318Cu << 16));
    dl_cmd(G_PLTT, 1, PAL_C4X4 >> 4);
    mesh_box(FX(-1.5), 0, FX(-1.2), FX(1.5), FX(2), FX(1.2), texparam(TEX_C4X4, 5, 4, 4, 3 << 16), 64, 0);
    dl_cmd(G_TEXPARAM, 1, 0); dl_cmd(G_BEGIN, 1, 0);
    for (int s = 0; s < 2; s++) {
        dl_cmd(G_COLOR, 1, RGB(24, 6, 4));
        vtx16(FX(-1.7), FX(2), s ? FX(1.4) : FX(-1.4)); vtx16(FX(1.7), FX(2), s ? FX(1.4) : FX(-1.4)); vtx16(0, FX(3.2), 0);
    }
    g_house = dl_finish();
    dl_start(dl_lines);                                      /* outlines: line polygons (two equal vertices) */
    dl_cmd(G_POLYATTR, 1, 0xC0 | (31 << 16) | (2 << 24));
    dl_cmd(G_TEXPARAM, 1, 0); dl_cmd(G_COLOR, 1, 0); dl_cmd(G_BEGIN, 1, 0);
    static const s8 e[4][2] = { {-1,-1}, {1,-1}, {1,1}, {-1,1} };
    for (int i = 0; i < 4; i++) {
        const s8 *a = e[i], *b = e[(i + 1) & 3];
        vtx16(a[0] * FX(1.5), FX(2), a[1] * FX(1.2)); vtx16(b[0] * FX(1.5), FX(2), b[1] * FX(1.2)); vtx16(b[0] * FX(1.5), FX(2), b[1] * FX(1.2));
    }
    dl_cmd(G_COLOR, 1, 0x7FFF);
    g_lines = dl_finish();
    dl_start(dl_bill);                                       /* billboard quad (NPC, tree, player): texture set before */
    dl_cmd(G_BEGIN, 1, 1);
    dl_cmd(G_TEXCOORD, 1, tc(0, 32)); vtx16(FX(-0.5), 0, 0);
    dl_cmd(G_TEXCOORD, 1, tc(16, 32)); vtx16(FX(0.5), 0, 0);
    dl_cmd(G_TEXCOORD, 1, tc(16, 0)); vtx16(FX(0.5), FX(2), 0);
    dl_cmd(G_TEXCOORD, 1, tc(0, 0)); vtx16(FX(-0.5), FX(2), 0);
    g_bill = dl_finish();
}

static void load_chunks(int full) {                          /* the 3x3 chunks around (blk_x, blk_z); a row on a step */
    for (int j = 0; j < 3; j++) for (int i = 0; i < 3; i++)
        if (full || j == 2) { card_read(chunk[j][i], ROM_MAP((u32)((blk_x + i) * 7 + (blk_z + j) * 3) & 31), 0x2000); loads++; }
    for (int i = 0; i < 32 * 32; i++) colmap[i] = chunk[1][1][64 + i] & 1;
}

static void town_enter(void) {
    rnd_seed(0x70C1);
    texset_load(1); tex_cur = 1;
    overlay_load(0); ovl_cur = 0;
    build();
    nnpc = 1 + (s32)level * (ohg ? 8 : 3); if (nnpc > MAXN) nnpc = MAXN;
    ntree = (s32)level * 4;
    for (int i = 0; i < MAXN; i++) npc[i] = (npc_t){ (s32)(rnd() % 32) << 12, (s32)(rnd() % 32) << 12, 0, 0, (s32)(rnd() & 3), 0, 0, i };
    pl_x = 16 << 12; pl_z = 16 << 12; pl_dir = 0; blk_x = 0; blk_z = 0; door_t = 0; fade = 0; msg_pos = 0; msg_n = 0; loads = 0; glyph_reads = 0;
    load_chunks(1); vm_init(2, 0x3333); vm_init(3, 0x4444); vmh = 0;
    DISP3DCNT = 1 | (1 << 3) | (1 << 4);
    CLEAR_COLOR = RGB(8, 12, 8) | (31u << 16) | (63u << 24); CLEAR_DEPTH = 0x7FFF; VIEWPORT = 0 | (0 << 8) | (255u << 16) | (191u << 24);
    MTX_MODE = 0; MTX_IDENT = 0; for (int k = 0; k < 16; k++) MTX_LOAD44 = (u32)proj_town[k];
    MTX_MODE = 2; MTX_IDENT = 0;
    LIGHT_VEC = (u32)(0x3FF & -100) | ((u32)(0x3FF & -450) << 10) | ((u32)(0x3FF & -100) << 20);
    LIGHT_COL = 0x7FFF;
    DISPCNT(0) = 0x10000 | (1 << 3) | (1 << 8) | (1 << 9) | (1 << 12) | (1 << 4);
    BGCNT(0, 0) = 1; BGCNT(0, 1) = 0 | (2 << 2) | (30 << 8);
    volatile u16 *m = (volatile u16 *)0x0600F000;            /* text box: 28x4 cells at rows 18..21 */
    for (int i = 0; i < 1024; i++) m[i] = 0;
    for (int l = 0; l < 2; l++) for (int c = 0; c < 28; c++) for (int k = 0; k < 2; k++)
        m[(18 + l * 2 + k) * 32 + 2 + c] = (u16)(1 + (l * 28 + c) * 2 + k);
    for (int i = 0; i < 16; i++) PAL_BG(0)[i] = i ? RGB(31, 31, 31) : 0;
    PAL_BG(0)[2] = RGB(4, 4, 10);
    DISPCNT(1) = 0x10000 | 0 | (1 << 8) | (1 << 9) | (1 << 11) | (1 << 12) | (1 << 4);
    BGCNT(1, 1) = 1 | (2 << 2) | (26 << 8); BGCNT(1, 3) = 3 | (2 << 2) | (28 << 8);
    volatile u16 *ch = (volatile u16 *)0x06208000;
    for (int i = 0; i < 0x2000; i++) ch[i] = (u16)((i & 7) == 0 ? 0x1111 : 0x2323 + (i >> 9));
    volatile u16 *bm = (volatile u16 *)(0x06200000 + 26 * 0x800);
    for (int i = 0; i < 2048; i++) bm[i] = (u16)((i % 7 == 0 ? 3 : 1 + (i & 1)) | (1 << 12));
    volatile u16 *b3 = (volatile u16 *)(0x06200000 + 28 * 0x800);
    for (int i = 0; i < 1024; i++) b3[i] = (u16)(((i ^ (i >> 5)) & 3) + 5 | (2 << 12));
    for (int i = 16; i < 48; i++) PAL_BG(1)[i] = (u16)RGB(i & 31, 20, 31 - (i & 15));
    volatile u16 *ob = (volatile u16 *)0x06600000;
    for (int i = 0; i < 4096; i++) ob[i] = (u16)(0x5555 + (i & 0x0F0F));
    for (int i = 0; i < 16; i++) PAL_OBJ(1)[i] = RGB(31 - i, 16 + (i >> 1), i * 2);
    fifo_send((4u << 28) | 2);
}

static void render_glyph(int line, int cell, int ch) {      /* 3x5 glyph at 2x into a cell's two 4bpp tiles */
    u32 *t = text_tiles[line][cell];
    int g = ch - 32; if (g < 0 || g > 63) g = 0;
    for (int r = 0; r < 16; r++) {
        int fy = (r - 2) / 2; u32 row = 0x22222222;          /* background colour 2 */
        if (r >= 2 && fy < 5)
            for (int x = 0; x < 6; x++) if (font3x5[g] >> ((4 - fy) * 3 + (2 - x / 2)) & 1) row = (row & ~(0xFu << ((x + 1) * 4))) | (1u << ((x + 1) * 4));
        t[r] = row;
    }
}

static void walk(u32 f) {
    static const s8 dx[4] = { 1, 0, -1, 0 }, dz[4] = { 0, 1, 0, -1 };
    if (f % 90 == 0) pl_dir = (pl_dir + 1 + (s32)(rnd() % 3 == 0)) & 3;
    pl_x += dx[pl_dir] * 120; pl_z += dz[pl_dir] * 120 + 40;
    s32 bx = pl_x >> 15, bz = pl_z >> 15;                    /* a block = 8 tiles of 4096 */
    if (bx != blk_x || bz != blk_z) { blk_x = bx; blk_z = bz; load_chunks(0); }
    cam_x += (pl_x - cam_x) >> 3; cam_z += (pl_z - cam_z) >> 3;
}

static void draw(u32 f) {
    MTX_IDENT = 0;
    gx_rot_x(100);
    gx_trans(-(cam_x >> 1) * 2, FX(-9), -(cam_z >> 1) * 2 - FX(8));
    s32 cbx = (cam_x >> 15), cbz = (cam_z >> 15);
    for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) {
        s32 ox = (cbx + i) * BLK * BLKU + BLK * BLKU / 2, oz = (cbz + j) * BLK * BLKU + BLK * BLKU / 2;
        const u8 *c = chunk[j + 1][i + 1];
        MTX_PUSH = 0; gx_trans(ox, 0, oz); MTX_SCALE = 2 << 12; MTX_SCALE = 1 << 12; MTX_SCALE = 2 << 12;
        gx_send(g_ground[c[0] & 3]); MTX_POP = 1;
        for (int h = 0; h < 2; h++) {
            s32 hx = ox + ((s32)(c[1 + h] & 7) - 3) * FX(3), hz = oz + ((s32)(c[3 + h] & 7) - 3) * FX(3);
            MTX_PUSH = 0; gx_trans(hx, 0, hz); gx_send(g_house); gx_send(g_lines); MTX_POP = 1;
        }
    }
    MTX_IDENT = 0; gx_rot_x(100); gx_trans(-(cam_x >> 1) * 2, FX(-9), -(cam_z >> 1) * 2 - FX(8));
    POLY_ATTR = 0xC0 | (31 << 16) | (3 << 24);
    PLTT_BASE = PAL_NPC >> 4;
    for (int i = 0; i < nnpc; i++) {                         /* NPCs: their frame's texture */
        npc_t *n = &npc[i];
        MTX_PUSH = 0; gx_trans((n->x - (16 << 12)) * 3 / 2 + (cbx * BLK * BLKU), 0, (n->z - (16 << 12)) * 3 / 2 + (cbz * BLK * BLKU));
        TEX_PARAM = texparam(TEX_NPC + (u32)((n->anim >> 3) & 7) * 256, 3, 1, 2, 1u << 29);
        GX_COLOR = 0x7FFF; gx_send(g_bill);
        if (ohg) {                                           /* Origin HG: a water reflection under each (flipped, translucent) */
            POLY_ATTR = 0xC0 | (12 << 16) | (9 << 24); MTX_SCALE = 4096; MTX_SCALE = (u32)-4096; MTX_SCALE = 4096;
            gx_send(g_bill); POLY_ATTR = 0xC0 | (31 << 16) | (3 << 24);
        }
        MTX_POP = 1;
    }
    TEX_PARAM = texparam(TEX_TREE, 1, 2, 3, 0); PLTT_BASE = PAL_TREE >> 4;
    for (int i = 0; i < ntree; i++) {
        s32 tx = ((s32)(i * 37 % 24) - 12) * FX(2) + cbx * BLK * BLKU, tz = ((s32)(i * 53 % 24) - 12) * FX(2) + cbz * BLK * BLKU;
        MTX_PUSH = 0; gx_trans(tx, 0, tz); MTX_SCALE = 3 << 12; MTX_SCALE = 3 << 12; MTX_SCALE = 3 << 12; gx_send(g_bill); MTX_POP = 1;
    }
    TEX_PARAM = texparam(TEX_NPC + ((f >> 3) & 1) * 256, 3, 1, 2, 1u << 29); PLTT_BASE = PAL_NPC >> 4;
    MTX_PUSH = 0; gx_trans((pl_x >> 1) * 2, 0, (pl_z >> 1) * 2); gx_send(g_bill); MTX_POP = 1;
    if (ohg) {                                               /* the following Pokemon, a step behind */
        TEX_PARAM = texparam(TEX_NPC + (4 + ((f >> 3) & 3)) * 256, 3, 1, 2, 1u << 29);
        MTX_PUSH = 0; gx_trans((pl_x >> 1) * 2 - FX(1.5), 0, (pl_z >> 1) * 2 - FX(1.5)); MTX_SCALE = 3 << 11; MTX_SCALE = 3 << 11; MTX_SCALE = 3 << 11; gx_send(g_bill); MTX_POP = 1;
    }
}

static void town_frame(u32 f) {
    u32 t0 = timer_now();
    walk(f);
    OVL->npc_update(npc, nnpc, f, colmap);
    vmh ^= vm_run(2, (int)level * 900, chunk[1][1]) ^ vm_run(3, (int)level * 500, colmap);
    prof[0] = timer_now() - t0;
    if (f % (ohg ? 90 : 180) == 0) { msg_n = (msg_n + 1) & 3; msg_pos = 0; for (int l = 0; l < 2; l++) for (int c = 0; c < 28; c++) render_glyph(l, c, ' '); }
    /* the text printer: vanilla one glyph every other frame; Origin HG's FAST up to three a frame, each glyph's font
     * data read lazily from the card (the hack's NARC_ReadFromAbsOffset per glyph: 7-9 of a glyph's 9-12 lines) */
    for (int k = 0, n = ohg ? 3 : ((f & 1) ? 1 : 0); k < n && msg_pos < 56; k++) {
        const char *s = msgs[msg_n]; u32 p = msg_pos++; char ch = p < 40 ? s[p] : ' ';
        if (ohg) { card_read(glyph_buf, ROM_MAP((u32)ch & 31) + ((u32)ch & 0xF) * 0x200, 0x200); glyph_reads++; }
        render_glyph((int)(p / 28), (int)(p % 28), ch);
        if (ch != ' ' && (p & 3) == 0) fifo_send((3u << 28) | 12 | (64 << 8) | (30 << 16));
    }
    if (ohg && f % 1500 >= 600) vmh ^= vm_run(2, (int)level * 2200, chunk[1][1]);   /* the busy town: more scripts run */
    for (int i = 0; i < 4; i++) {                            /* this frame's NPC animation frames to upload */
        int k = (int)((f + (u32)i) % 8);
        for (int w = 0; w < 128; w++) npc_up[i][w] = (u16)(0x1111 * (u32)((w + k * 3 + i) & 15) ^ (u16)(f << 4));
    }
    u32 t1 = timer_now();
    draw(f);
    prof[1] = timer_now() - t1;
    SWAP_BUF = 0;
    for (int i = 0; i < 128 * 4; i += 4) oamb[i] = 0x0200;
    for (int i = 0; i < 8; i++) {                            /* menu icons, bobbing */
        oamb[i * 4] = (u16)((40 + (i >> 2) * 64 + (sn((s32)f * 8 + i * 128) >> 10)) & 255); oamb[i * 4 + 1] = (u16)((24 + (i & 3) * 56) | (2 << 14));
        oamb[i * 4 + 2] = (u16)(i * 16 | (1 << 12));
    }
    oamb[32] = (u16)(100 | (1 << 8) | (1 << 9)); oamb[33] = (u16)(100 | (2 << 14)); oamb[34] = 0;
    /* doors: fade out, load, fade in */
    door_t++;
    if (door_t == 464) fade = 1;
    if (fade > 0 && fade < 16) fade++;
    if (fade == 16) {
        ovl_cur ^= 1; overlay_load(ovl_cur);
        tex_cur = tex_cur == 2 ? 3 : 2; texset_load(tex_cur);
        load_chunks(1);
        fade = -16; door_t = 0;
    }
    if (fade < 0) fade++;
}

static void town_vblank(u32 f) {
    VRAMCNT(0) = 0x80;                                       /* NPC frames into texture VRAM (bank A in the LCDC) */
    for (int i = 0; i < 4; i++) dma_copy32((void *)(VRAM_LCDC(0) + TEX_NPC + (u32)((f + (u32)i) % 8) * 256), npc_up[i], 256);
    VRAMCNT(0) = 0x83;
    dma_copy32((void *)(0x06008000 + 32), text_tiles, sizeof text_tiles);
    dma_copy32((void *)OAM(1), oamb, 1024);
    volatile s16 *aff = (volatile s16 *)0x07000400;
    s32 a = (s32)f * 6; aff[3] = (s16)(cs(a) >> 4); aff[7] = (s16)(-sn(a) >> 4); aff[11] = (s16)(sn(a) >> 4); aff[15] = (s16)(cs(a) >> 4);
    BGHOFS(1, 3) = (u16)f; BGVOFS(1, 3) = (u16)(f >> 1);
    s32 b = fade > 0 ? fade : -fade;
    MASTER_BRIGHT(0) = b ? (u16)((2 << 14) | b) : 0; MASTER_BRIGHT(1) = MASTER_BRIGHT(0);
}

static void town_leave(void) { MASTER_BRIGHT(0) = 0; MASTER_BRIGHT(1) = 0; }
static u32 town_hash(void) {
    u32 h = OVL->hash(npc, nnpc);
    return h ^ vmh ^ (u32)pl_x ^ ((u32)pl_z << 7) ^ ((u32)loads << 20);
}

const scene_t scene_town = { "TOWN", town_enter, town_frame, town_vblank, town_leave, town_hash, 1 };
/* "ohg": Origin HeartGold's overworld (its FAQ / hardware notes, docs: originheartgold.github.io): the hack runs the
 * overworld at 60 fps, busier towns (more characters on screen, a following Pokemon, reflections), FAST text (three
 * glyphs a frame, a card read each); on a DS's 67 MHz ARM9 the busy part loses frames inside the game ("slowdown,
 * sound crackle ... the hack's 60 fps overworld: suggest DSi mode"). Its first 10 s are the route (the town's load),
 * the rest the busy town (more script work): the ROM's lag counter shows the game's own lost frames */
static void ohg_enter(void) { ohg = 1; town_enter(); }
static void ohg_leave(void) { town_leave(); ohg = 0; }
static u32 ohg_hash(void) { return town_hash() ^ glyph_reads; }
const scene_t scene_ohg = { "OHG", ohg_enter, town_frame, town_vblank, ohg_leave, ohg_hash, 1 };
