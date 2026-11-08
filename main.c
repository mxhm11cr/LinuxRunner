#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <stdbool.h>
#include <execinfo.h>
#include <termios.h>
#include <unistd.h>
#include <pthread.h>
#include <stddef.h>
#include <stdarg.h>
#include "kernel/calls.h"
#include "kernel/task.h"
#include "kernel/init.h"
#include "graphics/guest_gpu.h"
#include "fs/path.h"
#include "fs/devices.h"
#include "emu/cpu.h"
#include "emu/tlb.h"
#include "asbestos/frame.h"
#ifdef ISH_JIT
#include "asbestos/guest-arm64/jit.h"
#endif
#include "asbestos/asbestos.h"
#include "platform/host_context_aarch64.h"
#include "platform/native_fault.h"
#include "xX_main_Xx.h"
#include "fs/sock.h"

// Thread-local JIT recovery state (defined in asbestos.c)
extern __thread volatile sig_atomic_t in_jit;
extern __thread volatile uint64_t jit_saved_pc;

// Diagnostic: JIT crash info (defined in calls.c)
extern __thread volatile uint64_t jit_last_host_fault;
extern __thread volatile uint64_t jit_last_x7;
extern __thread volatile uint64_t jit_last_x10;
extern __thread volatile int jit_crash_count;
extern volatile bool g_trace_highbits;
extern volatile bool g_trace_faults;
extern volatile bool g_trace_syscalls;

int fakefs_bind_mount(const char *linux_path, const char *host_path, bool read_only);
int fakefs_bind_mount_set_owner(const char *linux_path, uint32_t uid, uint32_t gid);

// Assembly trampoline: returns INT_JIT_CRASH via fiber_exit (defined in entry.S)
extern void jit_crash_trampoline(void);

// CLI adapter retains its existing gadget replay and signal-mask policy.
// The guest's fd 2 wraps host stderr and may close it; printk's dup (666)
// survives, so crash reports go there when it exists.
static int crash_fd(void) { return fcntl(666, F_GETFD) != -1 ? 666 : STDERR_FILENO; }
static void crash_handler(int sig, siginfo_t *info, void *ctx) {
    enum ish_fault_result recovered=ish_native_fault_recover(sig,info,ctx,1);
    if(recovered==ISH_FAULT_FATAL) {
        static const char message[]="unrecoverable native JIT/AOT fault\n";
        (void)write(crash_fd(),message,sizeof(message)-1);
        _exit(139);
    }
    if(recovered==ISH_FAULT_REDIRECTED) {
        sigset_t unblock; sigemptyset(&unblock); sigaddset(&unblock,sig);
        sigprocmask(SIG_UNBLOCK,&unblock,NULL);
        return;
    }

    // Non-JIT crash: dump state and exit
    char buf[512];
    int len;
    ucontext_t *uc = (ucontext_t *)ctx;
    len = snprintf(buf, sizeof(buf), "\n=== HOST CRASH: signal %d ===\nfault addr: %p\n", sig, info->si_addr);
    write(crash_fd(), buf, len);
#ifdef __aarch64__
    len = snprintf(buf, sizeof(buf),
        "pc:  0x%llx\nlr:  0x%llx\nsp:  0x%llx\n"
        "x0:  0x%llx\nx1:  0x%llx\nx2:  0x%llx\n"
        "x7:  0x%llx\nx28: 0x%llx\n",
        (unsigned long long) host_ctx_aarch64_pc(uc),
        (unsigned long long) host_ctx_aarch64_lr(uc),
        (unsigned long long) host_ctx_aarch64_sp(uc),
        (unsigned long long) host_ctx_aarch64_reg(uc, 0),
        (unsigned long long) host_ctx_aarch64_reg(uc, 1),
        (unsigned long long) host_ctx_aarch64_reg(uc, 2),
        (unsigned long long) host_ctx_aarch64_reg(uc, 7),
        (unsigned long long) host_ctx_aarch64_reg(uc, 28));
    write(crash_fd(), buf, len);
#endif
    void *bt[20];
    int n = backtrace(bt, 20);
    backtrace_symbols_fd(bt, n, crash_fd());
    _exit(139);
}

static struct termios saved_termios;
static int saved_termios_valid;

void restore_termios(void) {
    if (saved_termios_valid)
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
}

static char *trim_ascii_space(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r'))
        *--end = '\0';
    return s;
}

static void append_boot_env(char *envp, size_t cap, size_t *offset, const char *fmt, ...) {
    if (*offset >= cap)
        return;

    va_list ap;
    va_start(ap, fmt);
    int needed = vsnprintf(envp + *offset, cap - *offset, fmt, ap);
    va_end(ap);

    if (needed < 0)
        return;

    size_t n = (size_t) needed;
    if (n >= cap - *offset) {
        // Truncated entry; stop appending to avoid pointer/size underflow.
        *offset = cap;
        envp[cap - 1] = '\0';
        return;
    }

    // Entries are NUL-separated for xX_main_Xx.
    *offset += n + 1;
}

static void apply_env_bind_mounts(void) {
    const char *spec = getenv("ISH_BIND_MOUNTS");
    if (spec == NULL || spec[0] == '\0')
        return;

    char *copy = strdup(spec);
    if (copy == NULL) {
        fprintf(stderr, "ISH_BIND_MOUNTS: out of memory\n");
        return;
    }

    char *saveptr = NULL;
    for (char *entry = strtok_r(copy, ",", &saveptr); entry != NULL; entry = strtok_r(NULL, ",", &saveptr)) {
        entry = trim_ascii_space(entry);
        if (entry[0] == '\0')
            continue;

        char *eq = strchr(entry, '=');
        if (eq == NULL) {
            fprintf(stderr, "ISH_BIND_MOUNTS: skipping entry without '=': %s\n", entry);
            continue;
        }
        *eq = '\0';
        char *linux_path = trim_ascii_space(entry);
        char *host_path = trim_ascii_space(eq + 1);
        bool read_only = false;

        size_t host_len = strlen(host_path);
        if (host_len > 3 && strcmp(host_path + host_len - 3, ":ro") == 0) {
            host_path[host_len - 3] = '\0';
            read_only = true;
        } else if (host_len > 3 && strcmp(host_path + host_len - 3, ":rw") == 0) {
            host_path[host_len - 3] = '\0';
        }
        host_path = trim_ascii_space(host_path);

        if (linux_path[0] != '/' || host_path[0] != '/') {
            fprintf(stderr, "ISH_BIND_MOUNTS: paths must be absolute: %s=%s\n", linux_path, host_path);
            continue;
        }

        int err = fakefs_bind_mount(linux_path, host_path, read_only);
        if (err < 0)
            fprintf(stderr, "ISH_BIND_MOUNTS: failed to bind %s=%s%s: %s\n",
                    linux_path, host_path, read_only ? ":ro" : ":rw", strerror(-err));
    }

    free(copy);

    /* ISH_BIND_MOUNT_OWNERS=/mnt/folder=1000:1000,... owner of host files that
     * have no guest metadata yet (the app does this for shared iPhone folders). */
    const char *owners = getenv("ISH_BIND_MOUNT_OWNERS");
    char *owners_copy = owners ? strdup(owners) : NULL;
    for (char *entry = owners_copy ? strtok_r(owners_copy, ",", &saveptr) : NULL; entry != NULL; entry = strtok_r(NULL, ",", &saveptr)) {
        unsigned uid, gid;
        char *eq = strrchr(entry, '=');
        if (eq == NULL || sscanf(eq + 1, "%u:%u", &uid, &gid) != 2) {
            fprintf(stderr, "ISH_BIND_MOUNT_OWNERS: skipping malformed entry: %s\n", entry);
            continue;
        }
        *eq = '\0';
        int err = fakefs_bind_mount_set_owner(trim_ascii_space(entry), uid, gid);
        if (err < 0)
            fprintf(stderr, "ISH_BIND_MOUNT_OWNERS: %s: %s\n", entry, strerror(-err));
    }
    free(owners_copy);
}

int main(int argc, char *const argv[]) {
    // Save host terminal settings so we can restore on exit
    if (isatty(STDIN_FILENO)) {
        if (tcgetattr(STDIN_FILENO, &saved_termios) == 0) {
            saved_termios_valid = 1;
            atexit(restore_termios);
        }
    }

    // Redirect printk output (fd 666) to stderr
    dup2(STDERR_FILENO, 666);

#ifdef GUEST_ARM64
    // Test hook: host directory+prefix for guest AF_UNIX backing sockets, so
    // device-length paths (iOS container temp dir) can be reproduced here.
    const char *sock_prefix = getenv("ISH_SOCK_TMP_PREFIX");
    if (sock_prefix && *sock_prefix) sock_tmp_prefix = sock_prefix;
    const char *trace_highbits = getenv("ISH_TRACE_HIGHBITS");
    g_trace_highbits = trace_highbits && trace_highbits[0] && strcmp(trace_highbits, "0") != 0;
    const char *trace_syscalls = getenv("ISH_SYSTRACE");
    g_trace_syscalls = trace_syscalls && !strcmp(trace_syscalls,"1");
    const char *trace_faults = getenv("ISH_TRACE_FAULTS");
    g_trace_faults = trace_faults && trace_faults[0] && strcmp(trace_faults, "0") != 0;
    extern void arm64_fusion_stats_set_enabled_from_env(const char *env);
    extern void arm64_block_stats_set_enabled_from_env(const char *env);
    extern void arm64_eager_prechain_set_enabled_from_env(const char *env);
    extern void arm64_eager_prechain_incoming_set_enabled_from_env(const char *env);
    extern void arm64_internal_continue_set_enabled_from_env(const char *env);
    extern void arm64_internal_continue_taken_set_enabled_from_env(const char *env);
    extern void arm64_memory_pc_fusion_set_enabled_from_env(const char *env);
    arm64_fusion_stats_set_enabled_from_env(getenv("ISH_ARM64_FUSION_STATS"));
    arm64_block_stats_set_enabled_from_env(getenv("ISH_ARM64_BLOCK_STATS"));
    arm64_eager_prechain_set_enabled_from_env(getenv("ISH_ARM64_EAGER_PRECHAIN"));
    arm64_eager_prechain_incoming_set_enabled_from_env(getenv("ISH_ARM64_EAGER_PRECHAIN_INCOMING"));
    arm64_internal_continue_set_enabled_from_env(getenv("ISH_ARM64_INTERNAL_CONTINUE"));
    arm64_internal_continue_taken_set_enabled_from_env(getenv("ISH_ARM64_INTERNAL_CONTINUE_TAKEN"));
    arm64_memory_pc_fusion_set_enabled_from_env(getenv("ISH_ARM64_MEMORY_PC_FUSION"));
    extern void arm64_indirect_branch_cache_set_enabled_from_env(const char *env);
    arm64_indirect_branch_cache_set_enabled_from_env(getenv("ISH_INDIRECT_BRANCH_CACHE"));
    extern void guest_profile_init(void);
    guest_profile_init();
    extern void arm64_lazy_fault_pc_set_enabled_from_env(const char *env);
    arm64_lazy_fault_pc_set_enabled_from_env(getenv("ISH_LAZY_FAULT_PC"));
    asbestos_set_trace_pcs(getenv("ISH_TRACE_PCS"));
    extern bool g_trace_memory_regs;
    g_trace_memory_regs = getenv("ISH_TRACE_MEMORY_REGS") && !strcmp(getenv("ISH_TRACE_MEMORY_REGS"), "1");
    asbestos_set_trace_gate(getenv("ISH_TRACE_GATE_PC"),
                            getenv("ISH_TRACE_GATE_X4"),
                            getenv("ISH_TRACE_GATE_BUDGET"));
#endif

    static char altstack[SIGSTKSZ];
    stack_t ss = {.ss_sp = altstack, .ss_size = SIGSTKSZ};
    // A sanitizer or embedding host may own an existing alternate stack and
    // unmap it during thread teardown. Never substitute our static storage.
    stack_t existing_stack;
    if (sigaltstack(NULL, &existing_stack) < 0 || (existing_stack.ss_flags & SS_DISABLE))
        sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
    char envp[512] = {0};
    size_t p = 0;
    append_boot_env(envp, sizeof(envp), &p, "HOME=/root");
    append_boot_env(envp, sizeof(envp), &p, "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
#ifdef GUEST_ARM64
    append_boot_env(envp, sizeof(envp), &p, "PYTHONMALLOC=malloc");
#endif
    const char *term = getenv("TERM");
    if (term && term[0] != '\0')
        append_boot_env(envp, sizeof(envp), &p, "TERM=%s", term);
    int err = xX_main_Xx(argc, argv, envp);
    if (err < 0) {
        fprintf(stderr, "xX_main_Xx: %s\n", strerror(-err));
        return err;
    }
    do_mount(&procfs, "proc", "/proc", "", 0);
    // sysfs describes only the software DRM device (fs/sys.c).
    const char *software_drm = getenv("ISH_SOFTWARE_DRM");
    if (software_drm && !strcmp(software_drm, "1"))
        do_mount(&sysfs, "sysfs", "/sys", "", 0);
    do_mount(&devptsfs, "devpts", "/dev/pts", "", 0);
    // Same guest Metal device node the app creates at boot (app/AppDelegate.m):
    // isolated per-open contexts, usable by ordinary desktop users.
    generic_mknodat(AT_PWD, "/dev/linuxrunner-gpu", S_IFCHR|0666, dev_make(LR_GPU_MAJOR, 0));
    generic_setattrat(AT_PWD, "/dev/linuxrunner-gpu", (struct attr) {.type = attr_mode, .mode = S_IFCHR|0666}, false);
    err = prepare_guest_runtime_dirs();
    fprintf(stderr, "GUEST_BOOT: /dev/shm mode=01777 host-page=%ld guest-page=%u result=%d\n", sysconf(_SC_PAGESIZE), (unsigned)PAGE_SIZE, err);
    if (err < 0) return 1;
    apply_env_bind_mounts();
    task_run_current();
}
