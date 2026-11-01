// A memory access that faults inside hot translated code must deliver exactly
// the guest registers of that instruction to the SIGSEGV handler, including
// registers written by earlier instructions of the same block after a previous
// access (the JIT keeps hot registers in host registers and stores them at
// access checkpoints). A loop runs long enough to be translated; its last
// iteration loads from a PROT_NONE page. The handler compares the ucontext.
// Build: gcc -O1 jit_fault_registers_contract.c   Prints JIT_FAULT_REGISTERS_CONTRACT PASS.
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

static sigjmp_buf back;
static uint64_t seen[31], seen_pc, fault_addr;
static void on_segv(int sig, siginfo_t *info, void *context) {
    (void) sig;
    ucontext_t *uc = context;
    memcpy(seen, uc->uc_mcontext.regs, sizeof seen);
    seen_pc = uc->uc_mcontext.pc;
    fault_addr = (uint64_t) info->si_addr;
    siglongjmp(back, 1);
}

// x0..x5 and x19..x24 get values derived from `i` between two loads; the
// second load is the one that faults on the last round.
static uint64_t round_trip(uint64_t i, const uint64_t *ok, const uint64_t *probe) {
    uint64_t out;
    __asm__ volatile(
        "ldr x9, [%[ok]]\n"            // first access: checkpoint
        "add x0, %[i], #1\n"  "add x1, %[i], #2\n"  "add x2, %[i], #3\n"
        "add x3, %[i], #4\n"  "add x4, %[i], #5\n"  "add x5, %[i], #6\n"
        "add x19, %[i], #19\n" "add x20, %[i], #20\n" "add x21, %[i], #21\n"
        "add x22, %[i], #22\n" "add x23, %[i], #23\n" "add x24, %[i], #24\n"
        "ldr x10, [%[probe]]\n"        // second access: faults on the last round
        "add %[out], x9, x10\n"
        : [out] "=&r" (out)
        : [i] "r" (i), [ok] "r" (ok), [probe] "r" (probe)
        : "x0", "x1", "x2", "x3", "x4", "x5", "x9", "x10", "x19", "x20", "x21", "x22", "x23", "x24", "memory");
    return out;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    long page = sysconf(_SC_PAGESIZE);
    uint64_t *guard = mmap(NULL, page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    static uint64_t ok = 7, also = 9;
    struct sigaction sa = {0};
    sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    int fails = 0;
    for (int trial = 0; trial < 5; trial++) {
        const uint64_t last = 200000 + trial;
        uint64_t sum = 0;
        if (sigsetjmp(back, 1) == 0) {
            for (uint64_t i = 0; i < last; i++) sum += round_trip(i, &ok, &also);   // becomes hot
            round_trip(last, &ok, guard);
            printf("JIT_FAULT_REGISTERS_CONTRACT FAIL no fault\n");
            return 1;
        }
        int bad = 0;
        for (int r = 0; r <= 5; r++) if (seen[r] != last + 1 + (uint64_t) r) bad++;
        for (int r = 19; r <= 24; r++) if (seen[r] != last + (uint64_t) r) bad++;
        if (fault_addr != (uint64_t) guard) bad++;
        if (bad) {
            printf("JIT_FAULT_REGISTERS_CONTRACT FAIL trial=%d wrong=%d x0=%#llx (want %#llx) x19=%#llx (want %#llx) addr=%#llx\n", trial, bad,
                   (unsigned long long) seen[0], (unsigned long long) (last + 1), (unsigned long long) seen[19], (unsigned long long) (last + 19),
                   (unsigned long long) fault_addr);
            fails++;
        }
        (void) sum;
    }
    if (!fails) puts("JIT_FAULT_REGISTERS_CONTRACT PASS");
    return fails != 0;
}
