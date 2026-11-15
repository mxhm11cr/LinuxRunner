#include <sys/stat.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>
#include "kernel/calls.h"
#include "fs/proc.h"
#include "platform/platform.h"
#include <pthread.h>
#include <stdlib.h>

static int proc_show_version(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct uname uts;
    do_uname(&uts);
    proc_printf(buf, "%s version %s %s\n", uts.system, uts.release, uts.version);
    return 0;
}

static int proc_show_stat(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct cpu_usage usage = get_cpu_usage();
    // Modern procps requires at least eight counters before parsing btime.
    // The first four are measured host timeshare counters. Guest IRQ, steal
    // and kernel guest-time counters do not exist in this userspace runtime.
    proc_printf(buf, "cpu  %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" 0 0 0 0 0 0\n", usage.user_ticks, usage.nice_ticks, usage.system_ticks, usage.idle_ticks);
    unsigned cpus = platform_guest_cpu_count();
    for (unsigned cpu = 0; cpu < cpus; cpu++) {
        // Project the aggregate over the synthetic guest CPU topology; these
        // are not individual physical CPU counters. Sum equals the aggregate.
#define CPU_SHARE(v) ((v) / cpus + (cpu < (v) % cpus))
        proc_printf(buf, "cpu%u %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" 0 0 0 0 0 0\n", cpu,
            CPU_SHARE(usage.user_ticks), CPU_SHARE(usage.nice_ticks), CPU_SHARE(usage.system_ticks), CPU_SHARE(usage.idle_ticks));
#undef CPU_SHARE
    }

    // calculate btime (boot time in seconds since epoch) by subtracting uptime from current time
    struct uptime_info uptime = get_uptime();
    struct timespec uptime_ts = {.tv_sec = uptime.uptime_ticks, .tv_nsec = 0};
    struct timespec boot_time = timespec_subtract(timespec_now(CLOCK_REALTIME), uptime_ts);
    proc_printf(buf, "btime %ld\n", boot_time.tv_sec);

    return 0;
}

static int proc_show_cpuinfo(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    unsigned cpus = platform_guest_cpu_count();
    for (unsigned i = 0; i < cpus; i++) {
        proc_printf(buf, "processor\t: %u\n", i);
        proc_printf(buf, "BogoMIPS\t: 48.00\n");
        // Match ARM64 AT_HWCAP: advertise only interpreter capabilities.
        proc_printf(buf, "Features\t: fp asimd aes\n");
        proc_printf(buf, "CPU implementer\t: 0x00\n");
        proc_printf(buf, "CPU architecture: 8\n");
        proc_printf(buf, "CPU variant\t: 0x0\n");
        proc_printf(buf, "CPU part\t: 0x000\n");
        proc_printf(buf, "CPU revision\t: 0\n");
        proc_printf(buf, "\n");
    }
    return 0;
}

static void show_kb(struct proc_data *buf, const char *name, uint64_t value) {
    proc_printf(buf, "%s%8"PRIu64" kB\n", name, value / 1024);
}

static int proc_show_meminfo(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct mem_usage usage = get_mem_usage();
    // Cap reported memory to match sys_sysinfo limit.
    // Reporting full host RAM (e.g. 24GB) causes V8 to set heap_size_limit=4GB
    // which exhausts the emulator's limited address space.
    #define MEMINFO_MAX_RAM PLATFORM_GUEST_MEMORY_DEFAULT_CAP
    if (platform_guest_memory_choice()) {
        usage.total = platform_guest_memory_choice();   // Settings › CPU & Memory
        if (usage.free > usage.total) usage.free = usage.total;
    } else if (usage.total > MEMINFO_MAX_RAM)
        usage.total = MEMINFO_MAX_RAM;
#if ANON_MMAP_LIMIT_PAGES > 0
    extern _Atomic long anon_page_count;
    long used_pages = atomic_load(&anon_page_count);
    uint64_t used_bytes = (uint64_t)(used_pages > 0 ? used_pages : 0) * 4096;
    usage.free = used_bytes < usage.total ? usage.total - used_bytes : 0;
#else
    if (usage.free > MEMINFO_MAX_RAM)
        usage.free = MEMINFO_MAX_RAM;
#endif
    show_kb(buf, "MemTotal:       ", usage.total);
    show_kb(buf, "MemFree:        ", usage.free);
    show_kb(buf, "MemShared:      ", 0);
    // a bunch of fields busybox top expects to see.
    show_kb(buf, "Shmem:          ", 0);
    show_kb(buf, "Buffers:        ", 0);
    show_kb(buf, "Cached:         ", 0);
    show_kb(buf, "SwapTotal:      ", 0);
    show_kb(buf, "SwapFree:       ", 0);
    show_kb(buf, "Dirty:          ", 0);
    show_kb(buf, "Writeback:      ", 0);
    show_kb(buf, "AnonPages:      ", 0);
    show_kb(buf, "Mapped:         ", 0);
    show_kb(buf, "Slab:           ", 0);
    return 0;
}

static int proc_show_uptime(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct uptime_info uptime_info = get_uptime();
    unsigned long uptime = uptime_info.uptime_ticks;
    proc_printf(buf, "%lu.00 %lu.00\n", uptime, uptime);
    return 0;
}

static int proc_readlink_self(struct proc_entry *UNUSED(entry), char *buf) {
    sprintf(buf, "%d", current->tgid);
    return 0;
}

static int proc_show_loadavg(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct uptime_info load = get_uptime();
    unsigned total = 0, runnable = 0, last = 0;
    lock(&pids_lock);
    for (unsigned pid = 1; pid <= MAX_PID; pid++) {
        struct task *task = pid_get_task(pid);
        if (!task || task->exiting || !task->group) continue;
        total++;
        if (!task->blocking && !task->group->stopped) runnable++;
    }
    last = task_last_pid_locked();
    unlock(&pids_lock);
    // Load averages are host measurements; runnable counts refer to guests.
    proc_printf(buf, "%.2f %.2f %.2f %u/%u %u\n", load.load_1m/65536., load.load_5m/65536., load.load_15m/65536., runnable,total,last);
    return 0;
}

static void proc_print_escaped(struct proc_data *buf, const char *str) {
    for (size_t i = 0; str[i]; i++) {
        switch (str[i]) {
            case '\t': case ' ': case '\\':
                proc_printf(buf, "\\%03o", str[i]);
                break;
            default:
                proc_printf(buf, "%c", str[i]);
        }
    }
}

#define proc_printf_comma(buf, at_start, format, ...) do { \
    proc_printf((buf), "%s" format, *(at_start) ? "" : ",", ##__VA_ARGS__); \
    *(at_start) = false; \
} while (0)

int proc_show_mounts(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        const char *point = mount->point;
        if (point[0] == '\0')
            point = "/";

        proc_print_escaped(buf, mount->source);
        proc_printf(buf, " ");
        proc_print_escaped(buf, point);
        proc_printf(buf, " %s ", mount->fs->name);
        bool at_start = true;
        proc_printf_comma(buf, &at_start, "%s", mount->flags & MS_READONLY_ ? "ro" : "rw");
        if (mount->flags & MS_NOSUID_)
            proc_printf_comma(buf, &at_start, "nosuid");
        if (mount->flags & MS_NODEV_)
            proc_printf_comma(buf, &at_start, "nodev");
        if (mount->flags & MS_NOEXEC_)
            proc_printf_comma(buf, &at_start, "noexec");
        if (strcmp(mount->info, "") != 0)
            proc_printf_comma(buf, &at_start, "%s", mount->info);
        proc_printf(buf, " 0 0\n");
    };
    return 0;
}

// /proc/PID/mountinfo (and /proc/self/mountinfo): the same mount table as
// /proc/mounts in mountinfo(5) form. LinuxRunner has one mount namespace, the
// root mount is the parent of every other mount, and there are no device
// numbers, so each mount reports 0:<mount id>. IDs match statx STATX_MNT_ID.
int proc_show_mountinfo(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct mount *mount;
    uint64_t root_id = 0;
    list_for_each_entry(&mounts, mount, mounts)
        if (mount->point[0] == '\0') root_id = mount->id;
    list_for_each_entry(&mounts, mount, mounts) {
        const char *point = mount->point[0] == '\0' ? "/" : mount->point;
        proc_printf(buf, "%llu %llu 0:%llu / ", (unsigned long long) mount->id,
                    (unsigned long long) root_id, (unsigned long long) mount->id);
        proc_print_escaped(buf, point);
        proc_printf(buf, " %s", mount->flags & MS_READONLY_ ? "ro" : "rw");
        if (mount->flags & MS_NOSUID_) proc_printf(buf, ",nosuid");
        if (mount->flags & MS_NODEV_) proc_printf(buf, ",nodev");
        if (mount->flags & MS_NOEXEC_) proc_printf(buf, ",noexec");
        proc_printf(buf, " - %s ", mount->fs->name);
        proc_print_escaped(buf, mount->source);
        proc_printf(buf, " %s%s%s\n", mount->flags & MS_READONLY_ ? "ro" : "rw",
                    strcmp(mount->info, "") != 0 ? "," : "", mount->info);
    }
    return 0;
}

static int proc_show_filesystems(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    // The filesystem types LinuxRunner can mount (see fs/ and kernel/fs.c).
    proc_printf(buf, "nodev\tproc\nnodev\ttmpfs\nnodev\tdevpts\nnodev\tsysfs\n\tfakefs\n\trealfs\n");
    return 0;
}

static int proc_show_cmdline(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "linuxrunner\n");
    return 0;
}

static int proc_sys_osrelease(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct uname uts; do_uname(&uts); proc_printf(buf, "%s\n", uts.release); return 0;
}
static int proc_sys_ostype(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct uname uts; do_uname(&uts); proc_printf(buf, "%s\n", uts.system); return 0;
}
static int proc_sys_version(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct uname uts; do_uname(&uts); proc_printf(buf, "%s\n", uts.version); return 0;
}
static int proc_sys_hostname(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct uname uts; do_uname(&uts); proc_printf(buf, "%s\n", uts.hostname); return 0;
}
static void proc_print_uuid(struct proc_data *buf, const uint8_t b[16]) {
    proc_printf(buf, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}
static void proc_random_uuid(uint8_t b[16]) {
    arc4random_buf(b, 16);
    b[6] = (b[6] & 0x0f) | 0x40; b[8] = (b[8] & 0x3f) | 0x80;   // RFC 4122 version 4
}
static uint8_t proc_boot_id[16];
static void proc_boot_id_init(void) { proc_random_uuid(proc_boot_id); }
static int proc_sys_boot_id(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    // Stable for the lifetime of this runtime ("boot"), like Linux.
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, proc_boot_id_init);
    proc_print_uuid(buf, proc_boot_id); return 0;
}
static int proc_sys_uuid(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    uint8_t id[16]; proc_random_uuid(id); proc_print_uuid(buf, id); return 0;
}
static int proc_sys_pid_max(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "%d\n", MAX_PID); return 0;
}
// Linux DEFAULT_OVERFLOWUID/GID: the id reported for unmapped users (bubblewrap
// reads these before creating its namespaces).
static int proc_sys_overflow_id(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "%d\n", 65534); return 0;
}
static int proc_sys_file_max(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "%d\n", 1048576); return 0;
}
static int proc_sys_nr_open(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "%d\n", 1048576); return 0;
}
static struct proc_children proc_sys_kernel_random_children = PROC_CHILDREN({
    {"boot_id", .show = proc_sys_boot_id},
    {"uuid", .show = proc_sys_uuid},
});
static struct proc_children proc_sys_kernel_children = PROC_CHILDREN({
    {"hostname", .show = proc_sys_hostname},
    {"osrelease", .show = proc_sys_osrelease},
    {"ostype", .show = proc_sys_ostype},
    {"overflowgid", .show = proc_sys_overflow_id},
    {"overflowuid", .show = proc_sys_overflow_id},
    {"pid_max", .show = proc_sys_pid_max},
    {"random", S_IFDIR, .children = &proc_sys_kernel_random_children},
    {"version", .show = proc_sys_version},
});
static struct proc_children proc_sys_fs_children = PROC_CHILDREN({
    {"file-max", .show = proc_sys_file_max},
    {"nr_open", .show = proc_sys_nr_open},
});
static struct proc_children proc_sys_children = PROC_CHILDREN({
    {"fs", S_IFDIR, .children = &proc_sys_fs_children},
    {"kernel", S_IFDIR, .children = &proc_sys_kernel_children},
});

static int proc_readlink_thread_self(struct proc_entry *UNUSED(entry), char *buf) {
    sprintf(buf, "%d/task/%d", current->tgid, current->pid);
    return 0;
}

// Forward declaration for /proc/net
extern struct proc_children proc_net_children;

// in alphabetical order
struct proc_dir_entry proc_root_entries[] = {
    {"cmdline", .show = proc_show_cmdline},
    {"cpuinfo", .show = proc_show_cpuinfo},
    {"filesystems", .show = proc_show_filesystems},
    {"ish", S_IFDIR, .children = &proc_ish_children},
    {"loadavg", .show = proc_show_loadavg},
    {"meminfo", .show = proc_show_meminfo},
    {"mounts", .show = proc_show_mounts},
    {"net", S_IFDIR, .children = &proc_net_children},
    {"self", S_IFLNK, .readlink = proc_readlink_self},
    {"stat", .show = proc_show_stat},
    {"sys", S_IFDIR, .children = &proc_sys_children},
    {"thread-self", S_IFLNK, .readlink = proc_readlink_thread_self},
    {"uptime", .show = proc_show_uptime},
    {"version", .show = proc_show_version},
};
#define PROC_ROOT_LEN sizeof(proc_root_entries)/sizeof(proc_root_entries[0])

bool proc_root_lookup(const char *name, struct proc_entry *entry) {
    // Static entries (including self) retain their original identity/index.
    for (size_t i = 0; i < PROC_ROOT_LEN; i++) {
        if (!strcmp(name, proc_root_entries[i].name)) {
            *entry = (struct proc_entry) {.meta = &proc_root_entries[i], .index = i};
            return true;
        }
    }
    // Match exactly the decimal names emitted by proc_pid_getname. Reject
    // signs, leading zeroes and overflow before touching the PID table.
    if (name[0] < '1' || name[0] > '9')
        return false;
    dword_t pid = 0;
    for (const char *p = name; *p; p++) {
        if (*p < '0' || *p > '9' || pid > (MAX_PID - (*p - '0')) / 10)
            return false;
        pid = pid * 10 + (*p - '0');
    }
    lock(&pids_lock);
    bool exists = pid_get_task(pid) != NULL;
    unlock(&pids_lock);
    if (exists)
        *entry = (struct proc_entry) {.meta = &proc_pid, .index = pid + PROC_ROOT_LEN - 1, .pid = pid};
    return exists;
}

static bool proc_root_readdir(struct proc_entry *UNUSED(entry), unsigned long *index, struct proc_entry *next_entry) {
    if (*index < PROC_ROOT_LEN) {
        *next_entry = (struct proc_entry) {&proc_root_entries[*index], *index, NULL, NULL, 0, 0};
        (*index)++;
        return true;
    }

    pid_t_ pid = *index - PROC_ROOT_LEN;
    if (pid <= MAX_PID) {
        lock(&pids_lock);
        do {
            pid++;
        } while (pid <= MAX_PID && pid_get_task(pid) == NULL);
        unlock(&pids_lock);
        if (pid > MAX_PID)
            return false;
        *next_entry = (struct proc_entry) {&proc_pid, .pid = pid};
        *index = pid + PROC_ROOT_LEN;
        return true;
    }

    return false;
}

struct proc_dir_entry proc_root = {NULL, S_IFDIR, .readdir = proc_root_readdir};
