/* dsreal ARM9: the scene loop. Per frame: the scene's logic + 3D submission, the stream refill when the ARM7 asks,
 * then halt until the vblank IRQ (as games do), the scene's vblank work. Per scene: checksums of the pictures
 * (display capture into bank D, 4 times a scene) and of the game state + the ARM7's driver state, the emulated ARM9
 * load (cycles before the vblank wait), lag frames; written as a 32-byte record to the save (EEPROM) at the scene's
 * end and drawn on the bottom screen's status line. */
#include "game.h"

u32 scene_frame, level, prof[4];
static const scene_t *const scenes[] = { &scene_race, &scene_action, &scene_town };
#define NSCENES 3
static const u16 scene_frames[NSCENES] = { 1500, 1200, 1500 };    /* 25 s, 20 s, 25 s at 60 fps (phase table) */

typedef struct { u32 magic, loop_scene, frames, crc_pic, crc_state, arm7, load_avg, lag_max; } rec_t;   /* 32 bytes */
static rec_t rec, recs[NSCENES];
static u32 loop_n;

/* ---- the stream: IMA ADPCM from the card, 2048 samples a half, decoded on the ARM9 ---- */
static s16 stream_buf[2][2][2048] __attribute__((aligned(32)));    /* [channel][half] contiguous per channel */
static u8 adpcm[1024] __attribute__((aligned(4)));
static u32 stream_pos;
static s32 st_pred, st_idx;
static const s16 ima_step[89] = { 7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,
    130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,
    1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,
    13899,15289,16818,18500,20350,22385,24623,27086,29794,32767 };
static const s8 ima_idx[16] = { -1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8 };
static void stream_fill(int half) {
    card_read(adpcm, ROM_STREAM + stream_pos, 1024);
    stream_pos = (stream_pos + 1024) & (STREAM_BYTES - 1);
    s16 *l = stream_buf[0][half], *r = stream_buf[1][half];
    s32 echo = r[2047];
    for (int i = 0; i < 2048; i++) {
        u32 nib = (adpcm[i >> 1] >> ((i & 1) * 4)) & 15;
        s32 step = ima_step[st_idx], d = step >> 3;
        if (nib & 4) d += step; if (nib & 2) d += step >> 1; if (nib & 1) d += step >> 2;
        st_pred += (nib & 8) ? -d : d;
        if (st_pred > 32767) st_pred = 32767; if (st_pred < -32768) st_pred = -32768;
        st_idx += ima_idx[nib]; if (st_idx < 0) st_idx = 0; if (st_idx > 88) st_idx = 88;
        l[i] = (s16)(st_pred >> 1);
        echo = (echo * 3 + st_pred) >> 2; r[i] = (s16)(echo >> 1);   /* a cheap stereo widening */
    }
}
static void stream_start(void) {
    stream_pos = 0; st_pred = 0; st_idx = 0;
    stream_fill(0); stream_fill(1);
    fifo_send((2u << 28) | ((u32)stream_buf & 0x0FFFFFFF));
}

/* ---- the sample bank for the ARM7's instruments ---- */
static s16 bank[0x4080 / 2 + 64] __attribute__((aligned(4)));
static void snd_bank(void) {
    for (int i = 0; i < 32; i++) { bank[i] = (s16)(sintab[i * 32] * 5); bank[32 + i] = (s16)((i - 16) * 1200); }
    u32 s = 7;
    for (int i = 0; i < 8192; i++) {                          /* pluck: decaying filtered noise */
        s = s * 1103515245u + 12345u;
        s32 env = 8192 - i, v = ((s32)(s >> 16) - 32768) * env / 8192;
        bank[64 + i] = (s16)((v + (i ? bank[63 + i] : 0)) / 3);
    }
    s8 *pad = (s8 *)&bank[0x4080 / 2];
    for (int i = 0; i < 64; i++) pad[i] = (s8)(sintab[i * 16] >> 6);
    fifo_send((1u << 28) | ((u32)bank & 0x0FFFFFFF));
}

/* ---- status line (bottom screen, engine B BG0) ---- */
static volatile u16 *const smap = (volatile u16 *)0x0620F800;
static u32 load_sum, load_n, load_max, lag_now, ram_max;
static void status_crc(int row, const rec_t *r);
void status_line(const char *name) {
    text(smap, 0, 0, "                                ");
    text(smap, 0, 0, name); text(smap, 7, 0, "L"); text_dec(smap, 8, 0, level, 2);
    text(smap, 11, 0, "F"); text_dec(smap, 12, 0, scene_frame, 5);
    text(smap, 18, 0, "CPU"); text_dec(smap, 21, 0, load_n ? load_sum / load_n : 0, 3); text(smap, 24, 0, "%");
    text(smap, 26, 0, "LP"); text_dec(smap, 28, 0, loop_n, 3);
    u32 rc = ram_max; text(smap, 0, 1, "POLY"); text_dec(smap, 5, 1, rc & 0xFFF, 4); text(smap, 10, 1, "VTX"); text_dec(smap, 14, 1, (rc >> 16) & 0x1FFF, 4);
    text(smap, 19, 1, "MAX"); text_dec(smap, 23, 1, load_max, 3); text(smap, 27, 1, "LAG"); text_dec(smap, 30, 1, lag_now, 2);
    for (int i = 0; i < 4; i++) { text(smap, i * 8, 6, "P"); text_dec(smap, i * 8 + 1, 6, prof[i] / 5602, 3); }
    for (int i = 0; i < NSCENES; i++) if (recs[i].magic) status_crc(2 + i, &recs[i]);
}
static void status_crc(int row, const rec_t *r) {
    text_hex(smap, 0, row, r->loop_scene, 4); text_hex(smap, 5, row, r->crc_pic, 8); text_hex(smap, 14, row, r->crc_state, 8);
    text_hex(smap, 23, row, r->arm7, 8);
}

static void vram_base(void) {
    POWCNT1 = 0x820F;                                          /* both LCDs, 2D A+B, 3D; engine A on top */
    VRAMCNT(4) = 0x81;                                         /* E: main BG 0x06000000 (64 KB) */
    VRAMCNT(6) = 0x82;                                         /* G: main OBJ 0x06400000 (16 KB) */
    VRAMCNT(2) = 0x84;                                         /* C: sub BG 0x06200000 */
    VRAMCNT(8) = 0x82;                                         /* I: sub OBJ 0x06600000 */
    VRAMCNT(3) = 0x80;                                         /* D: LCDC, the display capture's target */
    VRAMCNT(5) = 0x83;                                         /* F: texture palette slot 0 */
    for (int e = 0; e < 2; e++) {
        DISPCNT(e) = 0x10000; BLDCNT(e) = 0; MASTER_BRIGHT(e) = 0; MOSAIC(e) = 0;
        for (int n = 0; n < 4; n++) { BGHOFS(e, n) = 0; BGVOFS(e, n) = 0; }
    }
    dma_fill32((void *)0x06000000, 0, 0x10000); dma_fill32((void *)0x06200000, 0, 0x10000);
    font_upload((volatile u16 *)0x06000000); font_upload((volatile u16 *)0x06200000);
    PAL_BG(0)[0] = 0; PAL_BG(0)[1] = 0x7FFF; PAL_BG(1)[0] = RGB(2, 2, 6); PAL_BG(1)[1] = 0x7FFF;
    BGCNT(1, 0) = 0 | (31 << 8);                               /* the status line: engine B BG0, map 31, chars 0 */
}

int main(void) {
    core_init();
    level = cfg.level ? cfg.level : 5; if (level > 10) level = 10;
    snd_bank();
    stream_start();
    u32 si = cfg.mode == 1 ? cfg.scene % NSCENES : 0;
    for (;;) {
        const scene_t *s = scenes[si];
        u32 frames = cfg.frames ? cfg.frames : scene_frames[si];
        vram_base();
        s->enter();
        DISPCNT(1) |= (1 << 8);                                 /* the status line always shows */
        u32 crc_pic = 0x811C9DC5u, lag = 0, lagmax = 0, last_vbl = vbl_count;
        load_sum = 0; load_n = 0; load_max = 0; lag_now = 0; ram_max = 0;
        for (scene_frame = 0; scene_frame < frames; scene_frame++) {
            u32 t0 = timer_now();
            s->frame(scene_frame);
            { u32 rc = RAM_COUNT; if ((rc & 0xFFF) > (ram_max & 0xFFF)) ram_max = rc; }
            while (stream_req) {                                /* the ARM7 asked for a half */
                u32 r = stream_req; int h = (r & 1) ? 0 : 1; stream_req &= ~(1u << h); stream_fill(h);
            }
            u32 used = timer_now() - t0;                        /* 33.5 MHz ticks; a frame is 560,190 */
            u32 pct = used / 5602; load_sum += pct; load_n++; if (pct > load_max) load_max = pct;
            if (scene_frame % 300 == 150) DISPCAPCNT = (1u << 31) | (3 << 16) | (3 << 20);   /* capture A into D */
            wait_vblank();
            u32 v = vbl_count; if (v - last_vbl > 1) { lag += v - last_vbl - 1; lag_now = lag; if (v - last_vbl - 1 > lagmax) lagmax = v - last_vbl - 1; } last_vbl = v;
            s->vblank(scene_frame);
            if (scene_frame % 300 == 152) crc_pic = hash_words(crc_pic, (const volatile u32 *)VRAM_LCDC(3), 256 * 192 / 2);
            if ((scene_frame & 15) == 0) status_line(s->name);
        }
        arm7_hash_ok = 0; fifo_send(5u << 28);
        while (!arm7_hash_ok) wait_vblank();
        rec = (rec_t){ 0x43455252, (loop_n << 8) | si, frames, crc_pic, s->state_hash(), arm7_hash, load_sum / (load_n ? load_n : 1), (lagmax << 16) | (lag & 0xFFFF) };
        s->leave();
        eeprom_write(((loop_n & 3) * 8 + si) * 32, &rec, sizeof rec);
        recs[si] = rec;
        if (cfg.mode == 1) { loop_n++; continue; }
        if (++si == NSCENES) { si = 0; loop_n++; }
    }
}
