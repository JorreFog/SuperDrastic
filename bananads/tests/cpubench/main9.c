/* cpubench ARM9: a CPU-bound, game-like load for measuring CPU back-ends (BananaDS's docs/PERF.md). It never
   waits: every emulated frame is a full frame of ARM9 work, as in a busy game scene. Built as Thumb (games' main
   code) with some routines in ARM, one of them copied to ITCM and called there, as games do with hot code.
   Each pass runs eight workloads and folds their results into checksums. The top screen (VRAM A as a bitmap)
   shows the pass count, then the checksums as they were after pass 16, so a JIT and the interpreter can be compared
   once both have run 16 passes, whatever their timing. Written for this benchmark. */
typedef unsigned int u32; typedef unsigned short u16; typedef unsigned char u8; typedef int s32; typedef short s16;
#define R32(a) (*(volatile u32 *)(a))
#define R16(a) (*(volatile u16 *)(a))
#define R8(a) (*(volatile u8 *)(a))
#define ARM __attribute__((target("arm"), noinline))
#define NOINLINE __attribute__((noinline))

void *memset(void *d, int c, unsigned long n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }
/* word copy, 8 words a step (an LDM/STM pair in ARM), as libc's memcpy does for aligned buffers */
ARM void *memcpy(void *d, const void *s, unsigned long n)
{
    u32 *dw = d; const u32 *sw = s;
    if ((((u32)d | (u32)s) & 3) == 0) {
        while (n >= 32) { u32 a = sw[0], b = sw[1], c = sw[2], e = sw[3], f = sw[4], g = sw[5], h = sw[6], i = sw[7];
                          dw[0] = a; dw[1] = b; dw[2] = c; dw[3] = e; dw[4] = f; dw[5] = g; dw[6] = h; dw[7] = i;
                          dw += 8; sw += 8; n -= 32; }
        while (n >= 4) { *dw++ = *sw++; n -= 4; }
    }
    u8 *db = (u8 *)dw; const u8 *sb = (const u8 *)sw;
    while (n--) *db++ = *sb++;
    return d;
}

/* the DS's hardware divider, as games use it */
static NOINLINE s32 hwdiv(s32 n, s32 d)
{
    R16(0x04000280) = 0; R32(0x04000290) = (u32)n; R32(0x04000294) = (u32)(n >> 31); R32(0x04000298) = (u32)d;
    R32(0x0400029C) = (u32)(d >> 31);
    while (R16(0x04000280) & 0x8000) ;
    return (s32)R32(0x040002A0);
}

/* 1. a bytecode interpreter (scripting engines) */
enum { OP_PUSH, OP_ADD, OP_SUB, OP_MUL, OP_DUP, OP_SWAP, OP_JNZ, OP_DEC, OP_LOAD, OP_STORE, OP_CALL, OP_RET, OP_XOR,
       OP_SHL, OP_DROP, OP_END };
static const u8 script[] = {
    OP_PUSH, 40, OP_STORE, 0,                       /* r0 = 40 (outer count) */
    /* 4: loop */
    OP_LOAD, 0, OP_CALL, 20,                        /* f(r0) */
    OP_LOAD, 1, OP_ADD, OP_STORE, 1,                /* r1 += f(r0) */
    OP_LOAD, 0, OP_DEC, OP_DUP, OP_STORE, 0, OP_JNZ, 4 - 0, OP_END,
};
static const u8 func[] = { /* f(x) = ((x * 7) ^ (x << 3)) - 5 */
    OP_DUP, OP_PUSH, 7, OP_MUL, OP_SWAP, OP_PUSH, 3, OP_SHL, OP_XOR, OP_PUSH, 5, OP_SUB, OP_RET };
static NOINLINE u32 vm_run(u32 seed)
{
    s32 stack[32], regs[4] = {0, (s32)seed, 0, 0}; int sp = 0; const u8 *pc = script; const u8 *rs[8]; int rsp = 0;
    for (;;) {
        u8 op = *pc++;
        switch (op) {
        case OP_PUSH: stack[sp++] = *pc++; break;
        case OP_ADD: sp--; stack[sp - 1] += stack[sp]; break;
        case OP_SUB: sp--; stack[sp - 1] -= stack[sp]; break;
        case OP_MUL: sp--; stack[sp - 1] *= stack[sp]; break;
        case OP_DUP: stack[sp] = stack[sp - 1]; sp++; break;
        case OP_SWAP: { s32 t = stack[sp - 1]; stack[sp - 1] = stack[sp - 2]; stack[sp - 2] = t; break; }
        case OP_JNZ: { u8 t = *pc++; if (stack[--sp]) pc = script + t; break; }
        case OP_DEC: stack[sp - 1]--; break;
        case OP_LOAD: stack[sp++] = regs[*pc++]; break;
        case OP_STORE: regs[*pc++] = stack[--sp]; break;
        case OP_CALL: pc++; rs[rsp++] = pc; pc = func; break;
        case OP_RET: pc = rs[--rsp]; break;
        case OP_XOR: sp--; stack[sp - 1] ^= stack[sp]; break;
        case OP_SHL: sp--; stack[sp - 1] <<= stack[sp]; break;
        case OP_DROP: sp--; break;
        default: return (u32)regs[1];
        }
    }
}

/* 2. entities: fixed-point movement, bounces, collisions */
struct ent { s32 x, y, vx, vy; u16 flags, hp; u8 kind, frame, pad[2]; };
static struct ent ents[64];
static NOINLINE void ent_init(void)
{
    u32 r = 12345;
    for (int i = 0; i < 64; i++) {
        r = r * 1103515245 + 12345; ents[i].x = (r >> 8) & 0xFFFFF; r = r * 1103515245 + 12345; ents[i].y = (r >> 8) & 0xBFFFF;
        ents[i].vx = (s32)((r >> 16) & 0x7FF) - 0x400; ents[i].vy = (s32)((r >> 5) & 0x7FF) - 0x400;
        ents[i].hp = 100; ents[i].kind = i & 3;
    }
}
static NOINLINE u32 ent_update(void)
{
    u32 sum = 0;
    for (int i = 0; i < 64; i++) {
        struct ent *e = &ents[i];
        e->x += e->vx; e->y += e->vy;
        if (e->x < 0 || e->x > 0xFFFFF) { e->vx = -e->vx; e->x += e->vx; e->flags ^= 1; }
        if (e->y < 0 || e->y > 0xBFFFF) { e->vy = -e->vy; e->y += e->vy; e->flags ^= 2; }
        if (e->kind == 1) e->vy += 8;
        e->frame = (e->frame + 1) & 7;
    }
    for (int i = 0; i < 8; i++)
        for (int j = 8; j < 64; j++) {
            s32 dx = ents[i].x - ents[j].x, dy = ents[i].y - ents[j].y;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx < 0x8000 && dy < 0x8000) { ents[j].hp--; sum += j; }
        }
    for (int i = 0; i < 64; i++) sum += (u32)ents[i].x ^ (u32)ents[i].y ^ ents[i].hp;
    return sum;
}

/* 3. sprite sort by y (insertion) into an OAM shadow, then copied (memcpy) */
static u16 oam[128 * 4], oam2[128 * 4];
static u8 order[64];
static NOINLINE u32 sprite_sort(void)
{
    for (int i = 0; i < 64; i++) order[i] = (u8)i;
    for (int i = 1; i < 64; i++) {
        u8 k = order[i]; s32 ky = ents[k].y; int j = i - 1;
        while (j >= 0 && ents[order[j]].y > ky) { order[j + 1] = order[j]; j--; }
        order[j + 1] = k;
    }
    for (int i = 0; i < 64; i++) {
        struct ent *e = &ents[order[i]];
        oam[i * 4 + 0] = (u16)((e->y >> 12) & 0xFF) | 0x2000;
        oam[i * 4 + 1] = (u16)((e->x >> 12) & 0x1FF) | 0x4000;
        oam[i * 4 + 2] = (u16)(e->kind * 16 + e->frame * 2);
    }
    memcpy(oam2, oam, sizeof(oam));
    return oam2[5] ^ oam2[200];
}

/* 4. LZ77 (the DS BIOS's format: flags byte, 8 tokens, 12-bit distance, 4-bit length) */
static u8 lzsrc[4096], lzbuf[2048], lzout[4096];
static int lzlen;
static NOINLINE void lz_compress(void)
{
    u32 r = 99;
    for (int i = 0; i < 4096; i++) { r = r * 1664525 + 1013904223; lzsrc[i] = (i & 64) ? (u8)(r >> 29) : (u8)("pokemon text and tiles "[i % 23]); }
    int o = 0, i = 0;
    while (i < 4096 && o < 2000) {
        int fpos = o++; u8 flags = 0;
        for (int b = 0; b < 8 && i < 4096; b++) {
            int best = 0, bd = 0;
            for (int d = 1; d <= 24 && d <= i; d++) {
                int l = 0;
                while (l < 18 && i + l < 4096 && lzsrc[i + l] == lzsrc[i + l - d]) l++;
                if (l > best) { best = l; bd = d; }
            }
            if (best >= 3) { flags |= 0x80 >> b; lzbuf[o++] = (u8)(((best - 3) << 4) | ((bd - 1) >> 8)); lzbuf[o++] = (u8)(bd - 1); i += best; }
            else lzbuf[o++] = lzsrc[i++];
        }
        lzbuf[fpos] = flags;
    }
    lzlen = i;
}
static NOINLINE u32 lz_decompress(void)
{
    const u8 *s = lzbuf; u8 *d = lzout; u8 *end = lzout + lzlen;
    while (d < end) {
        u8 flags = *s++;
        for (int b = 0; b < 8 && d < end; b++, flags <<= 1) {
            if (flags & 0x80) {
                int len = (s[0] >> 4) + 3, dist = (((s[0] & 15) << 8) | s[1]) + 1; s += 2;
                const u8 *p = d - dist;
                while (len-- && d < end) *d++ = *p++;
            } else *d++ = *s++;
        }
    }
    return lzout[100] + lzout[3000] * 7 + lzout[lzlen - 1];
}

/* 5. text into 8x8 4bpp tiles from a font table */
static u8 font[96][8];
static u32 tiles[40 * 8];
static const char msg[] = "The quick brown fox jumps over the lazy dog. 0123456789!!!!!!!!!";   /* 64 characters */
static NOINLINE u32 text_render(u32 shift)
{
    u32 sum = 0;
    for (int c = 0; c < 40; c++) {
        u8 ch = (u8)msg[(c + shift) & 63] - 32;
        for (int row = 0; row < 8; row++) {
            u8 bits = font[ch][row]; u32 t = 0;
            for (int px = 0; px < 8; px++) if (bits & (0x80 >> px)) t |= 1u << (px * 4);
            tiles[c * 8 + row] = t; sum += t;
        }
    }
    return sum;
}

/* 6. CRC32 with a table, in ARM (games keep checksums and hot loops in ARM) */
static u32 crctab[256];
ARM static void crc_init(void)
{
    for (u32 i = 0; i < 256; i++) { u32 c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1; crctab[i] = c; }
}
ARM static u32 crc32(const u8 *p, int n)
{
    u32 c = 0xFFFFFFFF;
    while (n--) c = crctab[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

/* 7. calls and returns: a recursive walk and small helpers */
static NOINLINE u32 fib(u32 n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static NOINLINE s32 clamp(s32 v, s32 lo, s32 hi) { return v < lo ? lo : v > hi ? hi : v; }
static NOINLINE u32 calls(u32 seed)
{
    u32 s = fib(14);
    for (int i = 0; i < 64; i++) s += (u32)clamp((s32)(seed * i) >> 20, -100, 100) + (u32)hwdiv((s32)(seed ^ (u32)i) & 0xFFFFF, i + 3);
    return s;
}

/* 8. a mixing routine run from ITCM (position independent: everything comes in through the arguments) */
__attribute__((section(".itcmcode"), target("arm"), noinline)) u32 itcm_mix(u32 *buf, int n, u32 k)
{
    u32 a = k, b = 0x9E3779B9;
    for (int i = 0; i < n; i++) { a += buf[i]; a ^= a << 7; b += a ^ (a >> 11); buf[i] = b; }
    return a ^ b;
}
extern u8 __itcm_start[], __itcm_end[];
typedef u32 (*mixfn)(u32 *, int, u32);
static u32 mixbuf[256];

/* the checksums, drawn as 4x4 blocks: 8 rows of 32 bits */
static void draw(const u32 *v, int n)
{
    volatile u16 *fb = (volatile u16 *)0x06800000;
    for (int r = 0; r < n; r++)
        for (int b = 0; b < 32; b++) {
            u16 col = (v[r] >> b) & 1 ? 0x7FFF : (u16)(0x0010 + r * 0x0C00);
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) fb[(8 + r * 6 + y) * 256 + 8 + b * 7 + x] = col;
        }
}

int main(void)
{
    R32(0x04000304) = 0x8003;                      /* POWCNT1: LCDs, 2D A, top screen */
    R8(0x04000240) = 0x80;                          /* VRAM A: LCDC */
    R32(0x04000000) = 0x00020000;                   /* DISPCNT A: VRAM display */
    for (int i = 0; i < 96; i++) for (int r = 0; r < 8; r++) font[i][r] = (u8)((i * 37 + r * 11) ^ (r << 4));
    ent_init(); lz_compress(); crc_init();
    mixfn mix = (mixfn)0x01000000;                  /* ITCM */
    memcpy((void *)0x01000000, __itcm_start, (unsigned long)(__itcm_end - __itcm_start));
    static u32 sums[8], frozen[9];
    u32 pass = 0;
    for (;;) {
        pass++;
        sums[0] += vm_run(pass);
        sums[1] ^= ent_update();
        sums[2] += sprite_sort();
        sums[3] += lz_decompress();
        sums[4] += text_render(pass);
        sums[5] ^= crc32(lzout, 1024) + crc32((const u8 *)tiles, sizeof(tiles));
        sums[6] += calls(pass);
        for (int i = 0; i < 256; i++) mixbuf[i] = sums[i & 7] + (u32)i;
        sums[7] ^= mix(mixbuf, 256, pass);
        if (pass == 16) for (int i = 0; i < 8; i++) frozen[i + 1] = sums[i];
        frozen[0] = pass;
        draw(frozen, 9);
    }
}
