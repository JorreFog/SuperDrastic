// Prints "assembly|word" for every instruction form BananaJIT's encoder makes. tests/jit_encodings.sh
// assembles the left side with llvm-mc and checks it gives the same words.
#include <cstdio>

#include "a64.h"

using namespace bananajit;

static void emit(const char* text, void (*f)(A64&)) {
    uint32_t buf[8];
    A64 a(buf);
    f(a);
    printf("%s|", text);
    for (uint32_t* p = buf; p < a.here(); p++) printf("%08x%s", *p, p + 1 < a.here() ? "," : "");
    printf("\n");
}

#define E(text, body) emit(text, [](A64& a) { body; })

int main() {
    E("add w1, w2, w3", a.add((Reg)1, (Reg)2, (Reg)3));
    E("add w1, w2, w3, lsl #5", a.add((Reg)1, (Reg)2, (Reg)3, LSL, 5));
    E("adds w19, w20, w21, lsr #31", a.adds((Reg)19, (Reg)20, (Reg)21, LSR, 31));
    E("sub w0, w9, w10, asr #7", a.sub((Reg)0, (Reg)9, (Reg)10, ASR, 7));
    E("subs wzr, w1, w2", a.subs(ZR, (Reg)1, (Reg)2));
    E("add x0, x29, x1", a.addx((Reg)0, (Reg)29, (Reg)1));
    E("add w1, w2, #4095", a.addi((Reg)1, (Reg)2, 4095));
    E("sub w3, w4, #1", a.subi((Reg)3, (Reg)4, 1));
    E("add x0, sp, #16", a.addxi((Reg)0, SP, 16));
    E("and w1, w2, w3, ror #3", a.and_((Reg)1, (Reg)2, (Reg)3, ROR, 3));
    E("bic w1, w2, w3", a.bic((Reg)1, (Reg)2, (Reg)3));
    E("orr w5, w6, w7, lsl #2", a.orr((Reg)5, (Reg)6, (Reg)7, LSL, 2));
    E("orn w1, wzr, w2", a.orn((Reg)1, ZR, (Reg)2));
    E("eor w1, w2, w3, asr #1", a.eor((Reg)1, (Reg)2, (Reg)3, ASR, 1));
    E("ands w1, w2, w3", a.ands((Reg)1, (Reg)2, (Reg)3));
    E("mov w1, w2", a.mov((Reg)1, (Reg)2));
    E("mov x29, x0", a.movx((Reg)29, (Reg)0));
    E("tst w4, w4", a.tst((Reg)4, (Reg)4));
    E("adc w1, w2, w3", a.adc((Reg)1, (Reg)2, (Reg)3));
    E("adcs w1, w2, w3", a.adcs((Reg)1, (Reg)2, (Reg)3));
    E("sbc w1, w2, w3", a.sbc((Reg)1, (Reg)2, (Reg)3));
    E("sbcs w1, w2, w3", a.sbcs((Reg)1, (Reg)2, (Reg)3));
    E("lsl w1, w2, w3", a.lslv((Reg)1, (Reg)2, (Reg)3));
    E("lsr w1, w2, w3", a.lsrv((Reg)1, (Reg)2, (Reg)3));
    E("asr w1, w2, w3", a.asrv((Reg)1, (Reg)2, (Reg)3));
    E("ror w1, w2, w3", a.rorv((Reg)1, (Reg)2, (Reg)3));
    E("ubfx w2, w3, #29, #1", a.ubfx((Reg)2, (Reg)3, 29, 1));
    E("ubfx w0, w0, #28, #4", a.ubfx((Reg)0, (Reg)0, 28, 4));
    E("bfi w27, w0, #28, #4", a.bfi((Reg)27, (Reg)0, 28, 4));
    E("bfi w27, w2, #29, #1", a.bfi((Reg)27, (Reg)2, 29, 1));
    E("lsl w1, w2, #1", a.lsli((Reg)1, (Reg)2, 1));
    E("lsl w1, w2, #31", a.lsli((Reg)1, (Reg)2, 31));
    E("lsr w1, w2, #28", a.lsri((Reg)1, (Reg)2, 28));
    E("asr w1, w2, #31", a.asri((Reg)1, (Reg)2, 31));
    E("ror w1, w2, #9", a.rori((Reg)1, (Reg)2, 9));
    E("movz w1, #0x1234", a.movz((Reg)1, 0x1234));
    E("movz w1, #0x1234, lsl #16", a.movz((Reg)1, 0x1234, 1));
    E("movk w1, #0xbeef, lsl #16", a.movk((Reg)1, 0xbeef, 1));
    E("movn w1, #0", a.movn((Reg)1, 0));
    E("movz x16, #0x1234, lsl #32", a.movzx((Reg)16, 0x1234, 2));
    E("movk x16, #0x5678, lsl #48", a.movkx((Reg)16, 0x5678, 3));
    E("movz w1, #0x5678\nmovk w1, #0x1234, lsl #16", a.movi((Reg)1, 0x12345678));
    E("mov w1, #0xffffabcd", a.movi((Reg)1, 0xFFFFABCD));    // movn
    E("mov w1, #0xabcdffff", a.movi((Reg)1, 0xABCDFFFF));    // movn, lsl 16
    E("mov w1, #0x56780000", a.movi((Reg)1, 0x56780000));    // movz, lsl 16
    E("mrs x0, nzcv", a.mrs_nzcv((Reg)0));
    E("msr nzcv, x0", a.msr_nzcv((Reg)0));
    E("cset w1, eq", a.cset((Reg)1, EQ));
    E("cset w1, hi", a.cset((Reg)1, HI));
    E("ldr w28, [x29, #8]", a.ldr((Reg)28, (Reg)29, 8));
    E("str w19, [x29, #60]", a.str((Reg)19, (Reg)29, 60));
    E("ldr x1, [x29, #4088]", a.ldrx((Reg)1, (Reg)29, 4088));
    E("str x30, [sp, #8]", a.strx((Reg)30, SP, 8));
    E("strb w0, [x29, #100]", a.strb((Reg)0, (Reg)29, 100));
    E("ldrb w0, [x29, #4095]", a.ldrb((Reg)0, (Reg)29, 4095));
    E("strh w0, [x29, #50]", a.strh((Reg)0, (Reg)29, 50));
    E("br x16", a.br((Reg)16));
    E("blr x16", a.blr((Reg)16));
    E("ret", a.ret());
    E("nop", a.nop());
    E("brk #0", a.brk());
    E("b #8", a.put(A64::b_(2)));
    E("b #-4", a.put(A64::b_(-1)));
    E("bl #4096", a.put(A64::bl_(1024)));
    E("b.ne #12", a.put(A64::bcond_(NE, 3)));
    E("b.le #-8", a.put(A64::bcond_(LE, -2)));
    E("cbz w3, #16", a.put(A64::cbz_((Reg)3, 4)));
    E("cbnz w3, #-16", a.put(A64::cbnz_((Reg)3, -4)));
    // a forward fixup: b.eq over two nops lands 12 bytes on
    E("b.eq #12\nnop\nnop", { auto f = a.bcond_fwd(EQ); a.nop(); a.nop(); a.land(f); });
    E("add x16, x16, x17, lsl #2", a.addx((Reg)16, (Reg)16, (Reg)17, LSL, 2));
    E("cmp w17, #2", a.cmpi((Reg)17, 2));
    E("sxtb w0, w0", a.sxtb((Reg)0, (Reg)0));
    E("sxth w0, w1", a.sxth((Reg)0, (Reg)1));
    E("csel w17, w17, w16, hi", a.csel((Reg)17, (Reg)17, (Reg)16, HI));
    E("ldr w0, [x2, x17]", a.ldr_reg((Reg)0, (Reg)2, (Reg)17));
    E("ldrh w0, [x2, x17]", a.ldrh_reg((Reg)0, (Reg)2, (Reg)17));
    E("ldrb w17, [x16, x17]", a.ldrb_reg((Reg)17, (Reg)16, (Reg)17));
    E("cbnz w3, #8", a.put(A64::cbnz_((Reg)3, 2)));
    E("cmp x17, x0", a.cmpx((Reg)17, (Reg)0));
    E("add x17, x17, w28, sxtw", a.addx_sxtw((Reg)17, (Reg)17, (Reg)28));
    E("ldr x16, [x16, x17, lsl #3]", a.ldrx_reg_lsl3((Reg)16, (Reg)16, (Reg)17));
    E("lsr x17, x16, #32", a.lsrxi((Reg)17, (Reg)16, 32));
    return 0;
}
