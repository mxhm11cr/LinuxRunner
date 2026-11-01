// Differential oracle for scalar floating-point instructions used by libm
// (glibc pow/exp/log, Qt/KWin colour math): fused multiply-add family,
// rounding, conversions, comparisons and selects, FPCR rounding modes.
// Native ARM64 is the reference; output must be identical.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t rng = 0xd1b54a32d192ed03ull;
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static double rd(void) { int k = (int) (next() % 12); if (k == 0) return 0.0; if (k == 1) return -0.0; if (k == 2) return 1e-310;
    return ((double) (int64_t) next() / 9.2233720368547758e18) * (double) (1ull << (next() % 40)); }
static uint64_t hash;
static void mix(const void *p, int n) { const uint8_t *b = p; for (int i = 0; i < n; i++) { hash ^= b[i]; hash *= 0x100000001b3ull; } }

#define D3(NAME, ASM)                                                                    \
    static void NAME##_t(void) { hash = 0xcbf29ce484222325ull;                           \
        for (int i = 0; i < 30000; i++) { double a = rd(), b = rd(), c = rd(), r; uint64_t x;          \
            __asm__ volatile("fmov d1, %1\n fmov d2, %2\n fmov d3, %3\n" ASM "\n fmov %0, d0\n mrs %4, fpsr" \
                             : "=r"(r), "+r"(a), "+r"(b), "+r"(c), "=r"(x) :: "d0", "d1", "d2", "d3", "x9", "x10", "cc"); \
            mix(&r, 8); } \
        printf("ORACLE %-14s %016llx\n", #NAME, (unsigned long long) hash); }
D3(fmadd_d, "fmadd d0, d1, d2, d3")
D3(fmsub_d, "fmsub d0, d1, d2, d3")
D3(fnmadd_d, "fnmadd d0, d1, d2, d3")
D3(fnmsub_d, "fnmsub d0, d1, d2, d3")
D3(fmadd_s, "fcvt s1, d1\n fcvt s2, d2\n fcvt s3, d3\n fmadd s0, s1, s2, s3\n fcvt d0, s0")
D3(fnmul_d, "fnmul d0, d1, d2")
D3(fdiv_d, "fdiv d0, d1, d2")
D3(fsqrt_d, "fabs d1, d1\n fsqrt d0, d1")
D3(frintx_d, "frintx d0, d1")
D3(frinta_d, "frinta d0, d1")
D3(frintp_d, "frintp d0, d1")
D3(frintm_d, "frintm d0, d1")
D3(frintz_d, "frintz d0, d1")
D3(frintn_d, "frintn d0, d1")
D3(fcvtzs_x, "fcvtzs x9, d1\n fmov d0, x9")
D3(fcvtzu_w, "fcvtzu w9, d1\n fmov s0, w9")
D3(fcvtas_x, "fcvtas x9, d1\n fmov d0, x9")
D3(fcvtms_x, "fcvtms x9, d1\n fmov d0, x9")
D3(scvtf_d, "fmov x9, d1\n scvtf d0, x9")
D3(ucvtf_s, "fmov x9, d1\n ucvtf s0, x9\n fcvt d0, s0")
D3(fcvt_h, "fcvt h0, d1\n fcvt d0, h0")
D3(fcsel_gt, "fcmp d1, d2\n fcsel d0, d1, d3, gt")
D3(fcsel_vs, "fcmp d1, d1\n fcsel d0, d2, d3, vs")
D3(fccmp_d, "fcmp d1, d2\n fccmp d2, d3, #4, ge\n fcsel d0, d1, d3, eq")
D3(fmaxnm_d, "fmaxnm d0, d1, d2")
D3(fmin_d, "fmin d0, d1, d2")
D3(fabd_d, "fabd d0, d1, d2")
D3(fmov_imm, "fmov d0, #-1.25\n fmul d0, d0, d1")
D3(frecpe_d, "frecpe d0, d1")
D3(frsqrte_d, "frsqrte d0, d1")
D3(frecpe_s, "fcvt s1, d1\n frecpe s0, s1\n fcvt d0, s0")
D3(frsqrte_s, "fcvt s1, d1\n frsqrte s0, s1\n fcvt d0, s0")
D3(frecpx_d, "frecpx d0, d1")
D3(frecpx_s, "fcvt s1, d1\n frecpx s0, s1\n fcvt d0, s0")
// Rounding mode from FPCR (glibc saves/sets/restores it).
D3(fpcr_rp_add, "mrs x9, fpcr\n orr x10, x9, #(1 << 22)\n msr fpcr, x10\n fadd d0, d1, d2\n fmul d0, d0, d3\n msr fpcr, x9")
D3(fpcr_rz_madd, "mrs x9, fpcr\n orr x10, x9, #(3 << 22)\n msr fpcr, x10\n fmadd d0, d1, d2, d3\n msr fpcr, x9")

int main(void) {
    fmadd_d_t(); fmsub_d_t(); fnmadd_d_t(); fnmsub_d_t(); fmadd_s_t(); fnmul_d_t(); fdiv_d_t(); fsqrt_d_t();
    frintx_d_t(); frinta_d_t(); frintp_d_t(); frintm_d_t(); frintz_d_t(); frintn_d_t();
    fcvtzs_x_t(); fcvtzu_w_t(); fcvtas_x_t(); fcvtms_x_t(); scvtf_d_t(); ucvtf_s_t(); fcvt_h_t();
    fcsel_gt_t(); fcsel_vs_t(); fccmp_d_t(); fmaxnm_d_t(); fmin_d_t(); fabd_d_t(); fmov_imm_t(); frecpe_d_t(); frsqrte_d_t(); frecpe_s_t(); frsqrte_s_t(); frecpx_d_t(); frecpx_s_t();
    fpcr_rp_add_t(); fpcr_rz_madd_t();
    return 0;
}
