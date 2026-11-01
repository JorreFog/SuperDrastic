/* dsreal: DS hardware registers and small helpers (GBATEK names). Own code, GPL-3.0 like the rest. */
#pragma once
typedef unsigned int u32; typedef unsigned short u16; typedef unsigned char u8;
typedef int s32; typedef short s16; typedef signed char s8; typedef unsigned long long u64; typedef long long s64;
#define R32(a) (*(volatile u32 *)(a))
#define R16(a) (*(volatile u16 *)(a))
#define R8(a)  (*(volatile u8 *)(a))
#define NOINLINE __attribute__((noinline))
#define ARM     __attribute__((target("arm")))
#define ITCM    __attribute__((section(".itcm"), noinline, target("arm")))

/* 2D, engine A at 0x04000000, engine B at 0x04001000 */
#define DISPCNT(e)   R32(0x04000000 + (e) * 0x1000)
#define DISPSTAT     R16(0x04000004)
#define VCOUNT       R16(0x04000006)
#define BGCNT(e, n)  R16(0x04000008 + (e) * 0x1000 + (n) * 2)
#define BGHOFS(e, n) R16(0x04000010 + (e) * 0x1000 + (n) * 4)
#define BGVOFS(e, n) R16(0x04000012 + (e) * 0x1000 + (n) * 4)
#define BGAFF(e, n)  ((volatile s16 *)(0x04000000 + (e) * 0x1000 + 0x20 + ((n) - 2) * 0x10))   /* PA PB PC PD */
#define BGX(e, n)    R32(0x04000028 + (e) * 0x1000 + ((n) - 2) * 0x10)
#define BGY(e, n)    R32(0x0400002C + (e) * 0x1000 + ((n) - 2) * 0x10)
#define WIN0H(e)     R16(0x04000040 + (e) * 0x1000)
#define WIN1H(e)     R16(0x04000042 + (e) * 0x1000)
#define WIN0V(e)     R16(0x04000044 + (e) * 0x1000)
#define WIN1V(e)     R16(0x04000046 + (e) * 0x1000)
#define WININ(e)     R16(0x04000048 + (e) * 0x1000)
#define WINOUT(e)    R16(0x0400004A + (e) * 0x1000)
#define MOSAIC(e)    R16(0x0400004C + (e) * 0x1000)
#define BLDCNT(e)    R16(0x04000050 + (e) * 0x1000)
#define BLDALPHA(e)  R16(0x04000052 + (e) * 0x1000)
#define BLDY(e)      R16(0x04000054 + (e) * 0x1000)
#define MASTER_BRIGHT(e) R16(0x0400006C + (e) * 0x1000)
#define DISP3DCNT    R32(0x04000060)
#define DISPCAPCNT   R32(0x04000064)

/* DMA (ARM9): channel c */
#define DMASAD(c)    R32(0x040000B0 + (c) * 12)
#define DMADAD(c)    R32(0x040000B4 + (c) * 12)
#define DMACNT(c)    R32(0x040000B8 + (c) * 12)
#define DMAFILL(c)   R32(0x040000E0 + (c) * 4)
#define DMA_EN       (1u << 31)
#define DMA_32       (1u << 26)
#define DMA_REPEAT   (1u << 25)
#define DMA_SRC_FIX  (2u << 23)
#define DMA_DST_FIX  (2u << 21)
#define DMA_DST_RLD  (3u << 21)
#define DMA_VBL      (1u << 27)
#define DMA_HBL      (2u << 27)
#define DMA_GXFIFO   (7u << 27)
#define DMA_CARD     (5u << 27)

#define TMCNT_L(t)   R16(0x04000100 + (t) * 4)
#define TMCNT_H(t)   R16(0x04000102 + (t) * 4)
#define KEYINPUT     R16(0x04000130)

#define IPCSYNC      R16(0x04000180)
#define IPCFIFOCNT   R16(0x04000184)
#define IPCFIFOSEND  R32(0x04000188)
#define IPCFIFORECV  R32(0x04100000)

#define AUXSPICNT    R16(0x040001A0)
#define AUXSPIDATA   R16(0x040001A2)
#define ROMCTRL      R32(0x040001A4)
#define CARDCMD      ((volatile u8 *)0x040001A8)
#define CARDDATA     R32(0x04100010)
#define EXMEMCNT     R16(0x04000204)

#define IME          R32(0x04000208)
#define IE           R32(0x04000210)
#define IF           R32(0x04000214)
#define IRQ_VBLANK   (1u << 0)
#define IRQ_HBLANK   (1u << 1)
#define IRQ_TIMER(n) (1u << (3 + (n)))
#define IRQ_IPCRECV  (1u << 18)

#define VRAMCNT(b)   R8(0x04000240 + (b) + ((b) >= 7 ? 1 : 0))   /* A..G = 0..6, H = 7 (0x248), I = 8 (0x249) */
#define WRAMCNT      R8(0x04000247)

#define DIVCNT       R16(0x04000280)
#define DIV_NUMER    R32(0x04000290)
#define DIV_NUMER_H  R32(0x04000294)
#define DIV_DENOM    R32(0x04000298)
#define DIV_DENOM_H  R32(0x0400029C)
#define DIV_RESULT   R32(0x040002A0)
#define SQRTCNT      R16(0x040002B0)
#define SQRT_RESULT  R32(0x040002B4)
#define SQRT_PARAM   R32(0x040002B8)
#define SQRT_PARAM_H R32(0x040002BC)
#define POWCNT1      R32(0x04000304)

/* 3D */
#define EDGE_COLOR(n) R16(0x04000330 + (n) * 2)
#define ALPHA_TEST_REF R16(0x04000340)
#define CLEAR_COLOR  R32(0x04000350)
#define CLEAR_DEPTH  R16(0x04000354)
#define FOG_COLOR    R32(0x04000358)
#define FOG_OFFSET   R16(0x0400035C)
#define FOG_TABLE(n) R8(0x04000360 + (n))
#define TOON_TABLE(n) R16(0x04000380 + (n) * 2)
#define GXFIFO       R32(0x04000400)
#define MTX_MODE     R32(0x04000440)
#define MTX_PUSH     R32(0x04000444)
#define MTX_POP      R32(0x04000448)
#define MTX_IDENT    R32(0x04000454)
#define MTX_LOAD44   R32(0x04000458)
#define MTX_MULT33   R32(0x04000468)
#define MTX_SCALE    R32(0x0400046C)
#define MTX_TRANS    R32(0x04000470)
#define GX_COLOR     R32(0x04000480)
#define GX_NORMAL    R32(0x04000484)
#define GX_TEXCOORD  R32(0x04000488)
#define GX_VTX16     R32(0x0400048C)
#define GX_VTX10     R32(0x04000490)
#define POLY_ATTR    R32(0x040004A4)
#define TEX_PARAM    R32(0x040004A8)
#define PLTT_BASE    R32(0x040004AC)
#define DIF_AMB      R32(0x040004C0)
#define SPE_EMI      R32(0x040004C4)
#define LIGHT_VEC    R32(0x040004C8)
#define LIGHT_COL    R32(0x040004CC)
#define BEGIN_VTXS   R32(0x04000500)
#define END_VTXS     R32(0x04000504)
#define SWAP_BUF     R32(0x04000540)
#define VIEWPORT     R32(0x04000580)
#define GXSTAT       R32(0x04000600)
#define RAM_COUNT    R32(0x04000604)

/* packed GX command ids */
enum { G_MTX_MODE = 0x10, G_PUSH = 0x11, G_POP = 0x12, G_IDENT = 0x15, G_MULT33 = 0x1A, G_SCALE = 0x1B, G_TRANS = 0x1C,
       G_COLOR = 0x20, G_NORMAL = 0x21, G_TEXCOORD = 0x22, G_VTX16 = 0x23, G_VTX10 = 0x24, G_VTXXY = 0x25,
       G_VTXXZ = 0x26, G_VTXYZ = 0x27, G_VTXDIFF = 0x28, G_POLYATTR = 0x29, G_TEXPARAM = 0x2A, G_PLTT = 0x2B,
       G_DIFAMB = 0x30, G_SPEEMI = 0x31, G_BEGIN = 0x40, G_END = 0x41 };

#define RGB(r, g, b) ((u16)((r) | ((g) << 5) | ((b) << 10)))
#define VRAM_LCDC(b) (0x06800000 + (b) * 0x20000)   /* A..D */
#define LCDC_E 0x06880000
#define LCDC_F 0x06890000
#define LCDC_G 0x06894000
#define OAM(e)  ((volatile u16 *)(0x07000000 + (e) * 0x400))
#define PAL_BG(e)  ((volatile u16 *)(0x05000000 + (e) * 0x400))
#define PAL_OBJ(e) ((volatile u16 *)(0x05000200 + (e) * 0x400))
