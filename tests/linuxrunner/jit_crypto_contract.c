// ARMv8 cryptographic instructions (AESE/AESMC/AESD/AESIMC, SHA256H/H2/SU0/
// SU1, SHA1C/H/SU0/SU1) against published vectors, in a loop hot enough to be
// translated by the JIT: FIPS-197 C.1 (AES-128), SHA-256("abc") and
// SHA-1("abc") computed with the instructions directly.
// Build: gcc -O2 -march=armv8-a+crypto jit_crypto_contract.c   Prints JIT_CRYPTO_CONTRACT PASS.
#include <arm_neon.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fails;
static void check(const char *what, const uint8_t *got, const uint8_t *want, size_t n) {
    if (memcmp(got, want, n)) { printf("JIT_CRYPTO_CONTRACT FAIL %s\n", what); fails++; }
}

static void aes128_keys(const uint8_t key[16], uint8x16_t rk[11]) {
    static const uint8_t rcon[10] = {1, 2, 4, 8, 16, 32, 64, 128, 27, 54};
    uint32_t w[44];
    for (int i = 0; i < 4; i++) w[i] = (uint32_t) key[4 * i] | key[4 * i + 1] << 8 | key[4 * i + 2] << 16 | (uint32_t) key[4 * i + 3] << 24;
    for (int i = 4; i < 44; i++) {
        uint32_t t = w[i - 1];
        if (i % 4 == 0) {
            // SubWord(RotWord(t)) with AESE on a zero key: ShiftRows does not move row 0 bytes of column 0.
            uint8x16_t v = vreinterpretq_u8_u32(vdupq_n_u32(t));
            v = vaeseq_u8(v, vdupq_n_u8(0));
            t = vgetq_lane_u32(vreinterpretq_u32_u8(v), 0);
            t = (t >> 8 | t << 24) ^ rcon[i / 4 - 1];
        }
        w[i] = w[i - 4] ^ t;
    }
    for (int r = 0; r < 11; r++) rk[r] = vreinterpretq_u8_u32(vld1q_u32(&w[4 * r]));
}
static void aes128_encrypt(const uint8x16_t rk[11], const uint8_t in[16], uint8_t out[16]) {
    uint8x16_t s = vld1q_u8(in);
    for (int r = 0; r < 9; r++) s = vaesmcq_u8(vaeseq_u8(s, rk[r]));
    s = veorq_u8(vaeseq_u8(s, rk[9]), rk[10]);
    vst1q_u8(out, s);
}
static void aes128_decrypt(const uint8x16_t rk[11], const uint8_t in[16], uint8_t out[16]) {
    uint8x16_t s = vld1q_u8(in);
    s = vaesdq_u8(s, rk[10]);
    for (int r = 9; r > 0; r--) s = vaesdq_u8(vaesimcq_u8(s), vaesimcq_u8(rk[r]));
    s = veorq_u8(s, rk[0]);
    vst1q_u8(out, s);
}

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
static void sha256_abc(uint8_t digest[32]) {
    uint8_t block[64] = {'a', 'b', 'c', 0x80}; block[63] = 24;
    uint32x4_t abcd = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a}, efgh = {0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint32x4_t m[4];
    for (int i = 0; i < 4; i++) m[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + 16 * i)));
    uint32x4_t a0 = abcd, e0 = efgh;
    for (int i = 0; i < 16; i++) {
        uint32x4_t wk = vaddq_u32(m[i % 4], vld1q_u32(&K256[4 * i]));
        uint32x4_t prev = abcd;
        abcd = vsha256hq_u32(abcd, efgh, wk);
        efgh = vsha256h2q_u32(efgh, prev, wk);
        if (i < 12) m[i % 4] = vsha256su1q_u32(vsha256su0q_u32(m[i % 4], m[(i + 1) % 4]), m[(i + 2) % 4], m[(i + 3) % 4]);
    }
    abcd = vaddq_u32(abcd, a0); efgh = vaddq_u32(efgh, e0);
    vst1q_u8(digest, vrev32q_u8(vreinterpretq_u8_u32(abcd)));
    vst1q_u8(digest + 16, vrev32q_u8(vreinterpretq_u8_u32(efgh)));
}
static void sha1_abc(uint8_t digest[20]) {
    uint8_t block[64] = {'a', 'b', 'c', 0x80}; block[63] = 24;
    uint32x4_t abcd = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476}, a0 = abcd;
    uint32_t e = 0xc3d2e1f0, e0 = e;
    uint32x4_t m[4];
    for (int i = 0; i < 4; i++) m[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + 16 * i)));
    static const uint32_t k[4] = {0x5a827999, 0x6ed9eba1, 0x8f1bbcdc, 0xca62c1d6};
    for (int i = 0; i < 20; i++) {
        uint32x4_t wk = vaddq_u32(m[i % 4], vdupq_n_u32(k[i / 5]));
        uint32_t ne = vsha1h_u32(vgetq_lane_u32(abcd, 0));
        if (i < 5) abcd = vsha1cq_u32(abcd, e, wk);
        else if (i < 10 || i >= 15) abcd = vsha1pq_u32(abcd, e, wk);
        else abcd = vsha1mq_u32(abcd, e, wk);
        e = ne;
        if (i < 16) m[i % 4] = vsha1su1q_u32(vsha1su0q_u32(m[i % 4], m[(i + 1) % 4], m[(i + 2) % 4]), m[(i + 3) % 4]);
    }
    abcd = vaddq_u32(abcd, a0); e += e0;
    vst1q_u8(digest, vrev32q_u8(vreinterpretq_u8_u32(abcd)));
    uint32_t be = __builtin_bswap32(e); memcpy(digest + 16, &be, 4);
}

int main(void) {
    static const uint8_t key[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    static const uint8_t plain[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
    static const uint8_t cipher[16] = {0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a};
    static const uint8_t sha256[32] = {0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    static const uint8_t sha1[20] = {0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d};
    uint8x16_t rk[11];
    for (int round = 0; round < 20000; round++) {   // hot: translated after the first passes
        uint8_t out[32], back[16];
        aes128_keys(key, rk);
        aes128_encrypt(rk, plain, out); check("AES-128 encrypt (FIPS-197 C.1)", out, cipher, 16);
        aes128_decrypt(rk, cipher, back); check("AES-128 decrypt", back, plain, 16);
        sha256_abc(out); check("SHA-256(abc)", out, sha256, 32);
        sha1_abc(out); check("SHA-1(abc)", out, sha1, 20);
        if (fails) break;
    }
    if (!fails) puts("JIT_CRYPTO_CONTRACT PASS");
    return fails != 0;
}
