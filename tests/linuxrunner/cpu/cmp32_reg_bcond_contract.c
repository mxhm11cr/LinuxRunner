// Contract for the fused "CMP/SUBS Wn, Wm + B.cond" interpreter path.
// Every condition is checked on edge-case operand pairs, with garbage in the
// upper 32 bits of the source registers. The printed digest must equal the
// one produced by the same program on a native ARM64 CPU.
//   gcc -O1 cmp32_reg_bcond_contract.c -o cmp32 && ./cmp32
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#define TEST(cond)                                                              \
    static unsigned cmp_##cond(uint64_t a, uint64_t b) {                        \
        unsigned r;                                                             \
        __asm__ volatile("cmp %w1, %w2\n\tb." #cond " 1f\n\tmov %w0, #0\n\t"    \
                         "b 2f\n1:\tmov %w0, #1\n2:"                            \
                         : "=r"(r) : "r"(a), "r"(b) : "cc");                    \
        return r;                                                               \
    }                                                                           \
    static unsigned subs_##cond(uint64_t a, uint64_t b, uint64_t *d) {          \
        unsigned r; uint64_t out = 0xdeadbeefcafef00dULL;                       \
        __asm__ volatile("subs %w1, %w2, %w3\n\tb." #cond " 1f\n\t"             \
                         "mov %w0, #0\n\tb 2f\n1:\tmov %w0, #1\n2:"             \
                         : "=&r"(r), "+&r"(out) : "r"(a), "r"(b) : "cc");       \
        *d = out;                                                               \
        return r;                                                               \
    }
#define CONDS(X) X(eq) X(ne) X(cs) X(cc) X(mi) X(pl) X(vs) X(vc) X(hi) X(ls) X(ge) X(lt) X(gt) X(le)
CONDS(TEST)

int main(void) {
    static const uint64_t lo[] = {0, 1, 2, 0x7fffffff, 0x80000000, 0x80000001,
                                  0xfffffffe, 0xffffffff, 0x12345678, 0x9abcdef0};
    static const uint64_t hi[] = {0, 0xffffffff00000000ULL, 0x8000000100000000ULL};
    uint64_t digest = 1469598103934665603ULL;   // FNV-1a
    unsigned checks = 0;
#define MIX(v) do { digest = (digest ^ (uint64_t)(v)) * 1099511628211ULL; } while (0)
    for (unsigned i = 0; i < sizeof lo / sizeof *lo; i++)
        for (unsigned j = 0; j < sizeof lo / sizeof *lo; j++)
            for (unsigned h = 0; h < sizeof hi / sizeof *hi; h++) {
                uint64_t a = lo[i] | hi[h], b = lo[j] | hi[(h + 1) % 3], d;
#define RUN(cond) MIX(cmp_##cond(a, b)); MIX(subs_##cond(a, b, &d)); MIX(d); checks += 2;
                CONDS(RUN)
            }
    // Loop shapes compilers emit for int counters.
    for (int n = -3; n < 40; n += 7) { unsigned c = 0; for (int k = 0; k < n; k++) c++; MIX(c); }
    printf("CMP32_REG_BCOND checks=%u digest=%016" PRIx64 "\n", checks, digest);
    return 0;
}
