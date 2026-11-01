// Differential oracle for AdvSIMD floating-point instructions that compilers
// emit for matrix and colour math (by-element FMLA/FMUL, pairwise, estimates,
// conversions). Native ARM64 run is the reference; output must be identical.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static float rf(void) { int k = (int) (next() % 10); if (k == 0) return 0.0f; if (k == 1) return -1.5f;
    return ((float) (int32_t) next() / 2147483648.0f) * (float) (1 << (next() % 12)); }
static double rd(void) { return ((double) (int64_t) next() / 9.2233720368547758e18) * (double) (1 << (next() % 20)); }
static uint64_t hash;
static void mix(const void *p, int n) { const uint8_t *b = p; for (int i = 0; i < n; i++) { hash ^= b[i]; hash *= 0x100000001b3ull; } }

#define VEC3(NAME, ASM, DBL)                                                             \
    static void NAME##_t(void) { hash = 0xcbf29ce484222325ull;                           \
        for (int i = 0; i < 20000; i++) { uint8_t d[16], n[16], m[16];                   \
            if (DBL) { double a[2] = {rd(), rd()}, b[2] = {rd(), rd()}, c[2] = {rd(), rd()}; memcpy(d, a, 16); memcpy(n, b, 16); memcpy(m, c, 16); } \
            else { float a[4] = {rf(), rf(), rf(), rf()}, b[4] = {rf(), rf(), rf(), rf()}, c[4] = {rf(), rf(), rf(), rf()}; memcpy(d, a, 16); memcpy(n, b, 16); memcpy(m, c, 16); } \
            __asm__ volatile("ldr q0, [%0]\n ldr q1, [%1]\n ldr q2, [%2]\n" ASM "\n str q0, [%0]" \
                             :: "r"(d), "r"(n), "r"(m) : "v0", "v1", "v2", "memory"); mix(d, 16); } \
        printf("ORACLE %-14s %016llx\n", #NAME, (unsigned long long) hash); }
VEC3(fmla_e0_4s, "fmla v0.4s, v1.4s, v2.s[0]", 0)
VEC3(fmla_e3_4s, "fmla v0.4s, v1.4s, v2.s[3]", 0)
VEC3(fmls_e1_4s, "fmls v0.4s, v1.4s, v2.s[1]", 0)
VEC3(fmul_e2_4s, "fmul v0.4s, v1.4s, v2.s[2]", 0)
VEC3(fmla_e1_2d, "fmla v0.2d, v1.2d, v2.d[1]", 1)
VEC3(fmul_e0_2d, "fmul v0.2d, v1.2d, v2.d[0]", 1)
VEC3(fmla_s_e2, "fmla s0, s1, v2.s[2]", 0)
VEC3(fmul_s_e3, "fmul s0, s1, v2.s[3]", 0)
VEC3(fmla_4s, "fmla v0.4s, v1.4s, v2.4s", 0)
VEC3(fmls_4s, "fmls v0.4s, v1.4s, v2.4s", 0)
VEC3(fmla_2d, "fmla v0.2d, v1.2d, v2.2d", 1)
VEC3(fmul_4s, "fmul v0.4s, v1.4s, v2.4s", 0)
VEC3(fmulx_4s, "fmulx v0.4s, v1.4s, v2.4s", 0)
VEC3(fdiv_4s, "fdiv v0.4s, v1.4s, v2.4s", 0)
VEC3(fdiv_2d, "fdiv v0.2d, v1.2d, v2.2d", 1)
VEC3(fadd_4s, "fadd v0.4s, v1.4s, v2.4s", 0)
VEC3(fsub_2s, "fsub v0.2s, v1.2s, v2.2s", 0)
VEC3(faddp_4s, "faddp v0.4s, v1.4s, v2.4s", 0)
VEC3(faddp_s, "faddp s0, v1.2s", 0)
VEC3(faddp_d, "faddp d0, v1.2d", 1)
VEC3(fmaxnm_4s, "fmaxnm v0.4s, v1.4s, v2.4s", 0)
VEC3(fminnm_4s, "fminnm v0.4s, v1.4s, v2.4s", 0)
VEC3(fmax_4s, "fmax v0.4s, v1.4s, v2.4s", 0)
VEC3(fmin_2d, "fmin v0.2d, v1.2d, v2.2d", 1)
VEC3(fabd_4s, "fabd v0.4s, v1.4s, v2.4s", 0)
VEC3(frecpe_4s, "frecpe v0.4s, v1.4s", 0)
VEC3(frsqrte_4s, "frsqrte v0.4s, v1.4s", 0)
VEC3(frecps_4s, "frecps v0.4s, v1.4s, v2.4s", 0)
VEC3(frsqrts_4s, "frsqrts v0.4s, v1.4s, v2.4s", 0)
VEC3(fsqrt_4s, "fsqrt v0.4s, v1.4s", 0)
VEC3(fneg_fabs, "fneg v0.4s, v1.4s\n fabs v0.4s, v0.4s", 0)
VEC3(fcvtzs_4s, "fcvtzs v0.4s, v1.4s", 0)
VEC3(fcvtns_4s, "fcvtns v0.4s, v1.4s", 0)
VEC3(scvtf_4s, "scvtf v0.4s, v1.4s", 0)
VEC3(ucvtf_4s, "ucvtf v0.4s, v1.4s", 0)
VEC3(fcvtn_4h, "fcvtn v0.4h, v1.4s", 0)
VEC3(fcvtl_4s, "fcvtl v0.4s, v1.4h", 0)
VEC3(fcvtn_2s, "fcvtn v0.2s, v1.2d", 1)
VEC3(fcvtl_2d, "fcvtl v0.2d, v1.2s", 0)
VEC3(frintn_4s, "frintn v0.4s, v1.4s", 0)
VEC3(frintm_4s, "frintm v0.4s, v1.4s", 0)
VEC3(fcmgt_4s, "fcmgt v0.4s, v1.4s, v2.4s", 0)
VEC3(fcmge_z_4s, "fcmge v0.4s, v1.4s, #0.0", 0)
VEC3(fmaxv_s, "fmaxv s0, v1.4s", 0)
VEC3(fminnmv_s, "fminnmv s0, v1.4s", 0)

int main(void) {
    fmla_e0_4s_t(); fmla_e3_4s_t(); fmls_e1_4s_t(); fmul_e2_4s_t(); fmla_e1_2d_t(); fmul_e0_2d_t(); fmla_s_e2_t(); fmul_s_e3_t();
    fmla_4s_t(); fmls_4s_t(); fmla_2d_t(); fmul_4s_t(); fmulx_4s_t(); fdiv_4s_t(); fdiv_2d_t(); fadd_4s_t(); fsub_2s_t();
    faddp_4s_t(); faddp_s_t(); faddp_d_t(); fmaxnm_4s_t(); fminnm_4s_t(); fmax_4s_t(); fmin_2d_t(); fabd_4s_t();
    frecpe_4s_t(); frsqrte_4s_t(); frecps_4s_t(); frsqrts_4s_t(); fsqrt_4s_t(); fneg_fabs_t();
    fcvtzs_4s_t(); fcvtns_4s_t(); scvtf_4s_t(); ucvtf_4s_t(); fcvtn_4h_t(); fcvtl_4s_t(); fcvtn_2s_t(); fcvtl_2d_t();
    frintn_4s_t(); frintm_4s_t(); fcmgt_4s_t(); fcmge_z_4s_t(); fmaxv_s_t(); fminnmv_s_t();
    return 0;
}
