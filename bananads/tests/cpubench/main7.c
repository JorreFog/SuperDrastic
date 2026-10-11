/* cpubench ARM7: a sound-driver-like loop that never halts: per "tick", walks 16 channel structs, steps
   envelopes and a sequencer, and mixes 32 samples into a ring in main RAM. Written for this benchmark. */
typedef unsigned int u32; typedef int s32; typedef short s16; typedef unsigned char u8;
struct ch { s32 pos, step, vol, env, envstep; u8 on, note, pan, pad; };
static struct ch chs[16];
static const s16 wave[32] = {0,3212,6393,9512,12539,15446,18204,20787,23170,25329,27245,28898,30273,31356,32137,32609,
                             32767,32609,32137,31356,30273,28898,27245,25329,23170,20787,18204,15446,12539,9512,6393,3212};
static volatile s32 *const ring = (volatile s32 *)0x02300000;
static const u8 song[64] = {12,0,16,0,19,0,24,0,19,16,12,7,12,0,0,0,14,0,17,0,21,0,26,21,17,14,0,0,9,12,16,21,
                            24,0,19,0,16,0,12,0,7,0,12,16,19,24,28,31,0,0,24,0,19,16,12,0,5,9,12,17,21,24,0,0};
int main7(void)
{
    u32 tick = 0, w = 0;
    for (int i = 0; i < 16; i++) { chs[i].step = 0x1000 + i * 0x123; chs[i].vol = 64; chs[i].envstep = -1 - (i & 3); }
    for (;;) {
        tick++;
        u8 n = song[(tick >> 2) & 63];
        if (n) { struct ch *c = &chs[tick & 15]; c->on = 1; c->note = n; c->env = 127; c->step = 0x800 + n * 0x90; }
        for (int i = 0; i < 16; i++) {
            struct ch *c = &chs[i];
            if (!c->on) continue;
            c->env += c->envstep;
            if (c->env <= 0) { c->on = 0; c->env = 0; }
        }
        for (int s = 0; s < 32; s++) {
            s32 acc = 0;
            for (int i = 0; i < 16; i++) {
                struct ch *c = &chs[i];
                if (!c->on) continue;
                c->pos += c->step;
                acc += (wave[(c->pos >> 12) & 31] * c->env) >> 7;
            }
            ring[w++ & 1023] = acc >> 4;
        }
    }
}
