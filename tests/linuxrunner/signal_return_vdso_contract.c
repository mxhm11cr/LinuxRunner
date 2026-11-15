// Signal handler return through the vDSO (__kernel_rt_sigreturn) — GitHub issue #3.
// AArch64 libcs (glibc, musl) install handlers without SA_RESTORER, so returning
// from any handler executes the kernel-provided trampoline in the vDSO. If the
// vDSO page is not readable/executable to the guest, every handler return faults.
// Covers: raise/return, register preservation, repeated and nested delivery,
// SIGCHLD during waitpid (EINTR + restart), interrupted read on a pipe, the
// vDSO mapping permissions in /proc/self/maps, and a write to the vDSO faulting.
#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL " __VA_ARGS__); printf("\n"); } } while (0)

static volatile sig_atomic_t usr1, usr2, nested_inner, chld;
static void on_usr1(int s) { (void)s; usr1++; }
static void on_usr2(int s) { (void)s; usr2++; if (usr2 == 1) { raise(SIGUSR1); nested_inner = usr1; } }
static void on_chld(int s) { (void)s; chld++; }
static volatile sig_atomic_t alarms;
static void on_alrm(int s) { (void)s; alarms++; }
static sigjmp_buf segv_jump;
static void on_segv(int s) { (void)s; siglongjmp(segv_jump, 1); }

static void install(int sig, void (*h)(int), int flags) {
    struct sigaction sa = {0}; sa.sa_handler = h; sa.sa_flags = flags; sigemptyset(&sa.sa_mask);
    if (sigaction(sig, &sa, NULL)) { perror("sigaction"); exit(2); }
}

int main(void) {
    // LinuxRunner does not advertise its vDSO via AT_SYSINFO_EHDR (it holds only the
    // signal trampoline, not a loader-parsable vDSO); find it in /proc/self/maps.
    unsigned long vdso = 0; char perms[8] = "";
    FILE *f = fopen("/proc/self/maps", "r"); char line[512];
    while (f && fgets(line, sizeof line, f)) if (strstr(line, "[vdso]")) sscanf(line, "%lx-%*x %7s", &vdso, perms);
    if (f) fclose(f);
    CHECK(vdso != 0, "[vdso] mapped");

    // 1. raise + return; callee-saved state survives the trampoline.
    install(SIGUSR1, on_usr1, 0);
    volatile long a = 0x1111, b = 0x2222;
    for (int i = 0; i < 1000; i++) raise(SIGUSR1);
    CHECK(usr1 == 1000 && a == 0x1111 && b == 0x2222, "1000 handler returns (got %d)", (int)usr1);

    // 2. nested: SIGUSR2 handler raises SIGUSR1 (not blocked) and returns twice.
    install(SIGUSR2, on_usr2, 0);
    int before = usr1; raise(SIGUSR2);
    CHECK(usr2 == 1 && nested_inner == before + 1, "nested delivery and return");

    // 3. SIGCHLD interrupting waitpid (no SA_RESTART): EINTR, then the child is reaped.
    install(SIGCHLD, on_chld, 0);
    pid_t child = fork();
    if (child == 0) { usleep(100000); _exit(7); }
    int status = 0; pid_t r;
    install(SIGALRM, on_alrm, 0);
    while ((r = waitpid(child, &status, 0)) < 0 && errno == EINTR) {}
    CHECK(r == child && WIFEXITED(status) && WEXITSTATUS(status) == 7 && chld >= 1, "SIGCHLD + waitpid (r=%d chld=%d)", (int)r, (int)chld);

    // 4. interrupted read: SIGALRM during a blocking pipe read returns EINTR.
    int p[2]; CHECK(pipe(p) == 0, "pipe");
    alarm(1); char c; errno = 0; ssize_t n = read(p[0], &c, 1);
    CHECK(n < 0 && errno == EINTR, "read interrupted by SIGALRM (n=%zd errno=%d)", n, errno);

    // 5. SA_RESTART: the same read restarts and completes when data arrives.
    // The writer's exit also generates SIGCHLD. Its intentionally non-restart
    // handler from test 3 may legally interrupt this read before it consumes
    // the byte. Isolate the alarm restart test from that competing signal.
    sigset_t blocked, previous_mask;
    sigemptyset(&blocked); sigaddset(&blocked, SIGCHLD);
    CHECK(sigprocmask(SIG_BLOCK, &blocked, &previous_mask) == 0, "block competing SIGCHLD");
    install(SIGALRM, on_alrm, SA_RESTART);
    if (fork() == 0) { usleep(1500000); if (write(p[1], "x", 1) != 1) _exit(1); _exit(0); }
    int alarms_before = alarms;
    alarm(1); n = read(p[0], &c, 1);
    CHECK(n == 1 && c == 'x', "SA_RESTART read restarted (n=%zd)", n);
    CHECK(alarms == alarms_before + 1, "SA_RESTART alarm handler ran");
    CHECK(sigprocmask(SIG_SETMASK, &previous_mask, NULL) == 0, "restore signal mask");
    while (wait(NULL) > 0) {}

    // 6. vDSO mapping: readable and executable, never writable.
    CHECK(perms[0] == 'r' && perms[1] == '-' && perms[2] == 'x', "[vdso] mapped r-x (got '%s')", perms);
    install(SIGSEGV, on_segv, SA_NODEFER);
    volatile int wrote = 0;
    volatile char first = 0;
    if (vdso && !sigsetjmp(segv_jump, 1)) { first = *(volatile char *)vdso; *(volatile char *)vdso = first; wrote = 1; }
    CHECK(!vdso || first == 0x7f, "vDSO readable (ELF magic)");
    CHECK(!wrote, "write to the vDSO faults");

    printf("SIGNAL_RETURN_VDSO_CONTRACT %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
