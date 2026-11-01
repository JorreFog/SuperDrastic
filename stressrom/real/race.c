/* dsreal scene 1, "race": a 3D kart race on the top screen with a 2D HUD over it, the course map on the bottom.
 * Models Mario Kart DS's race (MKDS race start, documented in bananads docs/PERF.md: ~1,330 polygons, ~900
 * BEGINs, ~28,900 GX FIFO entries a frame, display lists by the GX FIFO DMA, ~1,500 ARM9 calls a frame, fog,
 * translucent particles and item boxes). Level 5 aims at those numbers. */
#include "mesh.h"

#define NSEG 128
#define MAXK 16
#define MAXP 160
#define MAXB 32
#define MAXT 128
#define MAXH 40
typedef struct { s32 x, z, h, speed, u, lane, hit, item, place; } kart_t;
typedef struct { s32 x, y, z, vx, vy, vz, life; } part_t;
static kart_t kart[MAXK];
static part_t part[MAXP];
static s32 seg_x[NSEG], seg_z[NSEG], seg_h[NSEG];
static s32 tree_x[MAXT], tree_z[MAXT], box_x[MAXH], box_z[MAXH];
static s32 nk, nvis, ntree, npart, nbox, nhouse, cam_x, cam_z, cam_h;
static u32 dlbuf[12000];
static dl_t dl_road, dl_kart, dl_tree, dl_box, dl_part, dl_house, dl_sky, dl_shadow;
static u32 hits, laps, lapt, vmh;
static s32 ctri[NSEG][8];
static s32 ncol, nvm;
static u16 oam[128 * 4] __attribute__((aligned(4))), oamb[128 * 4] __attribute__((aligned(4)));
static s16 hud_rot;

static void track_pt(s32 u, s32 *x, s32 *z) {               /* u: 0..1023 a lap */
    *x = 56 * cs(u) + 14 * cs(3 * u);
    *z = 40 * sn(u) + 10 * sn(2 * u);
}

static void build_meshes(void) {
    u32 *b = dlbuf;
    /* road piece: grass L, 2 road strips, grass R, 2 rails; 12 polygons, 5 BEGINs */
    dl_start(b);
    u32 tp_road = texparam(TEX_ROAD, 7, 3, 3, 3 << 16), tp_grass = texparam(TEX_GRASS, 3, 3, 3, 3 << 16);
    dl_cmd(G_POLYATTR, 1, 0xC0 | (31 << 16) | (1u << 15));   /* both faces? no: back culled; fog on */
    dl_cmd(G_DIFAMB, 1, 0x7FFF | (1 << 15) | (0x318Cu << 16));
    dl_cmd(G_NORMAL, 1, nrm(0, 4088, 0));
    dl_cmd(G_TEXPARAM, 1, tp_grass); dl_cmd(G_PLTT, 1, PAL_GRASS >> 4);
    dl_cmd(G_BEGIN, 1, 1);
    for (int s = 0; s < 2; s++) {
        s32 x0 = s ? FX(4) : FX(-7), x1 = s ? FX(7) : FX(-4);
        dl_cmd(G_TEXCOORD, 1, tc(0, 0));  dl_cmd(G_VTXXZ, 1, ((u32)x0 & 0xFFFF) | 0);
        dl_cmd(G_TEXCOORD, 1, tc(0, 64)); dl_cmd(G_VTXXZ, 1, ((u32)x0 & 0xFFFF) | ((u32)FX(-2.1) << 16));
        dl_cmd(G_TEXCOORD, 1, tc(64, 64)); dl_cmd(G_VTXXZ, 1, ((u32)x1 & 0xFFFF) | ((u32)FX(-2.1) << 16));
        dl_cmd(G_TEXCOORD, 1, tc(64, 0)); dl_cmd(G_VTXXZ, 1, ((u32)x1 & 0xFFFF) | 0);
    }
    dl_cmd(G_TEXPARAM, 1, tp_road);
    for (int r = 0; r < 2; r++) {                            /* two quad strips of 4 lanes */
        dl_cmd(G_BEGIN, 1, 3);
        s32 z0 = r ? FX(-1.05) : 0, z1 = r ? FX(-2.1) : FX(-1.05);
        for (int i = 0; i <= 4; i++) {
            s32 x = FX(-4) + i * FX(2);
            dl_cmd(G_TEXCOORD, 1, tc(i * 16, r * 32)); vtx16(x, FX(0.01), z0);
            dl_cmd(G_TEXCOORD, 1, tc(i * 16, r * 32 + 32)); vtx16(x, FX(0.01), z1);
        }
    }
    dl_cmd(G_TEXPARAM, 1, texparam(TEX_C4X4, 5, 4, 4, 3 << 16)); dl_cmd(G_PLTT, 1, PAL_C4X4 >> 4);
    dl_cmd(G_BEGIN, 1, 1);
    for (int s = 0; s < 2; s++) {
        s32 x = s ? FX(4) : FX(-4);
        dl_cmd(G_TEXCOORD, 1, tc(0, 16));  dl_cmd(G_VTX10, 1, v10(x, 0, 0));
        dl_cmd(G_TEXCOORD, 1, tc(64, 16)); dl_cmd(G_VTXDIFF, 1, (u32)((FX(-2.1) >> 3) & 0x3FF) << 20);
        dl_cmd(G_TEXCOORD, 1, tc(64, 0));  dl_cmd(G_VTXXY, 1, ((u32)x & 0xFFFF) | ((u32)FX(0.5) << 16));
        dl_cmd(G_TEXCOORD, 1, tc(0, 0));   dl_cmd(G_VTXXY, 1, ((u32)x & 0xFFFF) | ((u32)FX(0.5) << 16));
    }
    dl_cmd(G_END, 0); dl_cmd(G_COLOR, 1, 0x7FFF);
    dl_road = dl_finish(); b += dl_road.n;

    /* kart: body, cockpit, spoiler, 4 wheels, driver, flames: ~52 polygons, 9 BEGINs */
    dl_start(b);
    dl_cmd(G_POLYATTR, 1, 0xC0 | (31 << 16) | (1u << 15));
    dl_cmd(G_DIFAMB, 1, 0x7FFF | (1 << 15) | (0x2108u << 16));
    dl_cmd(G_PLTT, 1, PAL_BODY >> 4);
    mesh_box(FX(-0.6), FX(0.2), FX(-1.0), FX(0.6), FX(0.6), FX(1.0), texparam(TEX_BODY, 4, 3, 3, 0), 64, 0);
    mesh_box(FX(-0.9), FX(0.5), FX(0.8), FX(0.9), FX(0.6), FX(1.1), texparam(TEX_BODY, 4, 3, 3, 0), 32, 1);
    for (int w = 0; w < 4; w++) {
        s32 x = (w & 1) ? FX(0.6) : FX(-0.9), z = (w & 2) ? FX(0.5) : FX(-0.9);
        dl_cmd(G_PLTT, 1, PAL_C4X4 >> 4);
        mesh_box(x, 0, z, x + FX(0.3), FX(0.4), z + FX(0.4), texparam(TEX_C4X4, 5, 4, 4, 0), 32, w & 1);
    }
    dl_cmd(G_TEXPARAM, 1, 0);                                /* driver: untextured octahedron, lit */
    dl_cmd(G_BEGIN, 1, 0);
    static const s8 oc[6][3] = { {0,1,0}, {1,0,0}, {0,0,1}, {-1,0,0}, {0,0,-1}, {0,-1,0} };
    for (int i = 0; i < 8; i++) {
        int a = 1 + (i & 3), c = 1 + ((i + 1) & 3), top = i < 4 ? 0 : 5;
        int v3[3] = { top, i < 4 ? a : c, i < 4 ? c : a };
        for (int k = 0; k < 3; k++) {
            const s8 *p = oc[v3[k]];
            dl_cmd(G_NORMAL, 1, nrm(p[0] * 4088, p[1] * 4088, p[2] * 4088));
            vtx16(p[0] * FX(0.3), FX(0.95) + p[1] * FX(0.3), p[2] * FX(0.3));
        }
    }
    dl_cmd(G_POLYATTR, 1, 0x80 | (12 << 16) | (2 << 24));    /* flames: translucent, own poly id */
    dl_cmd(G_TEXPARAM, 1, texparam(TEX_SMOKE, 1, 2, 2, 0)); dl_cmd(G_PLTT, 1, PAL_SMOKE >> 4);
    dl_cmd(G_BEGIN, 1, 1);
    for (int s = 0; s < 2; s++) {
        s32 x = s ? FX(0.3) : FX(-0.5);
        dl_cmd(G_COLOR, 1, 0x03FF);
        dl_cmd(G_TEXCOORD, 1, tc(0, 0));  vtx16(x, FX(0.2), FX(1.0));
        dl_cmd(G_TEXCOORD, 1, tc(32, 0)); vtx16(x + FX(0.2), FX(0.2), FX(1.0));
        dl_cmd(G_TEXCOORD, 1, tc(32, 32)); vtx16(x + FX(0.2), FX(0.4), FX(1.5));
        dl_cmd(G_TEXCOORD, 1, tc(0, 32)); vtx16(x, FX(0.4), FX(1.5));
    }
    dl_cmd(G_END, 0); dl_cmd(G_COLOR, 1, 0x7FFF);
    dl_kart = dl_finish(); b += dl_kart.n;

    /* tree: crossed alpha-tested billboards (A3I5) */
    dl_start(b);
    dl_cmd(G_POLYATTR, 1, 0xC0 | (31 << 16) | (3 << 24) | (1u << 15));
    dl_cmd(G_TEXPARAM, 1, texparam(TEX_TREE, 1, 2, 3, 0)); dl_cmd(G_PLTT, 1, PAL_TREE >> 4);
    dl_cmd(G_COLOR, 1, 0x7FFF);
    dl_cmd(G_BEGIN, 1, 1);
    for (int s = 0; s < 2; s++) {
        dl_cmd(G_TEXCOORD, 1, tc(0, 64)); vtx16(s ? 0 : FX(-1.5), 0, s ? FX(-1.5) : 0);
        dl_cmd(G_TEXCOORD, 1, tc(32, 64)); vtx16(s ? 0 : FX(1.5), 0, s ? FX(1.5) : 0);
        dl_cmd(G_TEXCOORD, 1, tc(32, 0)); vtx16(s ? 0 : FX(1.5), FX(4), s ? FX(1.5) : 0);
        dl_cmd(G_TEXCOORD, 1, tc(0, 0)); vtx16(s ? 0 : FX(-1.5), FX(4), s ? FX(-1.5) : 0);
    }
    dl_tree = dl_finish(); b += dl_tree.n;

    /* item box: translucent A5I3 cube */
    dl_start(b);
    dl_cmd(G_POLYATTR, 1, 0xC0 | (16 << 16) | (4 << 24) | (1u << 15));
    dl_cmd(G_PLTT, 1, PAL_GLASS >> 4);
    mesh_box(FX(-0.5), FX(0.3), FX(-0.5), FX(0.5), FX(1.3), FX(0.5), texparam(TEX_GLASS, 6, 2, 2, 0), 32, 1);
    dl_box = dl_finish(); b += dl_box.n;

    /* particle: one translucent quad */
    dl_start(b);
    dl_cmd(G_POLYATTR, 1, 0xC0 | (10 << 16) | (5 << 24));
    dl_cmd(G_TEXPARAM, 1, texparam(TEX_SMOKE, 1, 2, 2, 0)); dl_cmd(G_PLTT, 1, PAL_SMOKE >> 4);
    dl_cmd(G_BEGIN, 1, 1);
    dl_cmd(G_TEXCOORD, 1, tc(0, 0)); vtx16(FX(-0.4), FX(-0.4), 0);
    dl_cmd(G_TEXCOORD, 1, tc(32, 0)); vtx16(FX(0.4), FX(-0.4), 0);
    dl_cmd(G_TEXCOORD, 1, tc(32, 32)); vtx16(FX(0.4), FX(0.4), 0);
    dl_cmd(G_TEXCOORD, 1, tc(0, 32)); vtx16(FX(-0.4), FX(0.4), 0);
    dl_part = dl_finish(); b += dl_part.n;

    /* house beside the course: 4x4-compressed box + roof */
    dl_start(b);
    dl_cmd(G_POLYATTR, 1, 0xC0 | (31 << 16) | (1u << 15));
    dl_cmd(G_PLTT, 1, PAL_C4X4 >> 4);
    mesh_box(FX(-2), 0, FX(-2), FX(2), FX(3), FX(2), texparam(TEX_C4X4, 5, 4, 4, 3 << 16), 128, 0);
    dl_cmd(G_TEXPARAM, 1, 0); dl_cmd(G_BEGIN, 1, 0);
    for (int s = 0; s < 2; s++) {
        dl_cmd(G_COLOR, 1, 0x001F | (s * 0x0200));
        vtx16(FX(-2.2), FX(3), s ? FX(2.2) : FX(-2.2)); vtx16(FX(2.2), FX(3), s ? FX(2.2) : FX(-2.2)); vtx16(0, FX(4.5), 0);
    }
    dl_cmd(G_COLOR, 1, 0x7FFF);
    dl_house = dl_finish(); b += dl_house.n;

    /* kart shadow: translucent black quad on the road */
    dl_start(b);
    dl_cmd(G_POLYATTR, 1, 0xC0 | (14 << 16) | (6 << 24));
    dl_cmd(G_TEXPARAM, 1, 0); dl_cmd(G_COLOR, 1, 0);
    dl_cmd(G_BEGIN, 1, 1);
    vtx16(FX(-0.8), FX(0.02), FX(-1.2)); vtx16(FX(0.8), FX(0.02), FX(-1.2)); vtx16(FX(0.8), FX(0.02), FX(1.2)); vtx16(FX(-0.8), FX(0.02), FX(1.2));
    dl_cmd(G_COLOR, 1, 0x7FFF);
    dl_shadow = dl_finish(); b += dl_shadow.n;

    /* backdrop: 4 far quads, fog off */
    dl_start(b);
    dl_cmd(G_POLYATTR, 1, 0xC0 | (31 << 16) | (7 << 24));
    dl_cmd(G_TEXPARAM, 1, 0);
    dl_cmd(G_BEGIN, 1, 1);
    for (int s = 0; s < 4; s++) {
        s32 a = s * 256, c0 = cs(a) * 7, s0 = sn(a) * 7, c1 = cs(a + 256) * 7, s1 = sn(a + 256) * 7;
        dl_cmd(G_COLOR, 1, RGB(10, 18, 31)); vtx16(c0, FX(-1), s0); vtx16(c1, FX(-1), s1);
        dl_cmd(G_COLOR, 1, RGB(24, 28, 31)); vtx16(c1, FX(5), s1); vtx16(c0, FX(5), s0);
    }
    dl_sky = dl_finish();
}

static void race_enter(void) {
    rnd_seed(0x5ACE);
    nk = 2 + (level * 6 + 2) / 5; if (nk > MAXK) nk = MAXK;
    nvis = 15 + level * 7;
    ntree = level * 16; if (ntree > MAXT) ntree = MAXT;
    npart = level * 18; if (npart > MAXP) npart = MAXP;
    nbox = 2 + level * 2;
    nhouse = level * 6; if (nhouse > MAXH) nhouse = MAXH;
    ncol = 8 + level * 8; nvm = level * 1300;
    texset_load(0);
    build_meshes();
    for (int i = 0; i < NSEG; i++) {
        s32 u = i * 1024 / NSEG, x, z, x2, z2;
        track_pt(u, &x, &z); track_pt(u + 8, &x2, &z2);
        seg_x[i] = x; seg_z[i] = z; seg_h[i] = atan2i(z2 - z, x2 - x);
    }
    for (int i = 0; i < MAXT; i++) {
        int s = (int)(rnd() % NSEG); s32 side = (rnd() & 1) ? 9 : -9, h = seg_h[s] + 256;
        tree_x[i] = seg_x[s] + side * cs(h) + (s32)(rnd() & 0x3FFF); tree_z[i] = seg_z[s] + side * sn(h);
    }
    for (int i = 0; i < MAXH; i++) {
        int s = (int)(rnd() % NSEG); s32 side = (rnd() & 1) ? 11 : -11, h = seg_h[s] + 256;
        box_x[i] = seg_x[s] + side * cs(h); box_z[i] = seg_z[s] + side * sn(h);
    }
    for (int k = 0; k < MAXK; k++) { kart[k].u = (s32)(k * 6) << 8; kart[k].lane = (k % 4) * 2 - 3; kart[k].speed = 300 + k * 7; kart[k].hit = 0; kart[k].item = 0;
        track_pt(kart[k].u >> 8, &kart[k].x, &kart[k].z); kart[k].h = seg_h[(k * 6 * NSEG >> 10) & (NSEG - 1)]; kart[k].place = k; }
    for (int i = 0; i < MAXP; i++) part[i] = (part_t){ 0, 0, 0, 0, 0, 0, 0 };
    hits = 0; laps = 0; lapt = 0; hud_rot = 0; vmh = 0;
    for (int i = 0; i < NSEG; i++) {                          /* the course's wall triangles (one a piece) */
        s32 h = seg_h[i] + 256; ctri[i][0] = cs(h); ctri[i][1] = 0; ctri[i][2] = sn(h); ctri[i][3] = (s32)(((s64)cs(h) * seg_x[i] + (s64)sn(h) * seg_z[i]) >> 12) + FX(4);
        ctri[i][4] = seg_x[i] - FX(2); ctri[i][5] = seg_z[i]; ctri[i][6] = seg_x[i] + FX(2); ctri[i][7] = seg_z[i] + FX(2);
    }
    vm_init(0, 0x1111); vm_init(1, 0x2222);

    /* 3D: fog, alpha blend, anti-alias, textures */
    DISP3DCNT = 1 | (1 << 3) | (1 << 4) | (1 << 7);
    FOG_COLOR = RGB(20, 24, 31) | (31u << 16); FOG_OFFSET = 0x6000;
    for (int i = 0; i < 32; i++) FOG_TABLE(i) = (u8)(i * 4 > 127 ? 127 : i * 4);
    CLEAR_COLOR = RGB(20, 24, 31) | (31u << 16) | (63u << 24); CLEAR_DEPTH = 0x7FFF; VIEWPORT = 0 | (0 << 8) | (255u << 16) | (191u << 24);
    MTX_MODE = 0; MTX_IDENT = 0; for (int k = 0; k < 16; k++) MTX_LOAD44 = (u32)proj_race[k];
    MTX_MODE = 2; MTX_IDENT = 0;
    LIGHT_VEC = (u32)(0x3FF & -200) | ((u32)(0x3FF & -380) << 10) | ((u32)(0x3FF & -150) << 20);
    LIGHT_COL = 0x7FFF; SPE_EMI = 0;
    /* 2D: engine A BG0 = 3D (priority 1), BG1 = HUD text (0), OBJ; engine B BG0 text, BG2 affine course map */
    DISPCNT(0) = 0x10000 | (1 << 3) | (1 << 8) | (1 << 9) | (1 << 12) | (1 << 4);
    BGCNT(0, 0) = 1; BGCNT(0, 1) = 0 | (31 << 8);
    DISPCNT(1) = 0x10000 | 1 | (1 << 8) | (1 << 10) | (1 << 12) | (1 << 4);
    BGCNT(1, 2) = 1 | (1 << 2) | (1 << 7) | (30 << 8) | (1 << 13);
    /* course map: 8bpp tiles drawn from the track, 32x32 map */
    volatile u16 *ch = (volatile u16 *)0x06204000, *mp = (volatile u16 *)0x0620F000;
    for (int t = 0; t < 128; t++) for (int i = 0; i < 32; i++) ch[t * 32 + i] = (u16)(((t * 7 + i) & 0x3F) | ((t * 3 + i) & 0x3F) << 8);
    for (int i = 0; i < 512; i++) mp[i] = (u16)((i * 37 & 0x7F) | ((i * 37 + 11) & 0x7F) << 8);
    for (int i = 0; i < 256; i++) PAL_BG(1)[i] = (u16)(i == 0 ? 0 : RGB(i & 31, (i * 3) & 31, 8 + (i >> 4)));
    PAL_BG(1)[1] = 0x7FFF;
    /* OBJ tiles: engine A (bank G at 0x06400000), engine B (bank I at 0x06600000) */
    volatile u16 *oa = (volatile u16 *)0x06400000, *ob = (volatile u16 *)0x06600000;
    for (int i = 0; i < 4096; i++) { oa[i] = (u16)(0x1111 * (1 + ((i >> 4) & 7))); ob[i] = (u16)(0x2222 + (i & 0x1111)); }
    for (int i = 0; i < 16; i++) { PAL_OBJ(0)[i] = RGB(i * 2, 31 - i, 16); PAL_OBJ(1)[i] = RGB(31, i * 2, i); }
    fifo_send((4u << 28) | 0);                               /* song 0 */
}

static void kart_logic(void) {
    for (int k = 0; k < nk; k++) {
        kart_t *K = &kart[k];
        s32 u = K->u >> 8, tx, tz, nx, nz;
        track_pt(u + 6, &tx, &tz);
        s32 h = seg_h[(u * NSEG >> 10) & (NSEG - 1)];
        tx += K->lane * cs(h + 256) / 2; tz += K->lane * sn(h + 256) / 2;
        s32 want = atan2i(tz - K->z, tx - K->x), d = ((want - K->h + 512) & 1023) - 512;
        K->h = (K->h + (d > 24 ? 24 : d < -24 ? -24 : d)) & 1023;
        s32 sp = K->speed + (K->hit ? -120 : 0) + (s32)(rnd() & 31);
        K->u += sp >> 2;
        track_pt(K->u >> 8, &nx, &nz);
        K->x = nx + K->lane * cs(h + 256) / 2; K->z = nz + K->lane * sn(h + 256) / 2;
        if (K->hit) K->hit--;
        s32 push = 0, s0 = ((u * NSEG >> 10) - ncol / 2) & (NSEG - 1);
        int n0 = s0 + ncol > NSEG ? NSEG - s0 : ncol;
        if (tri_collide(ctri[s0], n0, K->x, FX(0.5), K->z, &push) + tri_collide(ctri[0], ncol - n0, K->x, FX(0.5), K->z, &push)) K->lane += push > 0 ? -1 : 1;
        if (K->lane > 3) K->lane = 3; if (K->lane < -3) K->lane = -3;
        /* item boxes on the course */
        for (int i = 0; i < nbox; i++) {
            s32 bu = (i * 1024 / nbox) & 1023, bx, bz;
            track_pt(bu, &bx, &bz);
            s32 dx = (K->x - bx) >> 6, dz = (K->z - bz) >> 6;
            if (dx * dx + dz * dz < 4096 * 2 && !K->item) { K->item = 1 + (s32)(rnd() % 8); hits++; if (k == 0) fifo_send((3u << 28) | 5 | (64 << 8) | (90 << 16)); }
        }
        if (K->item && (rnd() & 255) == 0) {                /* use it: a burst of smoke */
            K->item = 0;
            for (int i = 0, n = 0; i < npart && n < 12; i++) if (!part[i].life) {
                part[i] = (part_t){ K->x, FX(0.5), K->z, (s32)(rnd() & 511) - 256, (s32)(rnd() & 255), (s32)(rnd() & 511) - 256, 40 + (s32)(rnd() & 31) }; n++;
            }
            fifo_send((3u << 28) | 2 | (32 << 8) | (70 << 16));
        }
    }
    /* kart against kart: distance through the SQRT unit, push apart */
    for (int a = 0; a < nk; a++) for (int b = a + 1; b < nk; b++) {
        s32 dx = (kart[a].x - kart[b].x) >> 4, dz = (kart[a].z - kart[b].z) >> 4;
        if (dx > 1024 || dx < -1024 || dz > 1024 || dz < -1024) continue;
        u32 d = hw_sqrt((u32)(dx * dx + dz * dz));
        if (d < 160) { kart[a].hit = 20; kart[b].lane = -kart[b].lane; s32 p = hw_div(160 - (s32)d, 4); kart[a].u += p; }
    }
    /* places: by distance along the course (insertion sort, as games keep a ranking) */
    static s8 order[MAXK];
    for (int k = 0; k < nk; k++) order[k] = (s8)k;
    for (int i = 1; i < nk; i++) { s8 t = order[i]; int j = i - 1; while (j >= 0 && kart[(int)order[j]].u < kart[(int)t].u) { order[j + 1] = order[j]; j--; } order[j + 1] = t; }
    for (int i = 0; i < nk; i++) kart[(int)order[i]].place = i;
    /* particles: idle ones respawn as tyre smoke behind karts */
    for (int i = 0; i < npart; i++) {
        part_t *p = &part[i];
        if (!p->life) {
            if ((rnd() & 7) == 0) { kart_t *K = &kart[i % nk]; *p = (part_t){ K->x, FX(0.3), K->z, (s32)(rnd() & 127) - 64, 40, (s32)(rnd() & 127) - 64, 24 + (s32)(rnd() & 15) }; }
            continue;
        }
        p->x += p->vx; p->y += p->vy; p->z += p->vz; p->vy -= 2; p->life--;
    }
    if (++lapt >= 60 * 30) { lapt = 0; laps++; }
    vmh ^= vm_run(0, nvm / 2, (const u8 *)part) ^ vm_run(1, nvm / 2, (const u8 *)ctri);
}

static void draw3d(u32 f) {
    kart_t *C = &kart[0];
    cam_h = C->h; cam_x = C->x - 7 * cs(cam_h); cam_z = C->z - 7 * sn(cam_h);
    MTX_IDENT = 0;
    gx_rot_x(40);
    gx_rot_y(cam_h + 256);
    gx_trans(-cam_x, FX(-2.6), -cam_z);
    s32 base = (C->u >> 8) * NSEG >> 10;
    for (int i = -3; i < nvis; i++) {                        /* course pieces from just behind the camera on */
        int s = (base + i) & (NSEG - 1);
        MTX_PUSH = 0; gx_trans(seg_x[s], 0, seg_z[s]); gx_rot_y(-(seg_h[s] + 256)); gx_send(dl_road); MTX_POP = 1;
    }
    for (int k = 0; k < nk; k++) {
        kart_t *K = &kart[k];
        MTX_PUSH = 0; gx_trans(K->x, 0, K->z); gx_rot_y(-(K->h + 256)); gx_send(dl_shadow); gx_send(dl_kart); MTX_POP = 1;
    }
    for (int i = 0; i < nbox; i++) {
        s32 bx, bz; track_pt((i * 1024 / nbox) & 1023, &bx, &bz);
        MTX_PUSH = 0; gx_trans(bx, 0, bz); gx_rot_y((s32)f * 8 + i * 40); gx_send(dl_box); MTX_POP = 1;
    }
    for (int i = 0; i < ntree; i++) {
        MTX_PUSH = 0; gx_trans(tree_x[i], 0, tree_z[i]); gx_rot_y(-(cam_h + 256)); gx_send(dl_tree); MTX_POP = 1;
    }
    for (int i = 0; i < nhouse; i++) {
        MTX_PUSH = 0; gx_trans(box_x[i], 0, box_z[i]); gx_rot_y(i * 97); gx_send(dl_house); MTX_POP = 1;
    }
    for (int i = 0; i < npart; i++) {
        part_t *p = &part[i]; if (!p->life) continue;
        MTX_PUSH = 0; gx_trans(p->x, p->y, p->z); gx_rot_y(-(cam_h + 256));
        GX_COLOR = (u32)RGB(20 + (p->life & 7), 20, 20); gx_send(dl_part); MTX_POP = 1;
    }
    MTX_PUSH = 0; gx_trans(cam_x, 0, cam_z); MTX_SCALE = 4 << 12; MTX_SCALE = 4 << 12; MTX_SCALE = 4 << 12; gx_send(dl_sky); MTX_POP = 1;
}

static void race_frame(u32 f) {
    u32 t0 = timer_now();
    kart_logic();
    u32 t1 = timer_now();
    draw3d(f);
    prof[0] = t1 - t0; prof[1] = timer_now() - t1;
    SWAP_BUF = 1;                                            /* translucent: manual sort, as MKDS */
    /* HUD sprites into the OAM copies */
    hud_rot += 37;
    for (int i = 0; i < 128; i++) { oam[i * 4] = 0x0200; oamb[i * 4] = 0x0200; }  /* hidden */
    oam[0] = 8 | (1 << 8) | (1 << 9); oam[1] = (u16)(200 | (2 << 14)); oam[2] = 0;              /* item box 32x32 affine double */
    oam[4] = 120 | (1 << 8) | (1 << 9) | (3 << 14 & 0); oam[5] = (u16)(8 | (3 << 14) | (1 << 9)); oam[6] = 16;  /* place 64x64 */
    for (int i = 0; i < 10; i++) { oam[(4 + i) * 4] = 176; oam[(4 + i) * 4 + 1] = (u16)(8 + i * 10); oam[(4 + i) * 4 + 2] = (u16)(32 + (i & 3)); }
    for (int k = 0; k < nk; k++) {                           /* course map icons */
        s32 x = 128 + (kart[k].x >> 13), y = 96 + (kart[k].z >> 13);
        oamb[k * 4] = (u16)(y & 255); oamb[k * 4 + 1] = (u16)((x & 511) | (1 << 14)); oamb[k * 4 + 2] = (u16)(k * 4);
    }
}

static void race_vblank(u32 f) {
    dma_copy32((void *)OAM(0), oam, 1024); dma_copy32((void *)OAM(1), oamb, 1024);
    volatile s16 *aff = (volatile s16 *)0x07000000;             /* OAM affine group 0: item roulette rotation */
    s32 c = cs(hud_rot >> 4) >> 4, s = sn(hud_rot >> 4) >> 4;
    aff[3] = (s16)c; aff[7] = (s16)-s; aff[11] = (s16)s; aff[15] = (s16)c;
    volatile s16 *m = BGAFF(1, 2);                           /* course map, following the camera */
    s32 r = cam_h >> 2;
    m[0] = (s16)(cs(r) >> 4); m[1] = (s16)(-sn(r) >> 4); m[2] = (s16)(sn(r) >> 4); m[3] = (s16)(cs(r) >> 4);
    BGX(1, 2) = (u32)(cam_x >> 4) + (64 << 8); BGY(1, 2) = (u32)(cam_z >> 4) + (64 << 8);
    volatile u16 *hud = (volatile u16 *)0x0600F800;
    text(hud, 1, 1, "LAP"); text_dec(hud, 5, 1, laps % 3 + 1, 1); text(hud, 6, 1, "/3");
    text(hud, 20, 1, "TIME"); text_dec(hud, 25, 1, lapt / 60, 2); text(hud, 27, 1, ":"); text_dec(hud, 28, 1, (lapt % 60) * 100 / 60, 2);
    text(hud, 1, 22, "POS"); text_dec(hud, 5, 22, (u32)kart[0].place + 1, 2);
    text(hud, 20, 22, "ITEMS"); text_dec(hud, 26, 22, hits, 4);
    (void)f;
}

static u32 race_hash(void) {
    u32 h = 0x1234;
    for (int k = 0; k < nk; k++) { h = (h ^ (u32)kart[k].x) * 16777619u; h = (h ^ (u32)kart[k].z) * 16777619u; h ^= (u32)kart[k].u; }
    return h ^ hits ^ (laps << 16) ^ vmh;
}
static void race_leave(void) { }

const scene_t scene_race = { "RACE", race_enter, race_frame, race_vblank, race_leave, race_hash, 1 };
