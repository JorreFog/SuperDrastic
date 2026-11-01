/* the EABI division helpers clang calls on ARMv4T/v5TE (no divide instruction); shift-subtract */
typedef unsigned int u32; typedef unsigned long long u64;
u32 __aeabi_uidiv(u32 n, u32 d) {
    if (!d) return 0xFFFFFFFFu;
    u32 q = 0, r = 0;
    for (int i = 31; i >= 0; i--) { r = (r << 1) | ((n >> i) & 1); if (r >= d) { r -= d; q |= 1u << i; } }
    return q;
}
u64 __aeabi_uidivmod(u32 n, u32 d) { u32 q = __aeabi_uidiv(n, d); return (u64)q | ((u64)(n - q * d) << 32); }
int __aeabi_idiv(int n, int d) { int s = (n < 0) ^ (d < 0); u32 q = __aeabi_uidiv(n < 0 ? -n : n, d < 0 ? -d : d); return s ? -(int)q : (int)q; }
u64 __aeabi_idivmod(int n, int d) { int q = __aeabi_idiv(n, d); return (u64)(u32)q | ((u64)(u32)(n - q * d) << 32); }
/* 64-bit multiply and shifts (Thumb-1 code calls these) */
u64 __aeabi_lmul(u64 a, u64 b) { u32 al = (u32)a, ah = (u32)(a >> 32), bl = (u32)b, bh = (u32)(b >> 32);
    u64 r = (u64)al * bl; r += (u64)(al * bh + ah * bl) << 32; return r; }
u64 __aeabi_llsl(u64 a, int n) { return a << n; }
u64 __aeabi_llsr(u64 a, int n) { return a >> n; }
long long __aeabi_lasr(long long a, int n) { return a >> n; }
