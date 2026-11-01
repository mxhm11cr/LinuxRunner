// interp_bench.c — ARM64 interpreter microbenchmarks (LinuxRunner Engine work).
//
// Each workload isolates one cost of the gadget executor: ALU dispatch, flags/branches, loads/stores and
// the software TLB, function calls and returns, indirect calls, FP, NEON, atomics, syscalls and futexes.
// The same source builds for the guest (static aarch64 Linux, gcc -O2) and natively on macOS (clang -O2),
// so results can be reported as ns per iteration and as a slowdown factor against native.
//
//   interp_bench [scale] [filter]      scale multiplies every iteration count (default 1)
// Output: one line per workload: "BENCH <name> iters=<n> ms=<t> ns_per_iter=<x> check=<c>"
// "check" is a workload checksum: it must be identical across runtimes (correctness guard).
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#if defined(__aarch64__) || defined(__arm64__)
#include <arm_neon.h>
#endif

#define NOINLINE __attribute__((noinline))
static volatile uint64_t sink;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

// ALU: dependent chain of add/eor/shift/mul (data-processing gadgets, no memory, no branches).
static uint64_t w_alu(uint64_t n) {
    uint64_t a = 0x9e3779b97f4a7c15ull, b = 12345, c = 0;
    for (uint64_t i = 0; i < n; i++) {
        a += b ^ (c >> 3);
        b = (b << 7) | (b >> 57);
        c ^= a * 0x2545f4914f6cdd1dull;
        c += i;
    }
    return a ^ b ^ c;
}

// Branch: data-dependent conditional branches and compares (flags, b.cond, csel).
static uint64_t w_branch(uint64_t n) {
    uint64_t x = 88172645463325252ull, cnt = 0;
    for (uint64_t i = 0; i < n; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        if (x & 1) cnt += 3;
        else if (x & 2) cnt ^= i;
        else cnt--;
    }
    return cnt;
}

// Loads/stores: sequential sum and update over a 256 KiB buffer (memory gadgets, TLB hits).
static uint64_t w_mem_seq(uint64_t n) {
    enum { N = 32768 };
    static uint64_t buf[N];
    for (int i = 0; i < N; i++) buf[i] = i;
    uint64_t s = 0;
    for (uint64_t r = 0; r < n; r++)
        for (int i = 0; i < N; i++) { s += buf[i]; buf[i] = s; }
    return s;
}

// Random access: pointer chase over 64 MiB (16384 pages: software TLB misses). The permutation is built
// before timing starts (setup_mem_rand); only the chase is timed.
enum { RAND_N = 8u << 20 };
static uint32_t *rand_next;
static void setup_mem_rand(void) {
    rand_next = malloc(RAND_N * sizeof *rand_next);
    for (uint32_t i = 0; i < RAND_N; i++) rand_next[i] = i;
    uint64_t x = 0x2545f4914f6cdd1dull;
    for (uint32_t i = RAND_N - 1; i > 0; i--) {   // Sattolo: a single cycle
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        uint32_t j = x % i, t = rand_next[i]; rand_next[i] = rand_next[j]; rand_next[j] = t;
    }
}
static uint64_t w_mem_rand(uint64_t n) {
    uint32_t p = 0;
    for (uint64_t i = 0; i < n; i++) p = rand_next[p];
    return p;
}

// Calls: non-inlined call/return with stack frames (bl, ret, stp/ldp).
NOINLINE static uint64_t leaf(uint64_t a, uint64_t b) { return (a * 3) ^ b; }
NOINLINE static uint64_t mid(uint64_t a, uint64_t b) { return leaf(a, b) + leaf(b, a); }
static uint64_t w_call(uint64_t n) {
    uint64_t s = 1;
    for (uint64_t i = 0; i < n; i++) s = mid(s, i);
    return s;
}

// Indirect calls through a table (blr: block-cache lookups on every call).
NOINLINE static uint64_t f0(uint64_t x) { return x + 1; }
NOINLINE static uint64_t f1(uint64_t x) { return x ^ 0x55; }
NOINLINE static uint64_t f2(uint64_t x) { return x * 3; }
NOINLINE static uint64_t f3(uint64_t x) { return x >> 1 | 1; }
static uint64_t (*volatile table[4])(uint64_t) = { f0, f1, f2, f3 };
static uint64_t w_indirect(uint64_t n) {
    uint64_t s = 7;
    for (uint64_t i = 0; i < n; i++) s = table[(s ^ i) & 3](s);
    return s;
}

// Floating point: dependent fmadd/fdiv mix (FP gadgets).
static uint64_t w_float(uint64_t n) {
    double a = 1.0001, b = 0.9999, c = 0.5;
    for (uint64_t i = 0; i < n; i++) {
        c = c * 0.999999 + a * b;
        a = a * 0.9999 + 0.0001;
        b = c / (b + 1.0);
    }
    uint64_t r; memcpy(&r, &c, 8);
    return r >> 20;
}

// NEON: 128-bit integer vector arithmetic over a 16 KiB buffer.
static uint64_t w_neon(uint64_t n) {
#if defined(__aarch64__) || defined(__arm64__)
    static uint32_t v[4096] __attribute__((aligned(16)));
    for (int i = 0; i < 4096; i++) v[i] = i * 2654435761u;
    uint32x4_t acc = vdupq_n_u32(1);
    for (uint64_t r = 0; r < n; r++)
        for (int i = 0; i < 4096; i += 4) {
            uint32x4_t x = vld1q_u32(v + i);
            acc = vaddq_u32(vmulq_u32(acc, vdupq_n_u32(3)), veorq_u32(x, vshrq_n_u32(acc, 5)));
        }
    return vgetq_lane_u32(acc, 0) ^ vgetq_lane_u32(acc, 3);
#else
    return n;
#endif
}

// libc string routines (glibc/musl/macOS optimized memcpy, memset, strlen).
static uint64_t w_string(uint64_t n) {
    static char a[65536], b[65536];
    memset(a, 'x', sizeof a - 1);
    uint64_t s = 0;
    for (uint64_t i = 0; i < n; i++) {
        memcpy(b, a, sizeof a);
        b[(i * 977) % (sizeof b - 1)] = 0;
        s += strlen(b);
        memset(a + (i & 1023), 'y', 64);
    }
    return s;
}

// Short-string search as parsers and interpreters do it: strlen, strchr,
// memchr and memcmp on 16..256-byte strings. glibc's AArch64 versions are
// built on CMEQ/UMAXP/ADDP/SHRN; memcpy-dominated "string" does not show them.
static uint64_t w_strsearch(uint64_t n) {
    static char text[4096];
    for (size_t i = 0; i < sizeof text - 1; i++) text[i] = 'a' + (char)(i * 7 % 26);
    text[sizeof text - 1] = 0;
    uint64_t s = 0;
    for (uint64_t i = 0; i < n; i++) {
        size_t off = (i * 131) % 3584, len = 16 + (i * 37) % 241;
        char saved = text[off + len]; text[off + len] = 0;
        s += strlen(text + off);
        const char *c = strchr(text + off, 'z'); s += c ? (uint64_t)(c - text) : 1;
        const void *m = memchr(text + off, 'q', len); s += m ? (uint64_t)((const char *)m - text) : 3;
        s += (uint64_t)(memcmp(text + off, text + ((off + 26) % 3584), len) & 0xff);
        text[off + len] = saved;
    }
    return s;
}

// Atomics: uncontended atomic add and compare-exchange (ldxr/stxr or LSE).
static uint64_t w_atomic(uint64_t n) {
    static uint64_t c;
    uint64_t expect = 0;
    for (uint64_t i = 0; i < n; i++) {
        __atomic_fetch_add(&c, 1, __ATOMIC_SEQ_CST);
        expect = __atomic_load_n(&c, __ATOMIC_ACQUIRE);
        __atomic_compare_exchange_n(&c, &expect, expect + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    return c;
}

// Uncontended mutex lock/unlock (futex-free fast path in libc).
static uint64_t w_mutex(uint64_t n) {
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    uint64_t s = 0;
    for (uint64_t i = 0; i < n; i++) { pthread_mutex_lock(&m); s += i; pthread_mutex_unlock(&m); }
    return s;
}

// Syscall round trip (getppid: no work in the kernel personality).
static uint64_t w_syscall(uint64_t n) {
    uint64_t s = 0;
    for (uint64_t i = 0; i < n; i++) s += (uint64_t)getppid() & 1;
    return s + 1;
}

// Thread ping-pong through a mutex and condition variable (futex wait/wake).
static pthread_mutex_t pm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pc = PTHREAD_COND_INITIALIZER;
static uint64_t turn, rounds;
static void *pong(void *arg) {
    (void)arg;
    pthread_mutex_lock(&pm);
    for (uint64_t i = 0; i < rounds; i++) {
        while (turn != 1) pthread_cond_wait(&pc, &pm);
        turn = 0; pthread_cond_signal(&pc);
    }
    pthread_mutex_unlock(&pm);
    return NULL;
}
static uint64_t w_pingpong(uint64_t n) {
    pthread_t t; rounds = n; turn = 0;
    pthread_create(&t, NULL, pong, NULL);
    pthread_mutex_lock(&pm);
    for (uint64_t i = 0; i < n; i++) {
        turn = 1; pthread_cond_signal(&pc);
        while (turn != 0) pthread_cond_wait(&pc, &pm);
    }
    pthread_mutex_unlock(&pm);
    pthread_join(t, NULL);
    return n;
}

struct workload { const char *name; uint64_t (*fn)(uint64_t); uint64_t iters; void (*setup)(void); };
static const struct workload workloads[] = {
    { "alu",      w_alu,      4000000 },
    { "branch",   w_branch,   4000000 },
    { "mem_seq",  w_mem_seq,  40 },        // x 32768 elements
    { "mem_rand", w_mem_rand, 1000000, setup_mem_rand },
    { "call",     w_call,     2000000 },
    { "indirect", w_indirect, 2000000 },
    { "float",    w_float,    2000000 },
    { "neon",     w_neon,     200 },       // x 1024 vectors
    { "string",   w_string,   400 },       // x 64 KiB
    { "strsearch", w_strsearch, 200000 },   // strlen+strchr+memchr+memcmp, 16..256 B
    { "atomic",   w_atomic,   1000000 },
    { "mutex",    w_mutex,    1000000 },
    { "syscall",  w_syscall,  200000 },
    { "pingpong", w_pingpong, 5000 },
};

int main(int argc, char **argv) {
    double scale = argc > 1 ? atof(argv[1]) : 1.0;
    const char *filter = argc > 2 ? argv[2] : NULL;
    double total = 0;
    for (size_t i = 0; i < sizeof workloads / sizeof *workloads; i++) {
        const struct workload *w = &workloads[i];
        if (filter && !strstr(filter, w->name)) continue;
        uint64_t n = (uint64_t)(w->iters * scale); if (!n) n = 1;
        if (w->setup) w->setup();
        double t0 = now_ms();
        uint64_t c = w->fn(n);
        double ms = now_ms() - t0;
        total += ms; sink = c;
        printf("BENCH %-9s iters=%-9llu ms=%9.2f ns_per_iter=%10.2f check=%016llx\n", w->name,
               (unsigned long long)n, ms, ms * 1e6 / n, (unsigned long long)c);
        fflush(stdout);
    }
    printf("BENCH_TOTAL ms=%.1f\n", total);
    return 0;
}
