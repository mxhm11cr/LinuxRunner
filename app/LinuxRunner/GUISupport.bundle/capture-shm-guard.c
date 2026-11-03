/* LinuxRunner capture launcher, same license as LinuxRunner.
 * Freestanding ARM64 Linux ELF: no guest libc/compiler installation needed.
 * Preferred source and reproducible recipe: scripts/build-capture-shm-guard.sh.
 */
#include "kernel/capture_shm.h"
#ifndef CAPTURE_SHM_REQUEST_VERSION
#define CAPTURE_SHM_REQUEST_VERSION LR_CAPTURE_SHM_VERSION
#endif

static long call5(long number, long a, long b, long c, long d, long e) {
    register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e, x8 __asm__("x8") = number;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory", "cc");
    return x0;
}
static int equal(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static void message(const char *text, unsigned long size) {
    (void)call5(64, 2, (long)text, size, 0, 0); // write(stderr)
}
__attribute__((used)) long capture_start(unsigned long *stack) {
    unsigned long argc = stack[0];
    char **argv = (char **)(stack + 1);
    if (argc < 2 || argc > 255) return 64;
    long enabled = call5(167, LR_PR_CAPTURE_SHM, CAPTURE_SHM_REQUEST_VERSION, 0, 0, 0);
    if (argc == 2 && equal(argv[1], "--probe")) return enabled == 0 ? 0 : 77;
    if (argv[1][0] != '/') return 64; // session.sh resolves x11vnc first
    char **env = argv + argc + 1;
    if (enabled == 0) {
        static const char text[] = "GUI_CAPTURE: cleanup=process-scope\n";
        message(text, sizeof(text) - 1);
        (void)call5(221, (long)argv[1], (long)(argv + 1), (long)env, 0, 0);
    } else {
        // Even if capability changed after --probe, never run unguarded shared
        // capture. Unsupported runtimes retain the established socket path.
        char *rollback[257]; rollback[0] = argv[1]; rollback[1] = "-noshm";
        for (unsigned long i = 2; i < argc; i++) rollback[i] = argv[i];
        rollback[argc] = (char *)0;
        static const char text[] = "GUI_CAPTURE: shared-images=off cleanup=unavailable socket-fallback\n";
        message(text, sizeof(text) - 1);
        (void)call5(221, (long)argv[1], (long)rollback, (long)env, 0, 0);
    }
    return 126;
}
__asm__(".text\n.align 2\n.global _start\n.type _start,%function\n_start:\n"
        "mov x0,sp\nbl capture_start\nmov x8,#93\nsvc #0\nb .\n");
