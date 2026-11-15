#include <limits.h>
#include <string.h>
#include "kernel/calls.h"
#include "platform/platform.h"

static bool resource_valid(int resource) {
    return resource >= 0 && resource < RLIMIT_NLIMITS_;
}

static int rlimit_get(struct task *task, int resource, struct rlimit_ *limit) {
    if (!resource_valid(resource))
        return _EINVAL;
    struct tgroup *group = task->group;
    lock(&group->lock);
    *limit = group->limits[resource];
    unlock(&group->lock);
    return 0;
}

static int rlimit_set(struct task *task, int resource, struct rlimit_ limit) {
    if (!resource_valid(resource))
        return _EINVAL;
    struct tgroup *group = task->group;
    lock(&group->lock);
    if (limit.cur > limit.max) { unlock(&group->lock); return _EINVAL; }
    if (!superuser() && limit.max > group->limits[resource].max) { unlock(&group->lock); return _EPERM; }
    group->limits[resource] = limit;
    unlock(&group->lock);
    return 0;
}

rlim_t_ rlimit(int resource) {
    struct rlimit_ limit;
    if (rlimit_get(current, resource, &limit) != 0)
        die("invalid resource %d", resource);
    return limit.cur;
}

static int do_getrlimit32(int resource, struct rlimit32_ *rlimit32) {
    STRACE("getlimit(%d)", resource);
    struct rlimit_ rlimit;
    int err = rlimit_get(current, resource, &rlimit);
    if (err < 0)
        return err;
    STRACE(" {cur=%#x, max=%#x}", rlimit.cur, rlimit.max);

    rlimit32->max = rlimit.max;
    rlimit32->cur = rlimit.cur;
    return 0;
}

dword_t sys_getrlimit32(dword_t resource, addr_t rlim_addr) {
    struct rlimit32_ rlimit;
    int err = do_getrlimit32(resource, &rlimit);
    if (err < 0)
        return err;
    if (user_put(rlim_addr, rlimit))
        return _EFAULT;
    return 0;
}

dword_t sys_old_getrlimit32(dword_t resource, addr_t rlim_addr) {
    struct rlimit32_ rlimit;
    int err = do_getrlimit32(resource, &rlimit);
    if (err < 0)
        return err;

    // This version of the call is for programs that aren't aware of rlim_t
    // being 64 bit. RLIM_INFINITY looks like -1 when truncated to 32 bits.
    if (rlimit.cur > INT_MAX)
        rlimit.cur = INT_MAX;
    if (rlimit.max > INT_MAX)
        rlimit.max = INT_MAX;

    if (user_put(rlim_addr, rlimit))
        return _EFAULT;
    return 0;
}

static int check_setrlimit(int resource, struct rlimit_ new_limit) {
    if (!resource_valid(resource) || new_limit.cur > new_limit.max) return _EINVAL;
    // Bound descriptor IDs to the Linux nr_open default, not the host limit.
    if (resource == RLIMIT_NOFILE_ && new_limit.max > 1048576) return _EPERM;
    if (superuser())
        return 0;
    struct rlimit_ old_limit;
    int err = rlimit_get(current, resource, &old_limit);
    if (err < 0)
        return err;
    if (new_limit.max > old_limit.max)
        return _EPERM;
    return 0;
}

dword_t sys_setrlimit32(dword_t resource, addr_t rlim_addr) {
    struct rlimit32_ input;
    if (user_get(rlim_addr, input)) return _EFAULT;
    struct rlimit_ rlimit = {input.cur, input.max};
    if (input.cur == (rlim32_t_) -1) rlimit.cur = RLIM_INFINITY_;
    if (input.max == (rlim32_t_) -1) rlimit.max = RLIM_INFINITY_;
    STRACE("setrlimit(%d, {cur=%#x, max=%#x})", resource, rlimit.cur, rlimit.max);
    int err = check_setrlimit(resource, rlimit);
    if (err < 0)
        return err;
    return rlimit_set(current, resource, rlimit);
}

// 64-bit getrlimit/setrlimit for ARM64 (struct rlimit with 64-bit cur/max)
dword_t sys_getrlimit64(dword_t resource, addr_t rlim_addr) {
    STRACE("getrlimit64(%d)", resource);
    struct rlimit_ rlimit;
    int err = rlimit_get(current, resource, &rlimit);
    if (err < 0)
        return err;
    STRACE(" {cur=%#llx, max=%#llx}", (unsigned long long)rlimit.cur, (unsigned long long)rlimit.max);
    if (user_put(rlim_addr, rlimit))
        return _EFAULT;
    return 0;
}

dword_t sys_setrlimit64(dword_t resource, addr_t rlim_addr) {
    struct rlimit_ rlimit;
    if (user_get(rlim_addr, rlimit))
        return _EFAULT;
    STRACE("setrlimit64(%d, {cur=%#llx, max=%#llx})", resource, (unsigned long long)rlimit.cur, (unsigned long long)rlimit.max);
    int err = check_setrlimit(resource, rlimit);
    if (err < 0)
        return err;
    return rlimit_set(current, resource, rlimit);
}

dword_t sys_prlimit64(pid_t_ pid, dword_t resource, addr_t new_addr, addr_t old_addr) {
    if (pid < 0 || !resource_valid(resource)) return _EINVAL;
    pid = pid_arg(pid);
    struct rlimit_ replacement, previous;
    if (new_addr && user_get(new_addr, replacement)) return _EFAULT;
    if (new_addr && replacement.cur > replacement.max) return _EINVAL;
    if (new_addr && resource == RLIMIT_NOFILE_ && replacement.max > 1048576) return _EPERM;
    lock(&pids_lock);
    struct task *target = pid ? pid_get_task(pid) : current;
    int result = 0;
    if (!target) { result = _ESRCH; goto out; }
    if (target != current && !superuser() &&
        (current->uid != target->uid || current->uid != target->euid || current->uid != target->suid ||
         current->gid != target->gid || current->gid != target->egid || current->gid != target->sgid)) {
        result = _EPERM; goto out;
    }
    lock(&target->group->lock);
    previous = target->group->limits[resource];
    if (new_addr) {
        if (!superuser() && replacement.max > previous.max) result = _EPERM;
        else target->group->limits[resource] = replacement;
    }
    unlock(&target->group->lock);
out:
    unlock(&pids_lock);
    if (!result && old_addr && user_put(old_addr, previous)) result = _EFAULT;
    return result;
}

struct rusage_ rusage_get_current() {
    // only the time fields are currently implemented
    struct platform_thread_cpu_usage usage = platform_get_thread_cpu_usage();
    return (struct rusage_) {
        .utime.sec = usage.user_sec,
        .utime.usec = usage.user_usec,
        .stime.sec = usage.system_sec,
        .stime.usec = usage.system_usec,
    };
}

static void timeval_add(struct timeval_ *dst, struct timeval_ *src);
struct rusage_ rusage_get_group_locked(struct tgroup *group) {
    lock(&group->lock);
    struct rusage_ total = group->rusage; // threads that already exited
    unlock(&group->lock);
    struct task *task;
    list_for_each_entry(&group->threads, task, group_links) {
        // An exiting thread adds its own time to group->rusage; skip it here.
        if (task->exiting || !task->thread)
            continue;
        struct platform_thread_cpu_usage usage;
        if (!platform_get_thread_cpu_usage_of(task->thread, &usage))
            continue;
        struct rusage_ thread = {
            .utime = {.sec = usage.user_sec, .usec = usage.user_usec},
            .stime = {.sec = usage.system_sec, .usec = usage.system_usec},
        };
        rusage_add(&total, &thread);
    }
    return total;
}

static void timeval_add(struct timeval_ *dst, struct timeval_ *src) {
    dst->sec += src->sec;
    dst->usec += src->usec;
    if (dst->usec >= 1000000) {
        dst->usec -= 1000000;
        dst->sec++;
    }
}

void rusage_add(struct rusage_ *dst, struct rusage_ *src) {
    timeval_add(&dst->utime, &src->utime);
    timeval_add(&dst->stime, &src->stime);
}

dword_t sys_getrusage(dword_t who, addr_t rusage_addr) {
    struct rusage_ rusage;
    switch (who) {
        case RUSAGE_SELF_:
            lock(&pids_lock);
            rusage = rusage_get_group_locked(current->group);
            unlock(&pids_lock);
            break;
        case RUSAGE_THREAD_:
            rusage = rusage_get_current();
            break;
        case RUSAGE_CHILDREN_:
            lock(&current->group->lock);
            rusage = current->group->children_rusage;
            unlock(&current->group->lock);
            break;
        default:
            return _EINVAL;
    }
    if (user_put(rusage_addr, rusage))
        return _EFAULT;
    return 0;
}

int_t sys_sched_getaffinity(pid_t_ pid, dword_t cpusetsize, addr_t cpuset_addr) {
    STRACE("sched_getaffinity(%d, %d, %#x)", pid, cpusetsize, cpuset_addr);
    pid = pid_arg(pid);
    if (pid != 0) {
        lock(&pids_lock);
        struct task *task = pid_get_task(pid);
        unlock(&pids_lock);
        if (task == NULL)
            return _ESRCH;
    }

    unsigned cpus = platform_guest_cpu_count();
    char cpuset[(PLATFORM_GUEST_CPU_MAX + 7) / 8];
    if (cpusetsize < sizeof(cpuset))
        return _EINVAL;
    memset(cpuset, 0, sizeof(cpuset));
    for (unsigned i = 0; i < cpus; i++)
        bit_set(i, cpuset);
    if (user_write(cpuset_addr, cpuset, sizeof(cpuset)))
        return _EFAULT;
    // return the number of bytes written
    return sizeof(cpuset);
}
int_t sys_sched_setaffinity(pid_t_ UNUSED(pid), dword_t UNUSED(cpusetsize), addr_t UNUSED(cpuset_addr)) {
    // meh
    return 0;
}

int_t sys_getcpu(addr_t cpu_addr, addr_t node_addr, addr_t UNUSED(tcache_addr)) {
    STRACE("getcpu(%#llx, %#llx)", (unsigned long long)cpu_addr, (unsigned long long)node_addr);
    dword_t cpu = 0;
    dword_t node = 0;
    if (cpu_addr && user_put(cpu_addr, cpu))
        return _EFAULT;
    if (node_addr && user_put(node_addr, node))
        return _EFAULT;
    return 0;
}

static int nice_target(int_t which, pid_t_ who, bool write) {
    if (which < 0 || which > 2 || who < 0) return _EINVAL;
    if (which != 2) who = pid_arg(who); // PRIO_PROCESS/PRIO_PGRP ids are pids
    if (!who) who = which == 0 ? current->pid : which == 1 ? current->group->pgid : current->uid;
    bool found = false; int result = 0;
    lock(&pids_lock);
    for (unsigned pid = 1; pid <= MAX_PID; pid++) {
        struct task *task = pid_get_task(pid);
        if (!task || task->exiting) continue;
        if ((which == 0 && task->pid != who) || (which == 1 && task->group->pgid != who) || (which == 2 && task->uid != who)) continue;
        found = true;
        if (write && !superuser() && current->euid != task->uid && current->euid != task->euid) { result = _EPERM; break; }
    }
    unlock(&pids_lock); return result ? result : found ? 0 : _ESRCH;
}
int_t sys_getpriority(int_t which, pid_t_ who) {
    int err = nice_target(which, who, false); return err ? err : 20;
}
int_t sys_setpriority(int_t which, pid_t_ who, int_t prio) {
    int err = nice_target(which, who, true); if (err) return err;
    // All guest threads currently have nice 0. Never silently pretend to alter
    // Linux fair-scheduler weights when only Darwin QoS donation is available.
    return prio ? _EOPNOTSUPP : 0;
}

// LinuxRunner currently runs guest threads in a timeshare scheduling domain.
// Do not report an unimplemented realtime policy as successfully activated.
static int sched_target(pid_t_ pid) {
    if (pid < 0) return _EINVAL;
    pid = pid_arg(pid);
    lock(&pids_lock);
    struct task *task = pid_get_task(pid ? pid : current->pid);
    int err = !task ? _ESRCH : !superuser() && current->euid != task->euid ? _EPERM : 0;
    unlock(&pids_lock); return err;
}
int_t sys_sched_getparam(pid_t_ pid, addr_t param_addr) {
    int err = sched_target(pid); if (err) return err;
    int_t priority = 0;
    return user_put(param_addr, priority) ? _EFAULT : 0;
}
int64_t sys_sched_setparam(pid_t_ pid, addr_t param_addr) {
    int_t priority; if (user_get(param_addr, priority)) return _EFAULT;
    int err = sched_target(pid); if (err) return err;
    return priority ? _EINVAL : 0; // valid unchanged SCHED_OTHER priority
}
int_t sys_sched_getscheduler(pid_t_ pid) {
    return sched_target(pid); // zero is the genuinely active SCHED_OTHER policy
}
int64_t sys_sched_setscheduler(pid_t_ pid, int_t policy, addr_t param_addr) {
    int_t priority; if (user_get(param_addr, priority)) return _EFAULT;
    int err = sched_target(pid); if (err) return err;
    policy &= ~0x40000000; // SCHED_RESET_ON_FORK; no RT state is inherited
    if (policy == 0) return priority ? _EINVAL : 0;
    if (policy == 1 || policy == 2) {
        if (priority < 1 || priority > 99) return _EINVAL;
        return _EPERM; // host cannot grant Linux realtime scheduling authority
    }
    if (policy == 3 || policy == 5) return priority ? _EINVAL : _EOPNOTSUPP;
    return _EINVAL;
}
int_t sys_sched_get_priority_max(int_t policy) {
    if (policy == 1 || policy == 2) return 99;
    if (policy == 0 || policy == 3 || policy == 5) return 0;
    return _EINVAL;
}
int_t sys_sched_get_priority_min(int_t policy) {
    if (policy == 1 || policy == 2) return 1;
    if (policy == 0 || policy == 3 || policy == 5) return 0;
    return _EINVAL;
}

int_t sys_ioprio_get(int_t UNUSED(which), int_t UNUSED(who), int_t UNUSED(ioprio)) {
    return 0;
}
int_t sys_ioprio_set(int_t UNUSED(which), int_t UNUSED(who), int_t UNUSED(ioprio)) {
    return 0;
}
