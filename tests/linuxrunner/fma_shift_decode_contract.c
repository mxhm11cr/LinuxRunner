// FMSUB/FNMSUB with addend V9 must not collide with SSHLL/USHLL.
// Native AArch64 oracle and interpreter contract; no Qt dependency.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static unsigned cases, failures;
static void fp(const char *name, const uint64_t actual[2], double expected, int single) {
    uint64_t bits = 0;
    if (single) { float f = expected; uint32_t u; memcpy(&u, &f, 4); bits = u; }
    else memcpy(&bits, &expected, 8);
    cases++;
    if (actual[0] != bits || actual[1] != 0) {
        printf("FMA_SHIFT_DECODE FAIL %s actual=%016llx:%016llx expected=%016llx:0\n", name,
            (unsigned long long)actual[0], (unsigned long long)actual[1], (unsigned long long)bits);
        failures++;
    }
}
#define FP(OP, PREC, R, EXPECTED, SINGLE) do { \
    uint64_t out[2]; \
    __asm__ volatile("movi v" #R ".16b,#0xff\n" \
        "fmov " #PREC "29,#2.0\n fmov " #PREC "27,#3.0\n fmov " #PREC "9,#10.0\n" \
        #OP " " #PREC #R "," #PREC "29," #PREC "27," #PREC "9\n" \
        "str q" #R ",[%0]" ::"r"(out): \
        "v0","v1","v2","v3","v4","v5","v6","v7","v8","v9","v10","v11","v12","v13","v14","v15", \
        "v16","v17","v18","v19","v20","v21","v22","v23","v24","v25","v26","v27","v28","v29","v30","v31","memory"); \
    fp(#OP "-" #PREC #R, out, EXPECTED, SINGLE); \
} while (0)
#define REGISTER(R) do { \
    FP(fmadd,s,R,16,1); FP(fmsub,s,R,4,1); FP(fnmadd,s,R,-16,1); FP(fnmsub,s,R,-4,1); \
    FP(fmadd,d,R,16,0); FP(fmsub,d,R,4,0); FP(fnmadd,d,R,-16,0); FP(fnmsub,d,R,-4,0); \
} while (0)
static void widened(const char *name, const uint8_t src[16], const uint8_t out[16], unsigned bits, int upper, int unsign, unsigned shift) {
    unsigned width=bits/8, lanes=16/(2*width), first=upper?lanes:0;
    for (unsigned i=0;i<lanes;i++) {
        uint64_t x=0,got=0; memcpy(&x,src+(first+i)*width,width); memcpy(&got,out+i*width*2,width*2);
        int64_t value=(int64_t)x;
        if (!unsign && (x & (1ULL<<(bits-1)))) value-=(1LL<<bits);
        uint64_t expected=(uint64_t)value<<shift;
        if (bits<32) expected&=(1ULL<<(bits*2))-1;
        cases++; if(got!=expected){printf("FMA_SHIFT_DECODE FAIL %s lane=%u actual=%llx expected=%llx\n",name,i,(unsigned long long)got,(unsigned long long)expected);failures++;}
    }
}
#define LONG(OP,DEST,SRC,BITS,UPPER,U,SHIFT) do { \
    uint8_t out[16]; __asm__ volatile("ldr q1,[%1]\n" #OP " v0." #DEST ",v1." #SRC ",#" #SHIFT "\n str q0,[%0]"::"r"(out),"r"(input):"v0","v1","memory"); \
    widened(#OP "-" #SRC,input,out,BITS,UPPER,U,SHIFT); \
} while(0)
int main(void) {
    REGISTER(0); REGISTER(1); REGISTER(2); REGISTER(3); REGISTER(4); REGISTER(5); REGISTER(6); REGISTER(7);
    REGISTER(8); REGISTER(9); REGISTER(10); REGISTER(11); REGISTER(12); REGISTER(13); REGISTER(14); REGISTER(15);
    REGISTER(16); REGISTER(17); REGISTER(18); REGISTER(19); REGISTER(20); REGISTER(21); REGISTER(22); REGISTER(23);
    REGISTER(24); REGISTER(25); REGISTER(26); REGISTER(27); REGISTER(28); REGISTER(29); REGISTER(30); REGISTER(31);
    const uint8_t input[16]={0x80,0xff,0,0x7f,0x10,0x99,0x33,0xee,0x42,0x81,0xff,0,0x12,0x34,0x56,0x87};
    LONG(sshll,8h,8b,8,0,0,0); LONG(sshll2,8h,16b,8,1,0,7);
    LONG(ushll,8h,8b,8,0,1,3); LONG(ushll2,8h,16b,8,1,1,7);
    LONG(sshll,4s,4h,16,0,0,0); LONG(sshll2,4s,8h,16,1,0,15);
    LONG(ushll,4s,4h,16,0,1,7); LONG(ushll2,4s,8h,16,1,1,15);
    LONG(sshll,2d,2s,32,0,0,0); LONG(sshll2,2d,4s,32,1,0,31);
    LONG(ushll,2d,2s,32,0,1,17); LONG(ushll2,2d,4s,32,1,1,31);
    printf("FMA_SHIFT_DECODE %s cases=%u failures=%u\n",failures?"FAIL":"PASS",cases,failures);
    return failures!=0;
}
