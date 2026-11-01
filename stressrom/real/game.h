/* dsreal ARM9: shared declarations */
#pragma once
#include "hw.h"
#include "tables.h"          /* generated: sintab[1024] (4.12), proj_race[16], proj_town[16], font3x5[64] */

extern volatile struct cfg_t { u32 magic, mode, scene, level, frames, flags; } cfg;

/* ---- core (core9.c) ---- */
extern volatile u32 vbl_count, hbl_count;
extern volatile u32 stream_req;                 /* bit n: half n wants refilling */
extern volatile u32 touch_word, arm7_hash, arm7_hash_ok;
extern void (*volatile hblank_hook)(void);
void cpu_halt(void); void irq_on(void); void irq_off(void);
void core_init(void);
void wait_vblank(void);
u32 timer_now(void);                            /* 33.5 MHz cascaded timer 0/1 */
void fifo_send(u32 w);
void dma_copy32(void *dst, const void *src, u32 bytes);    /* channel 3, immediate */
void dma_fill32(void *dst, u32 val, u32 bytes);
void copy16(volatile void *dst, const void *src, u32 bytes);
void card_read(void *dst, u32 rom_addr, u32 bytes);         /* B7 reads, 0x200 blocks */
void eeprom_write(u32 addr, const void *src, u32 n);
u32 hash_words(u32 h, const volatile u32 *p, u32 words);    /* constant time per word */
s32 hw_div(s32 num, s32 den);                   /* the DIV unit, 32/32 */
s32 hw_div_fx(s32 num, s32 den);                /* (num << 12) / den, 64/32 */
u32 hw_sqrt(u32 v);
static inline s32 sn(s32 a) { return sintab[a & 1023]; }
static inline s32 cs(s32 a) { return sintab[(a + 256) & 1023]; }
static inline s32 fmul(s32 a, s32 b) { return (s32)(((s64)a * b) >> 12); }
s32 atan2i(s32 y, s32 x);                       /* angle 0..1023 */
u32 rnd(void);
void rnd_seed(u32 s);

/* display lists (packed GX commands), sent by the GX FIFO DMA */
typedef struct { u32 *w; u32 n; } dl_t;
void dl_start(u32 *buf);
void dl_cmd(u32 id, u32 np, ...);
dl_t dl_finish(void);
void gx_send(dl_t d);
void gx_rot_y(s32 a);                           /* MTX_MULT33 */
void gx_rot_x(s32 a);
void gx_trans(s32 x, s32 y, s32 z);

/* text: 3x5 font in 8x8 tiles, 4bpp (palette 0, colour 1); `map` = screen block, `tile0` = the font's first tile */
void font_upload(volatile u16 *chars);
void text(volatile u16 *map, int x, int y, const char *s);
void text_hex(volatile u16 *map, int x, int y, u32 v, int digits);
void text_dec(volatile u16 *map, int x, int y, u32 v, int digits);

/* game systems (work.c) */
int tri_collide(const s32 *t, int n, s32 x, s32 y, s32 z, s32 *push);
void vm_init(int k, u32 seed);
u32 vm_run(int k, int budget, const u8 *mem);

/* ---- per-scene stats, checksums (main9.c) ---- */
extern u32 scene_frame, level;
extern u32 prof[4];   /* per-part emulated cycles of the last frame (33.5 MHz ticks), shown on the status line */
void status_line(const char *name);

/* ---- scenes ---- */
typedef struct {
    const char *name;
    void (*enter)(void);
    void (*frame)(u32 f);       /* logic + 3D submission, before the vblank */
    void (*vblank)(u32 f);      /* register/VRAM/OAM updates in the vblank */
    void (*leave)(void);
    u32 (*state_hash)(void);
    u32 has3d;
} scene_t;
extern const scene_t scene_race, scene_action, scene_town, scene_ohg;

/* ROM data layout (build.py) */
#define ROM_OVL_A    0x080000
#define ROM_OVL_B    0x088000
#define ROM_TEXSET(n) (0x090000 + (n) * 0x10000)  /* 4 sets x 64 KB: 0 race, 1 town, 2/3 interiors */
#define ROM_MAP(n)   (0x0D0000 + (n) * 0x2000)    /* 32 map chunks x 8 KB */
#define ROM_STREAM   0x110000                     /* IMA ADPCM mono, 32768 Hz */
#define STREAM_BYTES 0x40000
#define OVL_ADDR     0x02200000

/* overlays: a table at the overlay's start */
typedef struct npc { s32 x, z, tx, tz; s32 dir, anim, wait, id; } npc_t;
typedef struct { u32 magic; void (*npc_update)(npc_t *n, int count, u32 frame, const u8 *col); u32 (*hash)(const npc_t *n, int count); } ovl_t;
#define OVL ((const ovl_t *)OVL_ADDR)
void overlay_load(int which);

/* textures: a set's layout inside texture slot 0 (bank A) and the palette slot (bank F) */
#define TEX_ROAD    0x0000      /* 64x64 direct */
#define TEX_GRASS   0x2000      /* 64x64 16-colour */
#define TEX_SMOKE   0x2800      /* 32x32 A3I5 */
#define TEX_BODY    0x2C00      /* 64x64 256-colour */
#define TEX_C4X4    0x3C00      /* 128x128 4x4-compressed (index data in slot 1 at 0x20000 + ofs/2) */
#define TEX_GLASS   0x4C00      /* 32x32 A5I3 */
#define TEX_NPC     0x5000      /* 8 frames of 16x32 16-colour, 256 B each */
#define TEX_TREE    0x5800      /* 32x64 A3I5 */
#define TEX_SET_BYTES 0x6000
#define PAL_GRASS   0x000       /* palette byte offsets in bank F */
#define PAL_SMOKE   0x020
#define PAL_BODY    0x060
#define PAL_C4X4    0x260
#define PAL_GLASS   0x460
#define PAL_NPC     0x480
#define PAL_TREE    0x500
#define PAL_SET_BYTES 0x600
void texset_load(int set);      /* card -> RAM -> LCDC banks A, B, F; then back to texture/palette slots */
u32 texparam(u32 ofs, u32 fmt, u32 sz_s, u32 sz_t, u32 extra);
