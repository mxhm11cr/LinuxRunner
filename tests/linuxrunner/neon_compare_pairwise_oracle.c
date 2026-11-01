// Differential oracle for AdvSIMD three-same compare and pairwise ops.
// Build and run the same file natively on an ARM64 host (the reference) and
// inside the guest; the printed lines must be identical. Covers every
// arrangement, Rd aliasing Rn/Rm, Q=0 high-half clearing (Vd is pre-filled
// with a non-zero pattern) and reserved encodings (must raise SIGILL).
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

// Inputs mix random lanes, extremes and lanes copied between operands so that
// equal / greater / less all occur for every element width.
static void fill(uint8_t a[16], uint8_t b[16], int width) {
    for (int i = 0; i < 16; i++) { a[i] = (uint8_t) next(); b[i] = (uint8_t) next(); }
    for (int lane = 0; lane < 16; lane += width) {
        switch (next() % 6) {
            case 0: memcpy(b + lane, a + lane, width); break;
            case 1: memset(a + lane, 0xff, width); break;
            case 2: memset(b + lane, 0, width); break;
            case 3: memset(a + lane, 0x80, width); memset(b + lane, 0x7f, width); break;
            default: break;
        }
    }
}

static uint64_t hash = 0xcbf29ce484222325ull;
static void mix(const uint8_t *p, int n) { for (int i = 0; i < n; i++) { hash ^= p[i]; hash *= 0x100000001b3ull; } }

// out = op(a, b) with Vd pre-filled from d; also the Rd == Rn alias form.
#define CASE(OP, ARR)                                                                   \
    static void OP##_##ARR(uint8_t *out, uint8_t *alias, const uint8_t *a, const uint8_t *b, const uint8_t *d) { \
        __asm__ volatile("ldr q0, [%1]\n ldr q1, [%2]\n ldr q2, [%3]\n"                 \
                         #OP " v2." #ARR ", v0." #ARR ", v1." #ARR "\n str q2, [%0]\n"   \
                         #OP " v0." #ARR ", v0." #ARR ", v1." #ARR "\n str q0, [%4]\n"   \
                         :: "r"(out), "r"(a), "r"(b), "r"(d), "r"(alias)                \
                         : "v0", "v1", "v2", "memory");                                 \
    }
#define ALL7(OP) CASE(OP, 8b) CASE(OP, 16b) CASE(OP, 4h) CASE(OP, 8h) CASE(OP, 2s) CASE(OP, 4s) CASE(OP, 2d)
#define ALL6(OP) CASE(OP, 8b) CASE(OP, 16b) CASE(OP, 4h) CASE(OP, 8h) CASE(OP, 2s) CASE(OP, 4s)
ALL7(cmeq) ALL7(cmhi) ALL7(cmhs) ALL7(cmtst) ALL7(addp)
ALL6(umaxp) ALL6(uminp) ALL6(smaxp) ALL6(sminp)

typedef void (*op_fn)(uint8_t *, uint8_t *, const uint8_t *, const uint8_t *, const uint8_t *);
struct op { const char *name; op_fn fn; int width; };
#define E(OP, ARR, W) {#OP "." #ARR, OP##_##ARR, W}
#define E7(OP) E(OP, 8b, 1), E(OP, 16b, 1), E(OP, 4h, 2), E(OP, 8h, 2), E(OP, 2s, 4), E(OP, 4s, 4), E(OP, 2d, 8)
#define E6(OP) E(OP, 8b, 1), E(OP, 16b, 1), E(OP, 4h, 2), E(OP, 8h, 2), E(OP, 2s, 4), E(OP, 4s, 4)
static const struct op ops[] = { E7(cmeq), E7(cmhi), E7(cmhs), E7(cmtst), E7(addp), E6(umaxp), E6(uminp), E6(smaxp), E6(sminp) };

static sigjmp_buf jump;
static void on_sigill(int sig) { (void) sig; siglongjmp(jump, 1); }
static int raises_sigill(uint32_t insn) {
    static uint32_t code[2] __attribute__((aligned(16)));
    (void) code;
    if (sigsetjmp(jump, 1)) return 1;
    // Executed from a fixed text location: no runtime code generation.
    if (insn == 0x2ee08c00) __asm__ volatile(".inst 0x2ee08c00" ::: "v0");      // CMEQ V0.1D (reserved)
    else if (insn == 0x6ee0a400) __asm__ volatile(".inst 0x6ee0a400" ::: "v0"); // UMAXP V0.2D (reserved)
    return 0;
}

int main(void) {
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        hash = 0xcbf29ce484222325ull;
        for (int n = 0; n < 4000; n++) {
            uint8_t a[16], b[16], d[16], out[16], alias[16];
            fill(a, b, ops[i].width);
            for (int k = 0; k < 16; k++) d[k] = (uint8_t) (0xa5 ^ k);
            ops[i].fn(out, alias, a, b, d);
            mix(out, 16);
            mix(alias, 16);
        }
        printf("ORACLE %-10s %016llx\n", ops[i].name, (unsigned long long) hash);
    }
    struct sigaction sa = {0};
    sa.sa_handler = on_sigill;
    sigaction(SIGILL, &sa, NULL);
    printf("RESERVED cmeq.1d sigill=%d\n", raises_sigill(0x2ee08c00));
    printf("RESERVED umaxp.2d sigill=%d\n", raises_sigill(0x6ee0a400));
    return 0;
}
