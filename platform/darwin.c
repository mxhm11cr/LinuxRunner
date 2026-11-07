#include <mach/mach.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
#include <os/proc.h>
#include "platform/guest_memory.h"
#endif
#include <dispatch/dispatch.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/fcntl.h>
#include <pthread.h>
#include <malloc/malloc.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <CommonCrypto/CommonCrypto.h>
#include <CommonCrypto/CommonRandom.h>
#include "debug.h"
#include "platform/platform.h"

// mach_host_self() increments the send-right refcount on each call. Caching the
// right avoids leaking refs when guests poll /proc/stat or /proc/meminfo.
static mach_port_t cached_host_self(void) {
    static mach_port_t host = MACH_PORT_NULL;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        host = mach_host_self();
    });
    return host;
}

struct cpu_usage get_cpu_usage() {
    host_cpu_load_info_data_t load = {};
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(cached_host_self(), HOST_CPU_LOAD_INFO, (host_info_t) &load, &count) != KERN_SUCCESS)
        return (struct cpu_usage) {};
    return (struct cpu_usage) {
        .user_ticks = load.cpu_ticks[CPU_STATE_USER],
        .system_ticks = load.cpu_ticks[CPU_STATE_SYSTEM],
        .idle_ticks = load.cpu_ticks[CPU_STATE_IDLE],
        .nice_ticks = load.cpu_ticks[CPU_STATE_NICE],
    };
}

static struct mem_usage guest_mem_usage(struct mem_usage usage) {
#if TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
    task_vm_info_data_t info = {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    uint64_t remaining = os_proc_available_memory();
    uint64_t process_limit = 0;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
        process_limit = info.phys_footprint + remaining;
    usage.total = guest_memory_total(usage.total, process_limit);
    if (usage.free > remaining / 2) usage.free = remaining / 2;
    if (usage.free > usage.total) usage.free = usage.total;
#endif
    return usage;
}

struct mem_usage get_mem_usage() {
    static struct mem_usage last_good;
    host_basic_info_data_t basic = {};
    vm_statistics64_data_t vm = {};
    mach_msg_type_number_t count = HOST_BASIC_INFO_COUNT;
    kern_return_t status = host_info(cached_host_self(), HOST_BASIC_INFO, (host_info_t) &basic, &count);
    if (status == KERN_SUCCESS) {
        count = HOST_VM_INFO64_COUNT;
        status = host_statistics64(cached_host_self(), HOST_VM_INFO64, (host_info_t) &vm, &count);
    }
    if (status != KERN_SUCCESS) {
        printk("WARNING: get_mem_usage: Mach host statistics failed (%d)\n", status);
        if (last_good.total != 0)
            return guest_mem_usage(last_good);
        uint64_t total = 0;
        size_t size = sizeof(total);
        if (sysctlbyname("hw.memsize", &total, &size, NULL, 0) != 0 || total == 0)
            total = 4ULL * 1024 * 1024 * 1024;
        return guest_mem_usage((struct mem_usage) {
            .total = total,
            .free = total / 4,
            .active = total / 4,
            .inactive = total / 4,
        });
    }

    last_good = (struct mem_usage) {
        .total = basic.max_mem,
        .free = vm.free_count * vm_page_size,
        .active = vm.active_count * vm_page_size,
        .inactive = vm.inactive_count * vm_page_size,
    };
    return guest_mem_usage(last_good);
}

struct uptime_info get_uptime() {
    struct timeval kern_boottime = {0};
    size_t size = sizeof(kern_boottime);
    bool have_boot = sysctlbyname("kern.boottime", &kern_boottime, &size, NULL, 0) == 0;
    struct timeval now;
    gettimeofday(&now, NULL);

    struct {
        uint32_t ldavg[3];
        long scale;
    } vm_loadavg = {0};
    size = sizeof(vm_loadavg);
    sysctlbyname("vm.loadavg", &vm_loadavg, &size, NULL, 0);

    // linux wants the scale to be 16 bits
    for (int i = 0; i < 3; i++) {
        if (FSHIFT < 16)
            vm_loadavg.ldavg[i] <<= 16 - FSHIFT;
        else
            vm_loadavg.ldavg[i] >>= FSHIFT - 16;
    }

    struct timespec monotonic = {0};
    if (!have_boot) clock_gettime(CLOCK_MONOTONIC, &monotonic);
    struct uptime_info uptime = {
        .uptime_ticks = have_boot && now.tv_sec >= kern_boottime.tv_sec ? now.tv_sec - kern_boottime.tv_sec : monotonic.tv_sec,
        .load_1m = vm_loadavg.ldavg[0],
        .load_5m = vm_loadavg.ldavg[1],
        .load_15m = vm_loadavg.ldavg[2],
    };
    return uptime;
}

struct platform_sysinfo platform_get_sysinfo(void) {
    struct mem_usage mem = get_mem_usage();
    return (struct platform_sysinfo) {
        .totalram = mem.total,
        .freeram = mem.free,
        .mem_unit = 1,
    };
}

struct platform_thread_cpu_usage platform_get_thread_cpu_usage(void) {
    thread_basic_info_data_t info = {};
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    // mach_thread_self returns a send right owned by this caller (84770265).
    mach_port_t thread = mach_thread_self();
    kern_return_t status = thread_info(thread, THREAD_BASIC_INFO, (thread_info_t)&info, &count);
    mach_port_deallocate(mach_task_self(), thread);
    assert(status == KERN_SUCCESS);
    return (struct platform_thread_cpu_usage) {
        .user_sec = info.user_time.seconds,
        .user_usec = info.user_time.microseconds,
        .system_sec = info.system_time.seconds,
        .system_usec = info.system_time.microseconds,
    };
}

bool platform_get_thread_cpu_usage_of(pthread_t thread, struct platform_thread_cpu_usage *usage) {
    // No extra send right: pthread_mach_thread_np returns the thread's own port.
    mach_port_t port = pthread_mach_thread_np(thread);
    if (port == MACH_PORT_NULL)
        return false;
    thread_basic_info_data_t info = {};
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (thread_info(port, THREAD_BASIC_INFO, (thread_info_t)&info, &count) != KERN_SUCCESS)
        return false;
    *usage = (struct platform_thread_cpu_usage) {
        .user_sec = info.user_time.seconds, .user_usec = info.user_time.microseconds,
        .system_sec = info.system_time.seconds, .system_usec = info.system_time.microseconds,
    };
    return true;
}

int platform_fd_get_path(int fd, char *out, size_t out_size) {
    if (out_size == 0)
        return -1;
    char tmp[MAXPATHLEN];
    if (fcntl(fd, F_GETPATH, tmp) < 0)
        return -1;
    strlcpy(out, tmp, out_size);
    return strlen(tmp) < out_size ? 0 : -1;
}

uint64_t platform_stat_atime_sec(const struct stat *st) { return st->st_atimespec.tv_sec; }
uint64_t platform_stat_mtime_sec(const struct stat *st) { return st->st_mtimespec.tv_sec; }
uint64_t platform_stat_ctime_sec(const struct stat *st) { return st->st_ctimespec.tv_sec; }
long platform_stat_atime_nsec(const struct stat *st) { return st->st_atimespec.tv_nsec; }
long platform_stat_mtime_nsec(const struct stat *st) { return st->st_mtimespec.tv_nsec; }
long platform_stat_ctime_nsec(const struct stat *st) { return st->st_ctimespec.tv_nsec; }

int platform_get_random_bytes(char *buf, size_t len) {
    return CCRandomGenerateBytes(buf, len) == kCCSuccess ? 0 : -1;
}

int platform_create_shared_memory_fd(size_t size) {
    // iOS's global /tmp is outside the application sandbox. TMPDIR is the
    // host process's sandbox temporary directory, not the guest's /tmp.
    const char *directory = getenv("TMPDIR");
    if (!directory || !*directory) directory = "/tmp"; // CLI Darwin fallback
    char path[4096];
    if (directory[0] != '/' || snprintf(path, sizeof(path), "%s/ish-shm-XXXXXX", directory) >= sizeof(path)) {
        errno = EINVAL;
        return -1;
    }
    int fd = mkstemp(path);
    if (fd < 0) {
        int saved = errno;
        printk("SHARED_BACKING: create failed errno=%d sandbox_tmp=%d size=%zu\n", saved, strcmp(directory,"/tmp") != 0, size);
        errno = saved;
        return -1;
    }
    if (unlink(path) < 0) { int saved = errno; close(fd); errno = saved; return -1; }
    if (ftruncate(fd, (off_t) size) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

void platform_set_thread_name(const char *name) {
    pthread_setname_np(name);
}

void platform_release_thread_memory_pressure(void) {
    // Ask Darwin malloc zones to drop thread-local caches after guest thread
    // groups exit; this is a host memory-pressure hint, not guest semantics.
    malloc_zone_pressure_relief(NULL, 0);
}
