// Returns the ret_cache cannot predict, which the RET gadget now resolves
// through the per-thread block cache: recursion deeper than the cache, returns
// after longjmp, C++ exception unwinding and qsort callbacks. The checksums must
// equal a native run. Build: g++ -O1 ret_cache_miss_contract.cpp
// Prints RET_CACHE_MISS_CONTRACT sum=<hex> (compare with native).
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

static unsigned long mix(unsigned long a, unsigned long b) { return (a ^ b) * 0x9e3779b97f4a7c15ul + (a >> 7); }
__attribute__((noinline)) static unsigned long deep(int n, unsigned long acc) {
    if (n == 0) return acc;
    unsigned long r = deep(n - 1, mix(acc, n));   // not a tail call: one RET per level
    return mix(r, n * 3);
}
static jmp_buf env;
__attribute__((noinline)) static void jump_from(int depth) { if (depth == 0) longjmp(env, 7); jump_from(depth - 1); }
__attribute__((noinline)) static int thrower(int n) { if (n % 3 == 0) throw std::runtime_error("x"); return n * 2; }
static int cmp(const void *a, const void *b) { unsigned x = *(const unsigned *) a, y = *(const unsigned *) b; return (x > y) - (x < y); }

int main() {
    unsigned long sum = 0;
    for (int round = 0; round < 20; round++) {
        sum = mix(sum, deep(20000 + round, round));             // 20k nested returns
        int v = setjmp(env);
        if (v == 0) jump_from(500 + round); else sum = mix(sum, v + round);
        for (int i = 0; i < 300; i++) { try { sum = mix(sum, thrower(i)); } catch (const std::exception &) { sum = mix(sum, 0xdead); } }
        unsigned a[2000]; unsigned s = 12345u + round;
        for (unsigned &x : a) { s = s * 1103515245u + 12345u; x = s >> 8; }
        qsort(a, 2000, sizeof a[0], cmp);
        for (unsigned x : a) sum = mix(sum, x);
    }
    std::printf("RET_CACHE_MISS_CONTRACT sum=%016lx\n", sum);
    return 0;
}
