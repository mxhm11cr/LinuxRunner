#ifdef __APPLE__
#include <mach/mach.h>
#endif
#include "fs/proc.h"
#include "fs/proc/ish.h"
#include "kernel/errno.h"
#include "kernel/task.h"
#include "kernel/memory.h"
#include "kernel/futex.h"
#include "kernel/calls.h"
#include "kernel/ipc.h"
#include "asbestos/asbestos.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

const char *proc_ish_version = "";
char **(*get_all_defaults_keys)(void);
char *(*get_friendly_name)(const char *name);
char *(*get_underlying_name)(const char *name);
bool (*get_user_default)(const char *name, char **buffer, size_t *size);
bool (*set_user_default)(const char *name, char *buffer, size_t size);
bool (*remove_user_default)(const char *name);
char *(*get_documents_directory)(void);

static int proc_ish_show_colors(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf,
                "\x1B[30m" "iSH" "\x1B[39m "
                "\x1B[31m" "iSH" "\x1B[39m "
                "\x1B[32m" "iSH" "\x1B[39m "
                "\x1B[33m" "iSH" "\x1B[39m "
                "\x1B[34m" "iSH" "\x1B[39m "
                "\x1B[35m" "iSH" "\x1B[39m "
                "\x1B[36m" "iSH" "\x1B[39m "
                "\x1B[37m" "iSH" "\x1B[39m" "\n\x1B[7m"
                "\x1B[40m" "iSH" "\x1B[39m "
                "\x1B[41m" "iSH" "\x1B[39m "
                "\x1B[42m" "iSH" "\x1B[39m "
                "\x1B[43m" "iSH" "\x1B[39m "
                "\x1B[44m" "iSH" "\x1B[39m "
                "\x1B[45m" "iSH" "\x1B[39m "
                "\x1B[46m" "iSH" "\x1B[39m "
                "\x1B[47m" "iSH" "\x1B[39m" "\x1B[0m\x1B[1m\n"
                "\x1B[90m" "iSH" "\x1B[39m "
                "\x1B[91m" "iSH" "\x1B[39m "
                "\x1B[92m" "iSH" "\x1B[39m "
                "\x1B[93m" "iSH" "\x1B[39m "
                "\x1B[94m" "iSH" "\x1B[39m "
                "\x1B[95m" "iSH" "\x1B[39m "
                "\x1B[96m" "iSH" "\x1B[39m "
                "\x1B[97m" "iSH" "\x1B[39m" "\n\x1B[7m"
                "\x1B[100m" "iSH" "\x1B[39m "
                "\x1B[101m" "iSH" "\x1B[39m "
                "\x1B[102m" "iSH" "\x1B[39m "
                "\x1B[103m" "iSH" "\x1B[39m "
                "\x1B[104m" "iSH" "\x1B[39m "
                "\x1B[105m" "iSH" "\x1B[39m "
                "\x1B[106m" "iSH" "\x1B[39m "
                "\x1B[107m" "iSH" "\x1B[39m" "\x1B[0m\n"
                );
    return 0;
}

static int proc_ish_show_documents(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    if (get_documents_directory == NULL) {
        proc_printf(buf, "\n");
        return 0;
    }
    char *directory = get_documents_directory();
    proc_printf(buf, "%s\n", directory);
    free(directory);
    return 0;
}
// Do not fault in or expose memory contents: only bounded FP/LR addresses.
// Caller holds the task general lock; try-lock avoids blocking snapshot on a
// queued mapper. Each frame is best-effort because the guest may be running.
static void task_stack_snapshot(struct task *task, struct proc_data *buf) {
#ifdef GUEST_ARM64
    struct mem *mem = task->mem;
    uint64_t fp = __atomic_load_n(&task->cpu.regs[29], __ATOMIC_RELAXED);
    if (!mem || !read_wrtrylock(&mem->lock)) return;
    for (unsigned depth = 0; depth < 6; depth++) {
        if ((fp & 15) || fp > (USER_ADDR_MAX_PAGE << PAGE_BITS) || fp + 16 < fp) break;
        uint64_t frame[2]; bool valid = true;
        for (unsigned i = 0; i < 16; i++) {
            struct pt_entry *pte = mem_pt(mem, PAGE(fp + i));
            if (!pte || !(pte->flags & P_READ)) { valid = false; break; }
            ((unsigned char *)frame)[i] = *((unsigned char *)pte->data->data + pte->offset + PGOFFSET(fp + i));
        }
        if (!valid) break;
        proc_printf(buf, "TASK_FRAME: tid=%d depth=%u fp=%#llx lr=%#llx\n", task->pid, depth,
            (unsigned long long)fp, (unsigned long long)frame[1]);
        if (frame[0] <= fp || frame[0] - fp > 8 * 1024 * 1024) break;
        fp = frame[0];
    }
    read_wrunlock(&mem->lock);
#endif
}

// Runtime-specific, read-only diagnostics, not a fabricated Linux wchan.
// PC/SP and last syscall are sampled while tasks continue executing; they
// are best-effort observations, not a coherent stopped register frame.
extern _Atomic uint64_t asbestos_stat_retired_bytes;
extern _Atomic uint64_t asbestos_stat_instances, asbestos_stat_blocks, asbestos_stat_code_words, asbestos_stat_idle_flushes, asbestos_stat_cap_flushes, asbestos_stat_pressure_flushes;
static int proc_ish_show_memory(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
#ifdef __APPLE__
    task_vm_info_data_t info = {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
        proc_printf(buf, "host_footprint_bytes=%llu\nhost_resident_bytes=%llu\n",
            (unsigned long long)info.phys_footprint, (unsigned long long)info.resident_size);
#endif
    proc_printf(buf, "retired_code_bytes=%llu\n", (unsigned long long)atomic_load(&asbestos_stat_retired_bytes));
    uint64_t blocks = asbestos_stat_blocks, words = asbestos_stat_code_words;
    proc_printf(buf, "idle_flushes=%llu\ncap_flushes=%llu\npressure_flushes=%llu\n", (unsigned long long)asbestos_stat_idle_flushes, (unsigned long long)asbestos_stat_cap_flushes, (unsigned long long)asbestos_stat_pressure_flushes);
    proc_printf(buf, "translation_tables=%llu\nblocks=%llu\ncode_bytes=%llu\n",
        (unsigned long long)asbestos_stat_instances, (unsigned long long)blocks,
        (unsigned long long)(words * sizeof(unsigned long) + blocks * sizeof(struct fiber_block)));
    // Per address space (first task seen for each), best effort.
    lock(&pids_lock);
    struct mem *seen[256]; unsigned nseen = 0;
    for (unsigned pid = 1; pid <= MAX_PID; pid++) {
        struct task *task = pid_get_task(pid);
        if (!task || task->exiting || !task->mem) continue;
        bool dup = false; for (unsigned i = 0; i < nseen; i++) if (seen[i] == task->mem) dup = true;
        if (dup || nseen == 256) continue;
        seen[nseen++] = task->mem;
        struct asbestos *a = task->mem->mmu.asbestos;
        if (a) proc_printf(buf, "space pid=%d comm=%.16s blocks=%zu code_kb=%zu\n", task->tgid, task->comm,
            a->num_blocks, (a->mem_used * sizeof(unsigned long) + a->num_blocks * sizeof(struct fiber_block)) / 1024);
    }
    unlock(&pids_lock);
    return 0;
}
static int proc_ish_show_tasks(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "TASK_SNAPSHOT: best-effort; last_syscall may have returned; instrumented_wait covers only tracked waits\n");
    lock(&pids_lock);
    unsigned count = 0;
    for (unsigned pid = 1; pid <= MAX_PID && count < 1024; pid++) {
        struct task *task = pid_get_task(pid);
        if (!task || task->exiting || !task->group) continue;
        lock(&task->general_lock);
        proc_printf(buf, "TASK: tid=%d pid=%d ppid=%d comm=%.16s instrumented_wait=%d zombie=%d last_syscall=%u pc=%#llx sp=%#llx\n",
                task->pid, task->tgid, task_parent_pid(task),
                task->comm, task->blocking, task->zombie,
                __atomic_load_n(&task->syscall_restart_num, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&task->cpu.pc, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&task->cpu.sp, __ATOMIC_RELAXED));
#ifdef GUEST_ARM64
        proc_printf(buf, "TASK_REGS: tid=%d lr=%#llx x0=%#llx x1=%#llx x2=%#llx x3=%#llx x8=%#llx\n", task->pid,
            (unsigned long long)__atomic_load_n(&task->cpu.regs[30], __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&task->cpu.regs[0], __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&task->cpu.regs[1], __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&task->cpu.regs[2], __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&task->cpu.regs[3], __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&task->cpu.regs[8], __ATOMIC_RELAXED));
#endif
        proc_printf(buf, "TASK_PROGRESS: tid=%d blocks=%llu faults=%llu resolved=%llu fault_wait_ns=%llu fault_address=%#llx memory_stage=%u\n", task->pid,
            (unsigned long long)__atomic_load_n(&task->cpu.cycle, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&task->diagnostic_faults, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&task->diagnostic_resolved_faults, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&task->diagnostic_fault_ns, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&task->diagnostic_fault_address, __ATOMIC_RELAXED),
            __atomic_load_n(&task->diagnostic_memory_stage, __ATOMIC_RELAXED));
        task_stack_snapshot(task, buf);
        unlock(&task->general_lock); count++;
    }
    unlock(&pids_lock);
    return 0;
}

static void proc_ish_defaults_getname(struct proc_entry *entry, char *buf) {
    strcpy(buf, entry->name);
}

static int proc_ish_defaults_readlink(struct proc_entry *entry, char *buf) {
    char *name = get_underlying_name(entry->name);
    sprintf(buf, "../.defaults/%s", name);
    free(name);
    return 0;
}

static int proc_ish_underlying_defaults_show(struct proc_entry *entry, struct proc_data *data) {
    size_t size;
    char *buffer;
    if (get_user_default == NULL || !get_user_default(entry->name, &buffer, &size))
        return _EIO;
    proc_buf_append(data, buffer, size);
    free(buffer);
    return 0;
}

static int proc_ish_underlying_defaults_update(struct proc_entry *entry, struct proc_data *data) {
    if (set_user_default == NULL || !set_user_default(entry->name, data->data, data->size))
        return _EIO;
    return 0;
}

static int proc_ish_underlying_defaults_unlink(struct proc_entry *entry) {
    return (remove_user_default != NULL && remove_user_default(entry->name)) ? 0 : _EIO;
}

static int proc_ish_defaults_unlink(struct proc_entry *entry) {
    char *name = get_underlying_name(entry->name);
    int err = remove_user_default(name) ? 0 : _EIO;
    free(name);
    return err;
}

struct proc_dir_entry proc_ish_underlying_defaults_fd = { NULL,
    .getname = proc_ish_defaults_getname,
    .show = proc_ish_underlying_defaults_show,
    .update = proc_ish_underlying_defaults_update,
    .unlink = proc_ish_underlying_defaults_unlink,
};
struct proc_dir_entry proc_ish_defaults_fd = { NULL, S_IFLNK,
    .getname = proc_ish_defaults_getname,
    .readlink = proc_ish_defaults_readlink,
    .unlink = proc_ish_defaults_unlink,
};

static void get_child_names(struct proc_entry *entry, unsigned long index) {
    if (index == 0 || entry->child_names == NULL) {
        if (entry->child_names != NULL)
            free_string_array(entry->child_names);
        if (get_all_defaults_keys == NULL) {
            entry->child_names = NULL;
        } else {
            entry->child_names = get_all_defaults_keys();
        }
    }
}

static bool proc_ish_underlying_defaults_readdir(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    get_child_names(entry, *index);
    if (entry->child_names == NULL || entry->child_names[*index] == NULL)
        return false;
    next_entry->meta = &proc_ish_underlying_defaults_fd;
    next_entry->name = strdup(entry->child_names[*index]);
    (*index)++;
    return true;
}

static bool proc_ish_defaults_readdir(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    get_child_names(entry, *index);
    char *friendly_name;
    do {
        const char *name = NULL;
        if (entry->child_names != NULL) {
            name = entry->child_names[*index];
        }
        if (name == NULL)
            return false;
        friendly_name = get_friendly_name(name);
        (*index)++;
    } while (friendly_name == NULL);
    next_entry->meta = &proc_ish_defaults_fd;
    next_entry->name = friendly_name;
    return true;
}

static int proc_ish_show_version(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "%s\n", proc_ish_version);
    return 0;
}

#ifdef ISH_JIT
#include "asbestos/guest-arm64/jit.h"
static int proc_ish_jit_show(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    char *text = malloc(16384);
    if (!text) return _ENOMEM;
    jit_describe(text, 16384);
    proc_printf(buf, "%s", text);
    free(text);
    return 0;
}
static int proc_ish_jit_layout_show(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    char text[2048];
    jit_layout_describe(text,sizeof(text));
    proc_printf(buf,"%s",text);
    return 0;
}
static ssize_t proc_ish_jit_update(struct proc_entry *UNUSED(entry), struct proc_data *buf,
                                  off_t UNUSED(offset)) {
    return jit_control(buf->data, buf->size) ? (ssize_t)buf->size : _EINVAL;
}
#endif
static int proc_ish_show_pi(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    return futex_pi_show(buf);
}
static int proc_ish_show_ipctrace(struct proc_entry *UNUSED(entry),struct proc_data *buf) {
    char status[128];ipc_trace_dump(status,sizeof status);proc_printf(buf,"%s",status);return 0;
}
static void proc_fds_emit(const char *line,void *context) { proc_printf(context,"%s\n",line); }
static int proc_ish_show_fds(struct proc_entry *UNUSED(entry),struct proc_data *buf) {
    fd_global_census(proc_fds_emit,buf);return 0;
}
static int proc_ish_show_compat(struct proc_entry *UNUSED(entry),struct proc_data *buf) {
    compat_census(proc_fds_emit,buf);return 0;
}
static int proc_ish_show_shm(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct ipc_shm_usage u; ipc_shm_usage(&u);
    proc_printf(buf, "segments=%llu backing_fds=%llu bytes=%llu attachments=%llu capture_segments=%llu scopes=%llu close_failures=%llu\n",
        u.segments, u.backing_fds, u.bytes, u.attachments, u.capture_segments, u.scopes, u.close_failures);
    return 0;
}
struct proc_children proc_ish_children = PROC_CHILDREN({
    {"shm", S_IFREG | 0444, .show = proc_ish_show_shm},
    {"fds", S_IFREG | 0400, .show = proc_ish_show_fds},
    {"compat", S_IFREG | 0444, .show = proc_ish_show_compat},
    {"tasks", S_IFREG | 0400, .show = proc_ish_show_tasks},
    {"memory", S_IFREG | 0444, .show = proc_ish_show_memory},
    {"pi-futex", S_IFREG | 0400, .show = proc_ish_show_pi},
    {"ipctrace", S_IFREG | 0400, .show = proc_ish_show_ipctrace},
#ifdef ISH_JIT
    {"jit", S_IFREG | 0644, .show = proc_ish_jit_show, .pwrite = proc_ish_jit_update},
    {"jit-layout", S_IFREG | 0444, .show = proc_ish_jit_layout_show},
#endif
    {"colors", .show = proc_ish_show_colors},
    {".defaults", S_IFDIR, .readdir = proc_ish_underlying_defaults_readdir},
    {"defaults", S_IFDIR, .readdir = proc_ish_defaults_readdir},
    {"documents", .show = proc_ish_show_documents},
    {"version", .show = proc_ish_show_version},
});
