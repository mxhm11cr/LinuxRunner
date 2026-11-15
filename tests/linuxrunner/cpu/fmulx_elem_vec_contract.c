// FMULX (vector, by element): OpenJDK 21 C2 emits "fmulx v.2d, v.2d, v.d[i]"
// (insn 0x6fc3925c), which LinuxRunner's interpreter did not decode (SIGILL in
// the VW MIB2 HMI). FMULX equals FMUL except 0 x infinity = +/-2.0.
// Build: gcc -O1 fmulx_elem_vec_contract.c -o fmulx_elem_vec_contract
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
static void check_d(const char *what, double got, double want) {
    if (memcmp(&got, &want, sizeof got) != 0 && !(isnan(got) && isnan(want))) {
        printf("FAIL %s: got %a want %a\n", what, got, want); failures++;
    }
}
static void check_f(const char *what, float got, float want) {
    if (memcmp(&got, &want, sizeof got) != 0) { printf("FAIL %s: got %a want %a\n", what, got, want); failures++; }
}

int main(void) {
    // 2D, element 0 and element 1.
    double n[2] = {1.5, -3.0}, m[2] = {2.0, 0.25}, d0[2], d1[2];
    __asm__ volatile("ldr q1, [%2]\n ldr q2, [%3]\n fmulx v0.2d, v1.2d, v2.d[0]\n str q0, [%0]\n"
                     "fmulx v3.2d, v1.2d, v2.d[1]\n str q3, [%1]\n"
                     :: "r"(d0), "r"(d1), "r"(n), "r"(m) : "v0", "v1", "v2", "v3", "memory");
    check_d("2d[0] lane0", d0[0], 3.0);  check_d("2d[0] lane1", d0[1], -6.0);
    check_d("2d[1] lane0", d1[0], 0.375); check_d("2d[1] lane1", d1[1], -0.75);
    // FMULX special case: 0 x inf = +2, -0 x inf = -2 (FMUL would give NaN).
    double zn[2] = {0.0, -0.0}, inf[2] = {INFINITY, 1.0}, z[2];
    __asm__ volatile("ldr q1, [%1]\n ldr q2, [%2]\n fmulx v0.2d, v1.2d, v2.d[0]\n str q0, [%0]\n"
                     :: "r"(z), "r"(zn), "r"(inf) : "v0", "v1", "v2", "memory");
    check_d("0*inf", z[0], 2.0); check_d("-0*inf", z[1], -2.0);
    // 4S, element 3.
    float fn[4] = {1, 2, 3, 4}, fm[4] = {0, 0, 0, 0.5f}, fd[4];
    __asm__ volatile("ldr q1, [%1]\n ldr q2, [%2]\n fmulx v0.4s, v1.4s, v2.s[3]\n str q0, [%0]\n"
                     :: "r"(fd), "r"(fn), "r"(fm) : "v0", "v1", "v2", "memory");
    for (int i = 0; i < 4; i++) check_f("4s[3]", fd[i], fn[i] * 0.5f);
    puts(failures ? "FMULX_ELEM_VEC_FAIL" : "FMULX_ELEM_VEC_PASS");
    return failures != 0;
}
