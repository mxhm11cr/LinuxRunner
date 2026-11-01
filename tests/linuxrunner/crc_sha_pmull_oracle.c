// Differential oracle for the instruction families behind HWCAP_CRC32,
// HWCAP_PMULL, HWCAP_SHA1 and HWCAP_SHA2. Build and run the same file natively
// on an ARM64 host (reference) and in the guest; output must be identical.
// Build with -march=armv8-a+crc+crypto.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t rng = 0x243f6a8885a308d3ull;
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static uint64_t hash;
static void mix(const void *p, int n) { const uint8_t *b = p; for (int i = 0; i < n; i++) { hash ^= b[i]; hash *= 0x100000001b3ull; } }

#define CRC(NAME, W, R)                                                                  \
    static void NAME##_t(void) { hash = 0xcbf29ce484222325ull;                           \
        for (int i = 0; i < 20000; i++) { uint32_t acc = (uint32_t) next(); uint64_t v = next(); uint32_t out; \
            __asm__ volatile(#NAME " %w0, %w1, %" #R "2" : "=r"(out) : "r"(acc), "r"(v)); mix(&out, 4); } \
        printf("ORACLE %-10s %016llx\n", #NAME, (unsigned long long) hash); }
CRC(crc32b, 8, w) CRC(crc32h, 16, w) CRC(crc32w, 32, w) CRC(crc32x, 64, x)
CRC(crc32cb, 8, w) CRC(crc32ch, 16, w) CRC(crc32cw, 32, w) CRC(crc32cx, 64, x)

// Vector ops on three 128-bit registers: d = op(d, n, m), all inputs random.
#define VEC3(NAME, ASM)                                                                  \
    static void NAME##_t(void) { hash = 0xcbf29ce484222325ull;                           \
        for (int i = 0; i < 20000; i++) { uint64_t d[2] = {next(), next()}, n[2] = {next(), next()}, m[2] = {next(), next()}; \
            __asm__ volatile("ldr q0, [%0]\n ldr q1, [%1]\n ldr q2, [%2]\n" ASM "\n str q0, [%0]" \
                             :: "r"(d), "r"(n), "r"(m) : "v0", "v1", "v2", "memory"); mix(d, 16); } \
        printf("ORACLE %-10s %016llx\n", #NAME, (unsigned long long) hash); }
VEC3(pmull8, "pmull v0.8h, v1.8b, v2.8b")
VEC3(pmull2_8, "pmull2 v0.8h, v1.16b, v2.16b")
VEC3(pmull64, "pmull v0.1q, v1.1d, v2.1d")
VEC3(pmull2_64, "pmull2 v0.1q, v1.2d, v2.2d")
VEC3(sha1c, "sha1c q0, s1, v2.4s")
VEC3(sha1p, "sha1p q0, s1, v2.4s")
VEC3(sha1m, "sha1m q0, s1, v2.4s")
VEC3(sha1h, "sha1h s0, s1")
VEC3(sha1su0, "sha1su0 v0.4s, v1.4s, v2.4s")
VEC3(sha1su1, "sha1su1 v0.4s, v1.4s")
VEC3(sha256h, "sha256h q0, q1, v2.4s")
VEC3(sha256h2, "sha256h2 q0, q1, v2.4s")
VEC3(sha256su0, "sha256su0 v0.4s, v1.4s")
VEC3(sha256su1, "sha256su1 v0.4s, v1.4s, v2.4s")
// Destination aliasing a source (common in hand-written SHA loops).
VEC3(sha256h_alias, "sha256h q0, q0, v2.4s")
VEC3(pmull64_alias, "pmull v0.1q, v0.1d, v2.1d")

int main(void) {
    crc32b_t(); crc32h_t(); crc32w_t(); crc32x_t(); crc32cb_t(); crc32ch_t(); crc32cw_t(); crc32cx_t();
    pmull8_t(); pmull2_8_t(); pmull64_t(); pmull2_64_t();
    sha1c_t(); sha1p_t(); sha1m_t(); sha1h_t(); sha1su0_t(); sha1su1_t();
    sha256h_t(); sha256h2_t(); sha256su0_t(); sha256su1_t(); sha256h_alias_t(); pmull64_alias_t();
    return 0;
}
