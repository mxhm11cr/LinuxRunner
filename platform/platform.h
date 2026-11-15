#ifndef PLATFORM_H
#define PLATFORM_H
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <sys/stat.h>
#include "misc.h"

// CPU counters are USER_HZ-style jiffies. Uptime is seconds, matching the
// Linux sysinfo ABI; /proc formats it as decimal seconds at presentation time.
struct cpu_usage {
    uint64_t user_ticks;
    uint64_t system_ticks;
    uint64_t idle_ticks;
    uint64_t nice_ticks;
};
struct cpu_usage get_cpu_usage(void);

struct mem_usage {
    uint64_t total;
    uint64_t free;
    uint64_t active;
    uint64_t inactive;
};
struct mem_usage get_mem_usage(void);

struct uptime_info {
    uint64_t uptime_ticks;
    uint64_t load_1m, load_5m, load_15m;
};
struct uptime_info get_uptime(void);

struct platform_sysinfo {
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t procs;
    uint32_t mem_unit;
};
struct platform_sysinfo platform_get_sysinfo(void);

struct platform_thread_cpu_usage {
    uint64_t user_sec;
    uint64_t user_usec;
    uint64_t system_sec;
    uint64_t system_usec;
};
struct platform_thread_cpu_usage platform_get_thread_cpu_usage(void);
// CPU time of another live host thread (guest process accounting).
bool platform_get_thread_cpu_usage_of(pthread_t thread, struct platform_thread_cpu_usage *usage);

// Guest-visible CPU topology. Deterministic rather than mirroring the host:
// exposing every host core makes modern runtimes (Bun/JSC/V8) fan out too
// aggressively. The app passes the user's choice (Settings › CPU & Memory) as
// LINUXRUNNER_CPU_CORES before the runtime starts; default 4. Guest threads run
// on separate host threads, so programs sized by nproc really use the cores.
#define PLATFORM_GUEST_CPU_MAX 16
#define PLATFORM_GUEST_CPU_DEFAULT 4
static inline unsigned platform_guest_cpu_count(void) {
    static unsigned count;
    if (!count) {
        const char *value = getenv("LINUXRUNNER_CPU_CORES");
        unsigned long chosen = value ? strtoul(value, NULL, 10) : 0;
        count = chosen >= 1 && chosen <= PLATFORM_GUEST_CPU_MAX ? (unsigned) chosen : PLATFORM_GUEST_CPU_DEFAULT;
    }
    return count;
}
// Memory reported to Linux programs (MemTotal, sysinfo). Without a choice the
// existing policy applies (half of what iOS allows the app, at most 4 GiB).
// LINUXRUNNER_MEMORY_MB (512 MiB - 8 GiB) reports the chosen amount instead.
// Returns 0 when no choice was made.
#define PLATFORM_GUEST_MEMORY_DEFAULT_CAP (4ULL << 30)
static inline uint64_t platform_guest_memory_choice(void) {
    static uint64_t bytes = 1;
    if (bytes == 1) {
        const char *value = getenv("LINUXRUNNER_MEMORY_MB");
        unsigned long long mb = value ? strtoull(value, NULL, 10) : 0;
        bytes = mb >= 512 && mb <= 8192 ? (uint64_t) mb << 20 : 0;
    }
    return bytes;
}

// Host OS shims. Keep Linux/macOS/iOS API differences behind platform/* so
// emulator/kernel code can include one stable interface.
int platform_fd_get_path(int fd, char *out, size_t out_size);
uint64_t platform_stat_atime_sec(const struct stat *st);
uint64_t platform_stat_mtime_sec(const struct stat *st);
uint64_t platform_stat_ctime_sec(const struct stat *st);
long platform_stat_atime_nsec(const struct stat *st);
long platform_stat_mtime_nsec(const struct stat *st);
long platform_stat_ctime_nsec(const struct stat *st);
int platform_get_random_bytes(char *buf, size_t len);
int platform_create_shared_memory_fd(size_t size);
void platform_set_thread_name(const char *name);
void platform_release_thread_memory_pressure(void);

#endif
