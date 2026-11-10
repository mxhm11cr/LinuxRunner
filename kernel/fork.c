#include <time.h>
#include <stddef.h>
#include "debug.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "kernel/calls.h"
#include "fs/tty.h"
#include "kernel/mm.h"
#include "kernel/ptrace.h"
#include "kernel/ipc.h"

// Architecture-specific register access for fork/clone
#if defined(GUEST_ARM64)
#define CPU_SP(cpu) ((cpu).sp)
#define CPU_RETVAL(cpu) ((cpu).regs[0])
#else
#define CPU_SP(cpu) ((cpu).esp)
#define CPU_RETVAL(cpu) ((cpu).eax)
#endif

#define CSIGNAL_ 0x000000ff
#define CLONE_VM_ 0x00000100
#define CLONE_FS_ 0x00000200
#define CLONE_FILES_ 0x00000400
#define CLONE_SIGHAND_ 0x00000800
#define CLONE_PTRACE_ 0x00002000
#define CLONE_VFORK_ 0x00004000
#define CLONE_PARENT_ 0x00008000
#define CLONE_THREAD_ 0x00010000
#define CLONE_NEWNS_ 0x00020000
#define CLONE_SYSVSEM_ 0x00040000
#define CLONE_SETTLS_ 0x00080000
#define CLONE_PIDFD_ 0x00001000
#define CLONE_PARENT_SETTID_ 0x00100000
#define CLONE_CHILD_CLEARTID_ 0x00200000
#define CLONE_DETACHED_ 0x00400000
#define CLONE_UNTRACED_ 0x00800000
#define CLONE_CHILD_SETTID_ 0x01000000
#define CLONE_NEWCGROUP_ 0x02000000
#define CLONE_NEWUTS_ 0x04000000
#define CLONE_NEWIPC_ 0x08000000
#define CLONE_NEWUSER_ 0x10000000
#define CLONE_NEWPID_ 0x20000000
#define CLONE_NEWNET_ 0x40000000
#define CLONE_IO_ 0x80000000
#define IMPLEMENTED_FLAGS (CLONE_VM_|CLONE_FILES_|CLONE_FS_|CLONE_SIGHAND_|CLONE_SYSVSEM_|CLONE_VFORK_|CLONE_THREAD_|\
        CLONE_SETTLS_|CLONE_CHILD_SETTID_|CLONE_PARENT_SETTID_|CLONE_CHILD_CLEARTID_|CLONE_DETACHED_|CLONE_PIDFD_)

static struct tgroup *tgroup_copy(struct tgroup *old_group) {
    struct tgroup *group = malloc(sizeof(struct tgroup));
    *group = *old_group;
    list_init(&group->threads);
    list_add(&old_group->pgroup, &group->pgroup);
    list_add(&old_group->session, &group->session);
    if (group->tty) {
        lock(&group->tty->lock);
        group->tty->refcount++;
        unlock(&group->tty->lock);
    }
    group->itimer = NULL;
    group->doing_group_exit = false;
    // A new process starts with no CPU time of its own or of children.
    group->rusage = (struct rusage_) {};
    group->children_rusage = (struct rusage_) {};
    {
        struct timespec _ts;
        clock_gettime(CLOCK_MONOTONIC, &_ts);
        atomic_store_explicit(&group->last_progress_ns,
            (uint64_t)_ts.tv_sec * 1000000000ULL + _ts.tv_nsec,
            memory_order_relaxed);
    }
    cond_init(&group->child_exit);
    cond_init(&group->stopped_cond);
    lock_init(&group->lock);
    return group;
}

static int copy_task(struct task *task, dword_t flags, addr_t stack, addr_t ptid_addr, addr_t tls_addr, addr_t ctid_addr) {
    task->vfork = NULL;
    if (stack != 0)
        CPU_SP(task->cpu) = stack;

    int err;
    struct mm *mm = task->mm;
    if (flags & CLONE_VM_) {
        mm_retain(mm);
    } else {
        task_set_mm(task, mm_copy(mm));
    }

    if (flags & CLONE_FILES_) {
        task->files->refcount++;
    } else {
        task->files = fdtable_copy(task->files);
        if (IS_ERR(task->files)) {
            err = PTR_ERR(task->files);
            goto fail_free_mem;
        }
    }

    err = _ENOMEM;
    if (flags & CLONE_FS_) {
        task->fs->refcount++;
    } else {
        task->fs = fs_info_copy(task->fs);
        if (task->fs == NULL)
            goto fail_free_files;
    }

    if (flags & CLONE_SIGHAND_) {
        task->sighand->refcount++;
    } else {
        task->sighand = sighand_copy(task->sighand);
        if (task->sighand == NULL)
            goto fail_free_fs;
    }

    // Linux keeps sigaltstack state per thread. fork-style children inherit the
    // parent's alternate stack, but clone(CLONE_VM) children (normal pthreads,
    // including Go M threads) start with it disabled unless CLONE_VFORK is used.
    if ((flags & CLONE_VM_) && !(flags & CLONE_VFORK_)) {
        task->altstack = 0;
        task->altstack_size = 0;
    }

    struct tgroup *old_group = task->group;
    lock(&pids_lock);
    lock(&old_group->lock);
    if (!(flags & CLONE_THREAD_)) {
        task->group = tgroup_copy(old_group);
        task->group->leader = task;
        task->tgid = task->pid;
    }
    list_add(&task->group->threads, &task->group_links);
    unlock(&old_group->lock);
    unlock(&pids_lock);

    if (flags & CLONE_SETTLS_) {
#if defined(GUEST_ARM64)
        // On ARM64, the TLS argument is the actual TLS pointer value (for TPIDR_EL0),
        // not a pointer to a descriptor structure.
        task->cpu.tls_ptr = tls_addr;
#else
        err = task_set_thread_area(task, tls_addr);
        if (err < 0)
            goto fail_free_sighand;
#endif
    }

    err = _EFAULT;
    // The child sees its tid in its own PID namespace, the parent in the caller's.
    lock(&pids_lock);
    pid_t_ child_tid = task_vnr(task, task->pidns), parent_tid = task_vnr(task, current->pidns);
    unlock(&pids_lock);
    if (flags & CLONE_CHILD_SETTID_)
        if (user_put_task(task, ctid_addr, child_tid))
            goto fail_free_sighand;
    if (flags & CLONE_PARENT_SETTID_)
        if (user_put(ptid_addr, parent_tid))
            goto fail_free_sighand;
    if (flags & CLONE_CHILD_CLEARTID_)
        task->clear_tid = ctid_addr;
    task->exit_signal = flags & CSIGNAL_;
    task->pdeath_signal = 0;   // PR_SET_PDEATHSIG is not inherited

    // remember to do CLONE_SYSVSEM
    if (flags & CLONE_THREAD_)
        ipc_capture_shm_inherit_thread(task);
    return 0;

fail_free_sighand:
    sighand_release(task->sighand);
fail_free_fs:
    fs_info_release(task->fs);
fail_free_files:
    fdtable_release(task->files);
fail_free_mem:
    mm_release(task->mm);
    return err;
}

static dword_t clone_task(dword_t flags, addr_t stack, addr_t ptid, addr_t tls, addr_t ctid, addr_t pidfd_addr);

// clone(2): with CLONE_PIDFD the pidfd is returned through the parent_tid
// slot, so it cannot be combined with CLONE_PARENT_SETTID.
dword_t sys_clone(dword_t flags, addr_t stack, addr_t ptid, addr_t tls, addr_t ctid) {
    if ((flags & CLONE_PIDFD_) && (flags & CLONE_PARENT_SETTID_))
        return _EINVAL;
    return clone_task(flags, stack, ptid, tls, ctid, (flags & CLONE_PIDFD_) ? ptid : 0);
}

static dword_t clone_task(dword_t flags, addr_t stack, addr_t ptid, addr_t tls, addr_t ctid, addr_t pidfd_addr) {
    STRACE("clone(0x%x, 0x%x, 0x%x, 0x%x, 0x%x)", flags, stack, ptid, tls, ctid);
    // CLONE_PIDFD (Linux 5.2): a pidfd for the new process; not for threads or
    // detached children.
    if ((flags & CLONE_PIDFD_) && (flags & (CLONE_THREAD_ | CLONE_DETACHED_)))
        return _EINVAL;
    // Linux: a new user namespace cannot share the parent's thread group or fs.
    bool new_userns = (flags & CLONE_NEWUSER_) && userns_enabled();
    if (new_userns && (flags & (CLONE_THREAD_ | CLONE_FS_)))
        return _EINVAL;
    if (new_userns)
        flags &= ~CLONE_NEWUSER_;
    // A new PID namespace needs CAP_SYS_ADMIN in the (possibly new) user namespace.
    bool new_pidns = (flags & CLONE_NEWPID_) && userns_enabled();
    if (new_pidns && (flags & CLONE_THREAD_))
        return _EINVAL;
    if (new_pidns && !new_userns && !ns_capable(CAP_SYS_ADMIN_))
        return _EPERM;
    if (new_pidns)
        flags &= ~CLONE_NEWPID_;
    if (flags & ~CSIGNAL_ & ~IMPLEMENTED_FLAGS) {
        FIXME("unimplemented clone flags 0x%x", flags & ~CSIGNAL_ & ~IMPLEMENTED_FLAGS);
        return _EINVAL;
    }
    if (flags & CLONE_SIGHAND_ && !(flags & CLONE_VM_))
        return _EINVAL;
    if (flags & CLONE_THREAD_ && !(flags & CLONE_SIGHAND_))
        return _EINVAL;

    struct task *task = task_create_(current);
    if (task == NULL)
        return _ENOMEM;
    if (new_userns) {
        struct user_ns *ns = userns_create(current->userns, current->euid, current->egid);
        if (ns == NULL) { lock(&pids_lock); task_destroy(task); unlock(&pids_lock); return _ENOMEM; }
        userns_release(task->userns);
        task->userns = ns;
        task->cap_effective = task->cap_permitted = CAP_FULL_SET_;
        task->cap_bounding = CAP_FULL_SET_;
    }
    if (new_pidns || current->pidns) {
        lock(&pids_lock);
        struct pid_ns *ns = new_pidns ? pidns_create(current->pidns) : current->pidns;
        int attach = ns ? pidns_attach(task, ns) : _ENOSPC;
        if (new_pidns && ns) pidns_drop_creation_ref(ns);
        if (attach < 0) { task_destroy(task); unlock(&pids_lock); return attach; }
        unlock(&pids_lock);
    }
    int err = copy_task(task, flags, stack, ptid, tls, ctid);
    if (err < 0) {
        // FIXME: there is a window between task_create_ and task_destroy where
        // some other thread could get a pointer to the task.
        // FIXME: task_destroy doesn't free all aspects of the task, which
        // could cause leaks
        lock(&pids_lock);
        task_destroy(task);
        unlock(&pids_lock);
        return err;
    }
    runtime_process_event(flags & CLONE_THREAD_ ? "clone_thread" : "clone_process",task->pid,flags);
    CPU_RETVAL(task->cpu) = 0;

    struct vfork_info vfork;
    if (flags & CLONE_VFORK_) {
        lock_init(&vfork.lock);
        cond_init(&vfork.cond);
        vfork.done = false;
        task->vfork = &vfork;
    }

    // task might be destroyed by the time we finish, so save the pid (as the
    // caller's PID namespace sees it)
    lock(&pids_lock);
    pid_t pid = task_vnr(task, current->pidns);
    unlock(&pids_lock);

    // PTRACE_O_TRACEFORK/VFORK/CLONE: the new task is traced by our tracer and
    // we report the event (GETEVENTMSG: its pid as the tracer sees it).
    lock(&pids_lock);
    int ptrace_event = ptrace_attach_new_task(task, flags);
    struct task *event_tracer = ptrace_event ? ptrace_tracer(current) : NULL;
    uint64_t ptrace_event_pid = event_tracer ? (uint64_t) task_vnr(task, event_tracer->pidns) : 0;
    unlock(&pids_lock);

    // The pidfd exists before the child runs, so even an immediate exit is observable.
    int_t child_pidfd = -1;
    if (flags & CLONE_PIDFD_) {
        lock(&pids_lock);
        pid_t_ global_pid = task->pid;
        unlock(&pids_lock);
        child_pidfd = pidfd_install(global_pid);
        if (child_pidfd < 0 || user_put(pidfd_addr, child_pidfd)) {
            int err = child_pidfd < 0 ? child_pidfd : _EFAULT;
            if (child_pidfd >= 0) sys_close(child_pidfd);
            if (ptrace_event) { lock(&pids_lock); ptrace_forget_new_task(task); unlock(&pids_lock); }
            if (flags & CLONE_VFORK_) {
                task->vfork = NULL;
                cond_destroy(&vfork.cond);
            }
            task_discard_unstarted(task);
            if (flags & CLONE_VFORK_)
                pthread_mutex_destroy(&vfork.lock.m);
            return err;
        }
    }

    int start_err = task_start(task);
    if (start_err < 0) {
        if (child_pidfd >= 0) sys_close(child_pidfd);
        if (ptrace_event) { lock(&pids_lock); ptrace_forget_new_task(task); unlock(&pids_lock); }
        if (flags & CLONE_VFORK_) {
            task->vfork = NULL;
            cond_destroy(&vfork.cond);
        }
        task_discard_unstarted(task);
        if (flags & CLONE_VFORK_)
            pthread_mutex_destroy(&vfork.lock.m);
        return start_err;
    }

    if (ptrace_event)
        ptrace_stop_current(SIGTRAP_ | ptrace_event << 8, ptrace_event_pid);

    if (flags & CLONE_VFORK_) {
        lock(&vfork.lock);
        while (!vfork.done)
            // FIXME this should stop waiting if a fatal signal is received
            wait_for_ignore_signals(&vfork.cond, &vfork.lock, NULL);
        unlock(&vfork.lock);
        lock(&task->general_lock);
        task->vfork = NULL;
        unlock(&task->general_lock);
        cond_destroy(&vfork.cond);
        // PTRACE_O_TRACEVFORKDONE: report that the child released us.
        lock(&current->ptrace.lock);
        bool vfork_done_event = current->ptrace.traced && (current->ptrace.options & PTRACE_O_TRACEVFORKDONE_);
        unlock(&current->ptrace.lock);
        if (vfork_done_event)
            ptrace_stop_current(SIGTRAP_ | PTRACE_EVENT_VFORK_DONE_ << 8, ptrace_event_pid ? ptrace_event_pid : (uint64_t) pid);
    }

    return pid;
}

struct clone_args_ {
    qword_t flags;
    qword_t pidfd;
    qword_t child_tid;
    qword_t parent_tid;
    qword_t exit_signal;
    qword_t stack;
    qword_t stack_size;
    qword_t tls;
    qword_t set_tid;
    qword_t set_tid_size;
    qword_t cgroup;
};

dword_t sys_clone3(addr_t args_addr, size_t size) {
    STRACE("clone3(%#llx, %llu)", (unsigned long long)args_addr, (unsigned long long)size);

    if (size < offsetof(struct clone_args_, exit_signal) + sizeof(qword_t))
        return _EINVAL;
    if (size > sizeof(struct clone_args_))
        size = sizeof(struct clone_args_);

    struct clone_args_ args = {};
    if (user_read(args_addr, &args, size))
        return _EFAULT;

    // pidfd is an output pointer only when CLONE_PIDFD is requested. glibc
    // also fills this slot for ordinary pthread creation; Linux ignores it
    // without that flag.
    if (args.flags > UINT32_MAX) return _EINVAL;
    if (args.set_tid != 0 || args.set_tid_size != 0 || args.cgroup != 0)
        return _EINVAL;
    if (args.exit_signal & ~0xffULL)
        return _EINVAL;

    addr_t stack = (addr_t) args.stack;
    if (args.stack_size != 0)
        stack += (addr_t) args.stack_size;

    dword_t flags = (dword_t) args.flags | (dword_t) args.exit_signal;
    return clone_task(flags, stack, (addr_t) args.parent_tid, (addr_t) args.tls, (addr_t) args.child_tid,
                      (flags & CLONE_PIDFD_) ? (addr_t) args.pidfd : 0);
}

dword_t sys_fork() {
    return sys_clone(SIGCHLD_, 0, 0, 0, 0);
}

dword_t sys_vfork() {
    return sys_clone(CLONE_VFORK_ | CLONE_VM_ | SIGCHLD_, 0, 0, 0, 0);
}

void vfork_notify(struct task *task) {
    lock(&task->general_lock);
    if (task->vfork) {
        lock(&task->vfork->lock);
        task->vfork->done = true;
        notify(&task->vfork->cond);
        unlock(&task->vfork->lock);
    }
    unlock(&task->general_lock);
}

// unshare(2): only CLONE_NEWUSER (development switch ISH_USERNS=1). Linux
// requires a single-threaded caller with an unshared fs for a new user
// namespace; the caller gains the full capability set inside it.
int_t sys_unshare(dword_t flags) {
    STRACE("unshare(%#x)", flags);
    if (!userns_enabled()) return _ENOSYS;
    if (flags != CLONE_NEWUSER_) return _EINVAL;
    lock(&pids_lock);
    bool single = list_size(&current->group->threads) <= 1;
    unlock(&pids_lock);
    if (!single || current->fs->refcount > 1) return _EINVAL;
    struct user_ns *ns = userns_create(current->userns, current->euid, current->egid);
    if (!ns) return _ENOMEM;
    struct user_ns *old = current->userns;
    current->userns = ns;
    current->cap_effective = current->cap_permitted = CAP_FULL_SET_;
    current->cap_bounding = CAP_FULL_SET_;
    userns_release(old);
    return 0;
}
