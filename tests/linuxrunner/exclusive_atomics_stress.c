// Exclusive-monitor atomics under contention: 2, 4 and 8 threads perform
// exact-count work through LDXR/STXR (built for ARMv8.0, so no LSE), on
// 8/16/32/64-bit variables, a CAS loop and a pthread mutex. Any lost or
// duplicated update changes a total. Also checks that a failed CAS leaves
// memory untouched and reports the observed value.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define ITERS 20000
static uint8_t c8;
static uint16_t c16;
static uint32_t c32;
static uint64_t c64, cas_total, mutex_total;
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;

static void *worker(void *arg) {
    (void) arg;
    for (int i = 0; i < ITERS; i++) {
        __atomic_fetch_add(&c8, 1, __ATOMIC_SEQ_CST);
        __atomic_fetch_add(&c16, 1, __ATOMIC_ACQ_REL);
        __atomic_fetch_add(&c32, 3, __ATOMIC_RELAXED);
        __atomic_fetch_add(&c64, 5, __ATOMIC_SEQ_CST);
        uint64_t old = __atomic_load_n(&cas_total, __ATOMIC_RELAXED);
        while (!__atomic_compare_exchange_n(&cas_total, &old, old + 7, 1, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            ;
        pthread_mutex_lock(&m);
        mutex_total += 11;
        pthread_mutex_unlock(&m);
    }
    return NULL;
}

int main(void) {
    int failures = 0;
    uint64_t probe = 42, expected = 41;
    int ok = __atomic_compare_exchange_n(&probe, &expected, 99, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    printf("ATOMIC_CASE cas-failure %s (ok=%d mem=%llu observed=%llu)\n",
           !ok && probe == 42 && expected == 42 ? "PASS" : "FAIL", ok,
           (unsigned long long) probe, (unsigned long long) expected);
    failures += !(!ok && probe == 42 && expected == 42);
    for (int threads = 2; threads <= 8; threads *= 2) {
        c8 = 0; c16 = 0; c32 = 0; c64 = 0; cas_total = 0; mutex_total = 0;
        pthread_t t[8];
        for (int i = 0; i < threads; i++) pthread_create(&t[i], NULL, worker, NULL);
        for (int i = 0; i < threads; i++) pthread_join(t[i], NULL);
        uint64_t n = (uint64_t) threads * ITERS;
        int pass = c8 == (uint8_t) n && c16 == (uint16_t) n && c32 == (uint32_t) (3 * n) &&
                   c64 == 5 * n && cas_total == 7 * n && mutex_total == 11 * n;
        printf("ATOMIC_CASE threads=%d %s c8=%u c16=%u c32=%u c64=%llu cas=%llu mutex=%llu\n", threads,
               pass ? "PASS" : "FAIL", c8, c16, c32, (unsigned long long) c64,
               (unsigned long long) cas_total, (unsigned long long) mutex_total);
        failures += !pass;
    }
    printf("ATOMIC_RESULT %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
