#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "kernel/memory.h"
#include "kernel/calls.h"
#include "fs/proc.h"
#include "fs/fd.h"
#include "fs/tty.h"
#include "kernel/fs.h"
#include "kernel/vdso.h"
#include "util/sync.h"

static void proc_pid_getname(struct proc_entry *entry, char *buf) {
    sprintf(buf, "%d", entry->pid);
}

static struct task *proc_get_task(struct proc_entry *entry) {
    lock(&pids_lock);
    struct task *task = pid_get_task(entry->pid);
    // Also reject tasks that are mid-exit: sighand/group may already be freed.
    if (task != NULL && (task->exiting || task->sighand == NULL || task->group == NULL))
        task = NULL;
    if (task == NULL)
        unlock(&pids_lock);
    return task;
}
static void proc_put_task(struct task *UNUSED(task)) {
    unlock(&pids_lock);
}

static int proc_pid_stat_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    // pids_lock keeps task/sighand alive. Snapshot signals separately: signal
    // delivery holds sighand while writing a frame, and stack growth asks for
    // rlimit under group->lock. Holding group (or general) while acquiring
    // sighand reverses that order and can freeze all PID operations.
    lock(&task->sighand->lock);
    uint32_t pending = task->pending & 0xffffffff;
    uint32_t blocked = task->blocked & 0xffffffff;
    uint32_t ignored = 0, caught = 0;
    for (int i = 0; i < 32; i++) {
        if (task->sighand->action[i].handler == SIG_IGN_)
            ignored |= 1u << i;
        else if (task->sighand->action[i].handler != SIG_DFL_)
            caught |= 1u << i;
    }
    unlock(&task->sighand->lock);
    // CPU time in clock ticks (USER_HZ 100), as ps/top/htop read it. pids_lock
    // is held by proc_get_task; group->lock must not be (taken inside).
    struct rusage_ cpu = rusage_get_group_locked(task->group);
    lock(&task->group->lock);
    struct rusage_ children = task->group->children_rusage;
    unlock(&task->group->lock);
#define LR_TICKS(tv) ((unsigned long)(tv).sec * 100ul + (unsigned long)(tv).usec / 10000ul)
    // Stack pointer and PC of the last guest instruction state, for readers
    // allowed to inspect the task (as with ptrace access), 0 otherwise. Decided
    // before general_lock: task_may_inspect takes it for same-uid readers.
    bool inspect = task_may_inspect(task);
    lock(&task->general_lock);
    lock(&task->group->lock);

    proc_printf(buf, "%d ", task->pid);
    proc_printf(buf, "(%.16s) ", task->comm);
    proc_printf(buf, "%c ",
            task->zombie ? 'Z' :
            task->group->stopped ? 'T' :
            task->blocking ? 'S' : 'R');
    proc_printf(buf, "%d ", task_parent_pid(task));
    proc_printf(buf, "%d ", task->group->pgid);
    proc_printf(buf, "%d ", task->group->sid);
    struct tty *tty = task->group->tty;
    proc_printf(buf, "%d ", tty ? dev_make(tty->driver->major, tty->num) : 0);
    proc_printf(buf, "%d ", tty ? tty->fg_group : 0);
    proc_printf(buf, "%u ", 0); // flags

    // page faults (no data available)
    proc_printf(buf, "%lu ", 0l); // minor faults
    proc_printf(buf, "%lu ", 0l); // children minor faults
    proc_printf(buf, "%lu ", 0l); // major faults
    proc_printf(buf, "%lu ", 0l); // children major faults

    proc_printf(buf, "%lu ", LR_TICKS(cpu.utime)); // user time
    proc_printf(buf, "%lu ", LR_TICKS(cpu.stime)); // system time
    proc_printf(buf, "%ld ", (long)LR_TICKS(children.utime)); // children user time
    proc_printf(buf, "%ld ", (long)LR_TICKS(children.stime)); // children system time
#undef LR_TICKS

    proc_printf(buf, "%ld ", 20l); // priority (not adjustable)
    proc_printf(buf, "%ld ", 0l); // nice (also not adjustable)
    proc_printf(buf, "%ld ", list_size(&task->group->threads));
    proc_printf(buf, "%ld ", 0l); // itimer value (deprecated, always 0)
    proc_printf(buf, "%llu ", (unsigned long long)task->start_ticks);

    proc_printf(buf, "%lu ", 0l); // vsize
    proc_printf(buf, "%ld ", 0l); // rss
    proc_printf(buf, "%lu ", 0l); // rss limit

    // Fields that require debugger-only/kernel-internal address details.
    proc_printf(buf, "%lu ", 0l); // startcode
    proc_printf(buf, "%lu ", 0l); // endcode
    proc_printf(buf, "%lu ", task->mm ? task->mm->stack_start : 0);
    proc_printf(buf, "%lu ", inspect ? (unsigned long) task->cpu.sp : 0l); // kstkesp
    proc_printf(buf, "%lu ", inspect ? (unsigned long) task->cpu.pc : 0l); // kstkeip

    proc_printf(buf, "%lu ", (unsigned long) pending);
    proc_printf(buf, "%lu ", (unsigned long) blocked);
    proc_printf(buf, "%lu ", (unsigned long) ignored);
    proc_printf(buf, "%lu ", (unsigned long) caught);

    proc_printf(buf, "%lu ", 0l); // wchan (wtf)
    proc_printf(buf, "%lu ", 0l); // nswap
    proc_printf(buf, "%lu ", 0l); // cnswap
    proc_printf(buf, "%d ", task->exit_signal);
    proc_printf(buf, "%d", 0); // processor
    // that's enough for now
    proc_printf(buf, "\n");

    unlock(&task->group->lock);
    unlock(&task->general_lock);
    proc_put_task(task);
    return 0;
}

// Guest process identity, credentials and signal masks. Host RSS/security
// policy is deliberately not represented as Linux memory/capability accounting.
static int proc_pid_status_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL) return _ESRCH;
    lock(&task->sighand->lock);
    sigset_t_ pending = task->pending, blocked = task->blocked;
    sigset_t_ ignored = 0, caught = 0;
    for (unsigned i = 1; i < NUM_SIGS; i++) {
        if (task->sighand->action[i].handler == SIG_IGN_) ignored |= sig_mask(i);
        else if (task->sighand->action[i].handler != SIG_DFL_) caught |= sig_mask(i);
    }
    unlock(&task->sighand->lock);
    lock(&task->general_lock);
    lock(&task->group->lock);
    char name[sizeof(task->comm)];
    memcpy(name, task->comm, sizeof(name));
    name[sizeof(name)-1] = 0;
    for (char *c = name; *c; c++) if (*c == '\n' || *c == '\t') *c = '?';
    proc_printf(buf, "Name:\t%s\nState:\t%c\nTgid:\t%d\nPid:\t%d\nPPid:\t%d\n",
        name, task->zombie ? 'Z' : task->group->stopped ? 'T' : task->blocking ? 'S' : 'R',
        task->tgid, task->pid, task_parent_pid(task));
    proc_printf(buf, "Uid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\n",
        task->uid, task->euid, task->suid, task->euid,
        task->gid, task->egid, task->sgid, task->egid);
    proc_printf(buf, "Groups:\t");
    for (unsigned i = 0; i < task->ngroups; i++) proc_printf(buf, "%u ", task->groups[i]);
    proc_printf(buf, "\nThreads:\t%lu\nSigPnd:\t%016llx\nSigBlk:\t%016llx\nSigIgn:\t%016llx\nSigCgt:\t%016llx\n",
        (unsigned long)list_size(&task->group->threads), (unsigned long long)pending,
        (unsigned long long)blocked, (unsigned long long)ignored, (unsigned long long)caught);
    unlock(&task->group->lock);
    unlock(&task->general_lock);
    proc_put_task(task);
    return 0;
}

static int proc_pid_statm_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;

    proc_printf(buf, "%lu ", 0); // total vm size
    proc_printf(buf, "%lu ", 0); // vm resident size
    proc_printf(buf, "%lu ", 0); // resident shared
    proc_printf(buf, "%lu ", 0); // text
    proc_printf(buf, "%lu ", 0); // lib (always 0 since linux 2.6)
    proc_printf(buf, "%lu ", 0); // data + stack
    proc_printf(buf, "%lu ", 0); // dirty (always 0 since linux 2.6)
    proc_printf(buf, "\n");

    proc_put_task(task);
    return 0;
}

static int proc_pid_auxv_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    if (!task_may_inspect(task)) { proc_put_task(task); return _EACCES; }
    int err = 0;
    lock(&task->general_lock);
    if (task->mm == NULL)
        goto out_free_task;

    size_t size = task->mm->auxv_end - task->mm->auxv_start;
    char *data = malloc(size);
    if (data == NULL) {
        err = _ENOMEM;
        goto out_free_task;
    }
    if (user_read_task(task, task->mm->auxv_start, data, size) == 0)
        proc_buf_append(buf, data, size);
    free(data);

out_free_task:
    unlock(&task->general_lock);
    proc_put_task(task);
    return err;
}

static int proc_pid_cmdline_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    int err = 0;
    lock(&task->general_lock);
    if (task->mm == NULL)
        goto out_free_task;

    size_t size = task->mm->argv_end - task->mm->argv_start;
    char *data = malloc(size);
    if (data == NULL) {
        err = _ENOMEM;
        goto out_free_task;
    }
    if (user_read_task(task, task->mm->argv_start, data, size) == 0)
        proc_buf_append(buf, data, size);
    free(data);

out_free_task:
    unlock(&task->general_lock);
    proc_put_task(task);
    return err;
}

static int proc_pid_comm_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if (!task) return _ESRCH;
    lock(&task->general_lock);
    proc_printf(buf, "%.16s\n", task->comm);
    unlock(&task->general_lock); proc_put_task(task); return 0;
}
// oom_score_adj: Chromium raises renderer values; lowering needs privilege.
static int proc_pid_oom_score_adj_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if (!task) return _ESRCH;
    proc_printf(buf, "%d\n", atomic_load(&task->group->oom_score_adj));
    proc_put_task(task);
    return 0;
}
static ssize_t proc_pid_oom_score_adj_write(struct proc_entry *entry, struct proc_data *data, off_t off) {
    if (off != 0) return _EINVAL;
    char text[16]; if (data->size >= sizeof(text)) return _EINVAL;
    memcpy(text, data->data, data->size); text[data->size] = 0;
    char *end; long value = strtol(text, &end, 10);
    if (end == text || (*end && *end != '\n') || value < -1000 || value > 1000) return _EINVAL;
    struct task *task = proc_get_task(entry);
    if (!task) return _ESRCH;
    int err = (int) data->size;
    if (value < atomic_load(&task->group->oom_score_adj) && !superuser()) err = _EACCES;
    else atomic_store(&task->group->oom_score_adj, (int) value);
    proc_put_task(task);
    return err;
}

// user_namespaces(7): uid_map, gid_map, setgroups and ns/user.
static int proc_pid_idmap_show_kind(struct proc_entry *entry, struct proc_data *buf, int kind) {
    struct task *task = proc_get_task(entry);
    if (!task) return _ESRCH;
    char text[96]; int n = userns_show_map(task, kind, text, sizeof(text));
    proc_put_task(task);
    if (n > 0) proc_printf(buf, "%s", text);
    return 0;
}
static ssize_t proc_pid_idmap_write_kind(struct proc_entry *entry, struct proc_data *data, off_t off, int kind) {
    if (off != 0) return _EINVAL;
    struct task *task = proc_get_task(entry);
    if (!task) return _ESRCH;
    int result = userns_write_map(task, kind, data->data, data->size);
    proc_put_task(task);
    return result;
}
static int proc_pid_uid_map_show(struct proc_entry *e, struct proc_data *b) { return proc_pid_idmap_show_kind(e, b, 0); }
static int proc_pid_gid_map_show(struct proc_entry *e, struct proc_data *b) { return proc_pid_idmap_show_kind(e, b, 1); }
static int proc_pid_setgroups_show(struct proc_entry *e, struct proc_data *b) { return proc_pid_idmap_show_kind(e, b, 2); }
static ssize_t proc_pid_uid_map_write(struct proc_entry *e, struct proc_data *d, off_t o) { return proc_pid_idmap_write_kind(e, d, o, 0); }
static ssize_t proc_pid_gid_map_write(struct proc_entry *e, struct proc_data *d, off_t o) { return proc_pid_idmap_write_kind(e, d, o, 1); }
static ssize_t proc_pid_setgroups_write(struct proc_entry *e, struct proc_data *d, off_t o) { return proc_pid_idmap_write_kind(e, d, o, 2); }
static int proc_pid_ns_user_readlink(struct proc_entry *entry, char *buf) {
    struct task *task = proc_get_task(entry);
    if (!task) return _ESRCH;
    sprintf(buf, "user:[%llu]", userns_inum(task->userns));
    proc_put_task(task);
    return 0;
}
// Linux ns entries are magic links resolving to an nsfs object; existence
// checks (Chromium's KernelSupportsUnprivilegedNamespace) stat through them.
extern const struct fd_ops procfs_fdops;
static int proc_pid_ns_object_show(struct proc_entry *UNUSED(entry), struct proc_data *UNUSED(buf)) { return _EINVAL; }
static uint64_t proc_pid_ns_inode(struct proc_entry *entry, bool pid_ns) {
    struct task *task = proc_get_task(entry);
    if (!task) return 0;
    uint64_t inum = pid_ns ? pidns_inum(task->pidns) : userns_inum(task->userns);
    proc_put_task(task);
    return inum;
}
static uint64_t proc_pid_ns_user_inode(struct proc_entry *entry) { return proc_pid_ns_inode(entry, false); }
static uint64_t proc_pid_ns_pid_inode(struct proc_entry *entry) { return proc_pid_ns_inode(entry, true); }
static struct proc_dir_entry proc_pid_ns_user_object = {"user", S_IFREG | 0444, .show = proc_pid_ns_object_show, .inode_of = proc_pid_ns_user_inode};
static struct proc_dir_entry proc_pid_ns_pid_object = {"pid", S_IFREG | 0444, .show = proc_pid_ns_object_show, .inode_of = proc_pid_ns_pid_inode};
static struct fd *proc_pid_ns_follow(struct proc_entry *entry, struct proc_dir_entry *object) {
    struct task *task = proc_get_task(entry);
    if (!task) return ERR_PTR(_ESRCH);
    proc_put_task(task);
    struct fd *fd = fd_create(&procfs_fdops);
    if (!fd) return ERR_PTR(_ENOMEM);
    fd->proc.entry = (struct proc_entry) {.meta = object, .pid = entry->pid};
    char path[64]; snprintf(path, sizeof path, "/proc/%d/ns/%s", entry->pid, object->name);
    fd->proc.path = strdup(path);
    if (!fd->proc.path) { fd_close(fd); return ERR_PTR(_ENOMEM); }
    fd->proc.data.data = NULL;
    return fd;
}
static struct fd *proc_pid_ns_user_follow_link(struct proc_entry *entry, bool UNUSED(for_stat)) { return proc_pid_ns_follow(entry, &proc_pid_ns_user_object); }
static struct fd *proc_pid_ns_pid_follow_link(struct proc_entry *entry, bool UNUSED(for_stat)) { return proc_pid_ns_follow(entry, &proc_pid_ns_pid_object); }
static int proc_pid_ns_pid_readlink(struct proc_entry *entry, char *buf) {
    struct task *task = proc_get_task(entry);
    if (!task) return _ESRCH;
    sprintf(buf, "pid:[%llu]", pidns_inum(task->pidns));
    proc_put_task(task);
    return 0;
}
static struct proc_children proc_pid_ns_children = PROC_CHILDREN({
    {"pid", S_IFLNK, .readlink = proc_pid_ns_pid_readlink, .follow_link = proc_pid_ns_pid_follow_link},
    {"user", S_IFLNK, .readlink = proc_pid_ns_user_readlink, .follow_link = proc_pid_ns_user_follow_link},
});

static int proc_pid_environ_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if (!task) return _ESRCH;
    if (!task_may_inspect(task)) { proc_put_task(task); return _EACCES; }
    lock(&task->general_lock);
    int result = 0;
    if (task->mm && task->mm->env_end >= task->mm->env_start) {
        size_t size = task->mm->env_end - task->mm->env_start;
        if (size > 32 * PAGE_SIZE) result = _EIO;
        else if (size) {
            char *data = malloc(size);
            if (!data) result = _ENOMEM;
            else { if (user_read_task(task, task->mm->env_start, data, size)) result = _EFAULT;
                else proc_buf_append(buf, data, size); free(data); }
        }
    }
    unlock(&task->general_lock); proc_put_task(task); return result;
}

void proc_maps_dump(struct task *task, struct proc_data *buf) {
    struct mem *mem = task->mem;
    if (mem == NULL)
        return;

    read_wrlock(&mem->lock);
    page_t page = 0;
    while (page < MEM_PAGES) {
        // find a region
        while (page < MEM_PAGES && mem_pt(mem, page) == NULL) {
            mem_next_page(mem, &page);
        }
        if (page >= MEM_PAGES)
            break;
        page_t start = page;
        struct pt_entry *start_pt = mem_pt(mem, start);
        struct data *data = start_pt->data;

        // find the end of said region
        while (page < MEM_PAGES) {
            struct pt_entry *pt = mem_pt(mem, page);
            if (pt == NULL)
                break;
            if ((pt->flags & (P_RWX | P_SHARED)) != (start_pt->flags & (P_RWX | P_SHARED)))
                break;
            // region continues if data is the same or both are anonymous
            if (!(pt->data == data || (pt->flags & P_ANONYMOUS && start_pt->flags & P_ANONYMOUS)))
                break;
            mem_next_page(mem, &page);
        }
        page_t end = page;

        // output info
        char path[MAX_PATH] = "";
        if (start_pt->flags & P_GROWSDOWN) {
            strcpy(path, "[stack]");
        } else if (data->name != NULL) {
            strcpy(path, data->name);
        } else if (data->fd != NULL) {
            generic_getpath(start_pt->data->fd, path);
        }
        // mprotect and partial munmap split a backing into several reported
        // ranges. Their offsets advance from the first retained guest page,
        // excluding the native-page alignment prefix. Anonymous offsets stay 0.
        size_t file_offset = data->fd ?
            data->file_offset + start_pt->offset - data->mapping_offset : 0;
#ifdef GUEST_ARM64
        proc_printf(buf, "%012llx-%012llx %c%c%c%c %08lx 00:00 %-10d %s\n",
                (unsigned long long)(start << PAGE_BITS),
                (unsigned long long)(end << PAGE_BITS),
#else
        proc_printf(buf, "%08x-%08x %c%c%c%c %08lx 00:00 %-10d %s\n",
                start << PAGE_BITS, end << PAGE_BITS,
#endif
                start_pt->flags & P_READ ? 'r' : '-',
                start_pt->flags & P_WRITE ? 'w' : '-',
                start_pt->flags & P_EXEC ? 'x' : '-',
                start_pt->flags & P_SHARED ? 's' : 'p',
                (unsigned long) file_offset,
                0, // inode
                path);
    }
    read_wrunlock(&mem->lock);
}

static int proc_pid_maps_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    if (!task_may_inspect(task)) { proc_put_task(task); return _EACCES; }
    lock(&task->general_lock);
    proc_maps_dump(task, buf);
    unlock(&task->general_lock);
    proc_put_task(task);
    return 0;
}

static ssize_t proc_pid_mem_pread(struct proc_entry *entry, struct proc_data *buf, off_t offset) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    if (!task_may_inspect(task)) { proc_put_task(task); return _EACCES; }
    lock(&task->general_lock);
    int result = task->mem == NULL ? -1 :
        user_read_task(task, (addr_t)offset, buf->data, buf->size);
    unlock(&task->general_lock);
    proc_put_task(task);
    return result ? -1 : buf->size;
}

static ssize_t proc_pid_mem_pwrite(struct proc_entry *entry, struct proc_data *buf, off_t offset) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    if (!task_may_inspect(task)) { proc_put_task(task); return _EACCES; }
    lock(&task->general_lock);
    int result = task->mem == NULL ? -1 :
        user_write_task_ptrace(task, (addr_t)offset, buf->data, buf->size);
    unlock(&task->general_lock);
    proc_put_task(task);
    return result ? -1 : buf->size;
}

static int proc_pid_mem_seek(struct proc_entry *UNUSED(entry), qword_t *position,
        off_t_ off, int whence) {
    if (whence == LSEEK_SET)
        *position = (qword_t) off;
    else if (whence == LSEEK_CUR)
        *position += (qword_t) off;
    else
        return _EINVAL;
    return 0;
}


static struct proc_dir_entry proc_pid_fd;
static struct proc_dir_entry proc_pid_fdinfo;

static bool proc_pid_fd_readdir(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    struct task *task = proc_get_task(entry);
    // Task exited while its /proc/<pid>/fd dir was being iterated (e.g.
    // `find /proc` racing short-lived children). This function returns bool —
    // the old `return _ESRCH` here (-3) read as TRUE, so proc_readdir consumed
    // a next_entry that was never filled in (meta == NULL) and crashed in
    // proc_entry_getname (EXC_BAD_ACCESS at 0x10). No task = no more entries.
    if (task == NULL)
        return false;
    // Retain the fdtable, then drop pids_lock before taking files->lock. Other
    // paths take those locks in the opposite order, so overlapping them here
    // can deadlock concurrent /proc/<pid>/fd scans.
    struct fdtable *files = task->files;
    files->refcount++;
    proc_put_task(task);
    lock(&files->lock);
    while (*index < files->size && files->files[*index] == NULL)
        (*index)++;
    fd_t f = (*index)++;
    bool any_left = (unsigned) f < files->size;
    unlock(&files->lock);
    fdtable_release(files);
    *next_entry = (struct proc_entry) {&proc_pid_fd, .pid = entry->pid, .fd = f};
    return any_left;
}

bool proc_pid_fd_same_group(struct proc_entry *entry) {
    if (entry->meta->readdir != proc_pid_fd_readdir) return false;
    struct task *task = proc_get_task(entry);
    if (!task) return false;
    bool same = task->tgid == current->tgid;
    proc_put_task(task);
    return same;
}

static void proc_pid_fd_getname(struct proc_entry *entry, char *buf) {
    sprintf(buf, "%d", entry->fd);
}

static bool proc_pid_fdinfo_readdir(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    if (!proc_pid_fd_readdir(entry, index, next_entry)) return false;
    next_entry->meta = &proc_pid_fdinfo; return true;
}
static int proc_pid_fdinfo_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if (!task) return _ESRCH;
    if (!task_may_inspect(task)) { proc_put_task(task); return _EACCES; }
    struct fdtable *files = task->files; files->refcount++; proc_put_task(task);
    lock(&files->lock);
    struct fd *fd = fdtable_get(files, entry->fd);
    bool cloexec = fd && bit_test(entry->fd, files->cloexec);
    if (fd) fd_retain(fd);
    unlock(&files->lock); fdtable_release(files);
    if (!fd) return _ENOENT;
    struct statbuf st; int err = fd->mount->fs->fstat(fd, &st);
    if (!err) {
        unsigned flags = fd_getflags(fd) | (cloexec ? O_CLOEXEC_ : 0);
        off_t_ position = fd->ops->lseek ? fd->ops->lseek(fd, 0, LSEEK_CUR) : fd->offset;
        if (position < 0) position = 0; // pipes/sockets have no seek offset
        proc_printf(buf, "pos:\t%llu\nflags:\t0%o\nino:\t%llu\n", (unsigned long long)position, flags, (unsigned long long)st.inode);
        // pidfd (Linux 5.10+ fdinfo): the target in the reader's PID namespace,
        // -1 once it has exited. dbus-daemon and portals resolve SO_PEERPIDFD
        // peers through this line.
        pid_t_ target;
        if (fd_pidfd_target(fd, &target)) {
            lock(&pids_lock);
            struct task *t = pid_get_task(target);
            pid_t_ shown = t != NULL && !t->zombie ? pid_view(target, current->pidns) : -1;
            unlock(&pids_lock);
            proc_printf(buf, "Pid:\t%d\nNSpid:\t%d\n", shown, shown);
        }
    }
    fd_close(fd); return err;
}

static struct fd *proc_pid_fd_target(struct proc_entry *entry) {
    struct task *task = proc_get_task(entry);
    if (!task) return ERR_PTR(_ESRCH);
    // Virtual credentials only; never use host privileges for proc access.
    if (!task_may_inspect(task)) {
        proc_put_task(task);
        return ERR_PTR(_EACCES);
    }
    lock(&task->files->lock);
    struct fd *fd = fdtable_get(task->files, entry->fd);
    if (fd) fd_retain(fd);
    unlock(&task->files->lock);
    proc_put_task(task);
    if (!fd) return ERR_PTR(_ENOENT);
    return fd;
}

static struct fd *proc_pid_fd_follow_link(struct proc_entry *entry, bool for_stat) {
    struct fd *fd = proc_pid_fd_target(entry);
    if (IS_ERR(fd)) return fd;
    // Regular files retain the existing pathname open semantics. Pipes and
    // anonymous non-reopenable objects require semantic, not textual, lookup.
    // stat always follows the retained inode, even after unlink or for memfd.
    if (!for_stat && !fd->memfd && (fd->type == S_IFREG || fd->type == S_IFDIR || fd->type == S_IFCHR)) {
        fd_close(fd); return NULL;
    }
    return fd;
}

static int proc_pid_fd_readlink(struct proc_entry *entry, char *buf) {
    struct fd *fd = proc_pid_fd_target(entry);
    if (IS_ERR(fd)) return PTR_ERR(fd);
    // No pids/table lock is held across filesystem callbacks.
    int err = generic_getpath(fd, buf);
    fd_close(fd);
    return err;
}

static int proc_pid_exe_readlink(struct proc_entry *entry, char *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    if (!task_may_inspect(task)) { proc_put_task(task); return _EACCES; }
    lock(&task->general_lock);
    int err;
    if (task->mm == NULL || task->mm->exefile == NULL)
        err = _ENOENT;
    else
        err = generic_getpath(task->mm->exefile, buf);
    unlock(&task->general_lock);
    proc_put_task(task);
    return err;
}

static void proc_pid_task_getname(struct proc_entry *entry, char *buf) {
    sprintf(buf, "%d", entry->pid);
}

static struct proc_dir_entry proc_pid_task;

static bool proc_pid_task_readdir(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    struct task *parent = proc_get_task(entry);
    if (!parent) return false;
    unsigned tid = *index;
    while (++tid <= MAX_PID) {
        struct task *thread = pid_get_task(tid);
        if (!thread || thread->exiting || thread->tgid != parent->tgid) continue;
        *next_entry = (struct proc_entry) {&proc_pid_task, .pid = tid};
        *index = tid; proc_put_task(parent); return true;
    }
    proc_put_task(parent); return false;
}

static uint_t proc_pid_task_nlink(struct proc_entry *entry) {
    struct task *task = proc_get_task(entry);
    if (!task) return 2;
    uint_t links = 2; // '.' and '..', plus one subdirectory per live thread.
    lock(&task->group->lock);
    struct task *thread;
    list_for_each_entry(&task->group->threads, thread, group_links)
        if (!thread->exiting) links++;
    unlock(&task->group->lock);
    proc_put_task(task);
    return links;
}

static int proc_pid_cwd_readlink(struct proc_entry *entry, char *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    if (!task_may_inspect(task)) { proc_put_task(task); return _EACCES; }
    lock(&task->fs->lock);
    int err = generic_getpath(task->fs->pwd, buf);
    unlock(&task->fs->lock);
    proc_put_task(task);
    return err;
}


// One filesystem root for every process (no chroot/pivot_root namespaces).
static int proc_pid_root_readlink(struct proc_entry *entry, char *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    bool allowed = task_may_inspect(task);
    proc_put_task(task);
    if (!allowed)
        return _EACCES;
    strcpy(buf, "/");
    return 0;
}

// Linux format: "nr arg0 ... arg5 sp pc" while in a syscall, "running" otherwise.
static int proc_pid_syscall_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    bool allowed = task_may_inspect(task);
    int nr = atomic_load_explicit(&task->current_syscall, memory_order_acquire);
    uint64_t args[6], pc = task->current_syscall_pc, sp = task->cpu.sp;
    memcpy(args, task->current_syscall_args, sizeof args);
    proc_put_task(task);
    if (!allowed)
        return _EACCES;
    if (!nr) { proc_printf(buf, "running\n"); return 0; }
    proc_printf(buf, "%d", nr - 1);
    for (int i = 0; i < 6; i++) proc_printf(buf, " %#llx", (unsigned long long) args[i]);
    proc_printf(buf, " %#llx %#llx\n", (unsigned long long) sp, (unsigned long long) pc);
    return 0;
}

struct proc_children proc_pid_children = PROC_CHILDREN({
    {"auxv", .show = proc_pid_auxv_show},
    {"cmdline", .show = proc_pid_cmdline_show},
    {"comm", .show = proc_pid_comm_show},
    {"cwd", S_IFLNK, .readlink = proc_pid_cwd_readlink},
    {"exe", S_IFLNK, .readlink = proc_pid_exe_readlink},
    {"environ", S_IFREG | 0400, .show = proc_pid_environ_show},
    {"fd", S_IFDIR | 0500, .readdir = proc_pid_fd_readdir},
    {"fdinfo", S_IFDIR | 0500, .readdir = proc_pid_fdinfo_readdir},
    {"gid_map", S_IFREG | 0644, .show = proc_pid_gid_map_show, .pwrite = proc_pid_gid_map_write},
    {"maps", .show = proc_pid_maps_show},
    {"mountinfo", .show = proc_show_mountinfo},
    {"mounts", .show = proc_show_mounts},
    // 0600 like Linux; access also requires ptrace-style inspection rights.
    {"mem", S_IFREG | 0600, .pread = proc_pid_mem_pread, .pwrite = proc_pid_mem_pwrite,
        .seek = proc_pid_mem_seek},
    {"ns", S_IFDIR | 0511, .children = &proc_pid_ns_children},
    {"oom_score_adj", S_IFREG | 0644, .show = proc_pid_oom_score_adj_show, .pwrite = proc_pid_oom_score_adj_write},
    {"root", S_IFLNK, .readlink = proc_pid_root_readlink},
    {"setgroups", S_IFREG | 0644, .show = proc_pid_setgroups_show, .pwrite = proc_pid_setgroups_write},
    {"stat", .show = proc_pid_stat_show},
    {"statm", .show = proc_pid_statm_show},
    {"syscall", S_IFREG | 0400, .show = proc_pid_syscall_show},
    {"status", .show = proc_pid_status_show},
    {"task", S_IFDIR, .readdir = proc_pid_task_readdir, .nlink = proc_pid_task_nlink},
    {"uid_map", S_IFREG | 0644, .show = proc_pid_uid_map_show, .pwrite = proc_pid_uid_map_write},
});

struct proc_dir_entry proc_pid = {NULL, S_IFDIR,
    .children = &proc_pid_children, .getname = proc_pid_getname};

static struct proc_dir_entry proc_pid_fd = {NULL, S_IFLNK,
    .getname = proc_pid_fd_getname, .readlink = proc_pid_fd_readlink,
    .follow_link = proc_pid_fd_follow_link};

static struct proc_dir_entry proc_pid_fdinfo = {NULL, S_IFREG | 0400,
    .getname = proc_pid_fd_getname, .show = proc_pid_fdinfo_show};

static struct proc_dir_entry proc_pid_task = {NULL, S_IFDIR,
    .getname = proc_pid_task_getname, .children = &proc_pid_children};
