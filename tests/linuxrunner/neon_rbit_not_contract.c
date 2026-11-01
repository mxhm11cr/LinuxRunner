// RBIT, NOT and CNT (vector) differ only in U and size, so a decoder that does
// not check size runs NOT for RBIT. NSS (Firefox) bit-reflects GCM blocks with
// RBIT: wrong results there failed every TLS record (SSL_ERROR_BAD_MAC_READ).
// Checked against scalar references, .8B and .16B, in a loop hot enough for
// the JIT. Build: gcc -O2 neon_rbit_not_contract.c
// Prints NEON_RBIT_NOT_CONTRACT PASS.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t rbit8(uint8_t v) { uint8_t r = 0; for (int i = 0; i < 8; i++) if (v & (1u << i)) r |= 0x80u >> i; return r; }
static uint8_t cnt8(uint8_t v) { int n = 0; for (; v; v &= v - 1) n++; return (uint8_t) n; }

int main(void) {
    uint64_t seed = 0x243f6a8885a308d3ull;
    int fails = 0;
    for (int round = 0; round < 20000 && fails < 4; round++) {
        uint8_t in[16], out[5][16];
        for (int i = 0; i < 16; i += 8) {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            memcpy(in + i, &seed, 8);
        }
        __asm__ volatile(
            "ldr q0, [%[in]]\n"
            "rbit v1.16b, v0.16b\n str q1, [%[o0]]\n"
            "mvn v2.16b, v0.16b\n  str q2, [%[o1]]\n"
            "cnt v3.16b, v0.16b\n  str q3, [%[o2]]\n"
            "movi v4.2d, #-1\n rbit v4.8b, v0.8b\n str q4, [%[o3]]\n"
            "movi v5.2d, #-1\n mvn v5.8b, v0.8b\n  str q5, [%[o4]]\n"
            :
            : [in] "r"(in), [o0] "r"(out[0]), [o1] "r"(out[1]), [o2] "r"(out[2]), [o3] "r"(out[3]), [o4] "r"(out[4])
            : "v0", "v1", "v2", "v3", "v4", "v5", "memory");
        for (int i = 0; i < 16; i++) {
            const char *bad = NULL;
            if (out[0][i] != rbit8(in[i])) bad = "RBIT .16B";
            else if (out[1][i] != (uint8_t) ~in[i]) bad = "NOT .16B";
            else if (out[2][i] != cnt8(in[i])) bad = "CNT .16B";
            else if (out[3][i] != (i < 8 ? rbit8(in[i]) : 0)) bad = "RBIT .8B (upper half must be zero)";
            else if (out[4][i] != (i < 8 ? (uint8_t) ~in[i] : 0)) bad = "NOT .8B (upper half must be zero)";
            if (bad) {
                printf("NEON_RBIT_NOT_CONTRACT FAIL %s byte %d in=%02x\n", bad, i, in[i]);
                fails++;
                break;
            }
        }
    }
    if (!fails) puts("NEON_RBIT_NOT_CONTRACT PASS");
    return fails != 0;
}
