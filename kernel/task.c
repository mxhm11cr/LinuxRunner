#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "kernel/calls.h"
#include "kernel/task.h"
#include "kernel/guest_profile.h"
#include "kernel/memory.h"
#include "asbestos/asbestos.h"
#include "kernel/native_offload.h"
#include "kernel/ipc.h"
#include "emu/tlb.h"
#include "platform/platform.h"
#include "fs/fd.h"

__thread struct task *current;

static struct pid pids[MAX_PID + 1] = {};
static unsigned last_allocated_pid;
unsigned task_last_pid_locked(void) { return last_allocated_pid; }
lock_t pids_lock = LOCK_INITIALIZER;

static bool pid_empty(struct pid *pid) {
    return pid->task == NULL && list_empty(&pid->session) && list_empty(&pid->pgroup);
}

struct pid *pid_get(dword_t id) {
    if (id >= sizeof(pids)/sizeof(pids[0]))
        return NULL;
    struct pid *pid = &pids[id];
    if (pid_empty(pid))
        return NULL;
    return pid;
}

struct task *pid_get_task_zombie(dword_t id) {
    struct pid *pid = pid_get(id);
    if (pid == NULL)
        return NULL;
    struct task *task = pid->task;
    return task;
}

struct task *pid_get_task(dword_t id) {
    struct task *task = pid_get_task_zombie(id);
    if (task != NULL && task->zombie)
        return NULL;
    return task;
}

// ---- Linux PID namespaces ------------------------------------------------
// Each namespace maps its own pids (allocated from 1) to the runtime's global
// pids. A task belongs to its namespace and every ancestor. The first task in a
// namespace is its init (pid 1): it inherits orphans, and its death kills the
// namespace. All state is guarded by pids_lock.
struct pid_ns {
    unsigned refcount;
    struct pid_ns *parent;
    unsigned level;
    pid_t_ next;
    pid_t_ init_pid; // global pid of the namespace's init (0 = none yet / gone)
    unsigned long long inum;
    pid_t_ global[MAX_PID + 1]; // local pid -> global pid
};
static unsigned long long next_pidns_inum = 4026532600ull;
#define INIT_PIDNS_INUM_ 4026531836ull
struct pid_ns *pidns_create(struct pid_ns *parent) {
    unsigned level = parent ? parent->level + 1 : 1;
    if (level > PIDNS_LEVELS_) return NULL;
    struct pid_ns *ns = calloc(1, sizeof(*ns));
    if (!ns) return NULL;
    ns->refcount = 1; ns->parent = parent; ns->level = level; ns->next = 1;
    ns->inum = next_pidns_inum++;
    if (parent) parent->refcount++;
    return ns;
}
static void pidns_release(struct pid_ns *ns) {
    while (ns && --ns->refcount == 0) { struct pid_ns *parent = ns->parent; free(ns); ns = parent; }
}
void pidns_drop_creation_ref(struct pid_ns *ns) { pidns_release(ns); }
unsigned long long pidns_inum(struct pid_ns *ns) { return ns ? ns->inum : INIT_PIDNS_INUM_; }
static pid_t_ pidns_alloc(struct pid_ns *ns, pid_t_ global) {
    for (unsigned tries = 0; tries < MAX_PID; tries++) {
        pid_t_ local = ns->next;
        ns->next = ns->next >= MAX_PID ? 2 : ns->next + 1; // pid 1 is only ever the init
        if (local < 1 || ns->global[local] != 0) continue;
        ns->global[local] = global;
        return local;
    }
    return 0;
}
// Give task a pid in ns and every ancestor namespace (task->pid is global).
int pidns_attach(struct task *task, struct pid_ns *ns) {
    memset(task->ns_chain, 0, sizeof(task->ns_chain));
    memset(task->ns_pid, 0, sizeof(task->ns_pid));
    task->pidns = ns;
    for (struct pid_ns *n = ns; n; n = n->parent) {
        pid_t_ local = pidns_alloc(n, task->pid);
        if (!local) {
            for (struct pid_ns *m = ns; m != n; m = m->parent) {
                m->global[task->ns_pid[m->level - 1]] = 0;
                if (m->init_pid == task->pid) m->init_pid = 0;
                m->refcount--; // the per-level reference taken below; m stays alive (caller holds ns)
            }
            task->pidns = NULL;
            return _EAGAIN;
        }
        task->ns_chain[n->level - 1] = n; task->ns_pid[n->level - 1] = local;
        n->refcount++;
        if (local == 1 && n->init_pid == 0) n->init_pid = task->pid;
    }
    return 0;
}
static void pidns_detach(struct task *task) {
    for (struct pid_ns *n = task->pidns; n; ) {
        struct pid_ns *parent = n->parent;
        pid_t_ local = task->ns_pid[n->level - 1];
        if (local && n->global[local] == task->pid) n->global[local] = 0;
        if (n->init_pid == task->pid) n->init_pid = 0;
        pidns_release(n);
        n = parent;
    }
    task->pidns = NULL;
}
pid_t_ task_vnr(struct task *task, struct pid_ns *viewer) {
    if (!task) return 0;
    if (!viewer) return task->pid;
    if (!task->pidns || task->pidns->level < viewer->level || task->ns_chain[viewer->level - 1] != viewer) return 0;
    return task->ns_pid[viewer->level - 1];
}
pid_t_ pid_view(pid_t_ global, struct pid_ns *viewer) {
    if (!viewer || global <= 0) return global;
    return task_vnr(pid_get_task_zombie(global), viewer);
}
pid_t_ tgid_view(pid_t_ global_tgid, struct pid_ns *viewer) { return pid_view(global_tgid, viewer); }
pid_t_ pid_from_view(pid_t_ local, struct pid_ns *viewer) {
    if (!viewer) return local;
    if (local <= 0 || local > MAX_PID) return 0;
    return viewer->global[local];
}
// A guest pid argument (caller's PID namespace) as a global pid. Zero/negative
// values pass through for callers' own conventions; invisible pids map to an
// out-of-range id so lookups fail with ESRCH. Entries change under pids_lock;
// a racing read only sees an older mapping of the same namespace.
pid_t_ pid_arg(pid_t_ pid) {
    if (!current->pidns || pid <= 0) return pid;
    pid_t_ global = pid_from_view(pid, current->pidns);
    return global ? global : PID_ARG_INVISIBLE_;
}
struct task *pidns_init_task(struct pid_ns *ns) { return ns && ns->init_pid ? pid_get_task(ns->init_pid) : NULL; }
bool pidns_is_init(struct task *task) { return task->pidns && task->pidns->init_pid == task->pid; }
void pidns_kill_members(struct pid_ns *ns, struct task *except) {
    for (pid_t_ local = 1; local <= MAX_PID; local++) {
        pid_t_ global = ns->global[local];
        if (!global) continue;
        struct task *task = pid_get_task(global);
        if (task && task != except && task->group != except->group)
            send_signal(task, SIGKILL_, SIGINFO_NIL);
    }
}

struct task *task_create_(struct task *parent) {
    lock(&pids_lock);
    static int cur_pid = 0;
    do {
        cur_pid++;
        if (cur_pid > MAX_PID) cur_pid = 1;
    } while (!pid_empty(&pids[cur_pid]));
    struct pid *pid = &pids[cur_pid];
    pid->id = cur_pid;
    list_init(&pid->session);
    list_init(&pid->pgroup);

    struct task *task = malloc(sizeof(struct task));
    if (task == NULL) {
        // Release pids_lock before bailing out. Returning while still holding
        // it wedged the whole kernel: every later task_create_, pid_get_task
        // and signal delivery blocks on this lock forever, so an allocation
        // failure turned into a total freeze instead of one failed fork.
        // Also roll back the pid slot claimed above, which would otherwise be
        // permanently reserved for a task that was never created.
        pid->id = 0;
        pid->task = NULL;
        unlock(&pids_lock);
        return NULL;
    }
    *task = (struct task) {};
    if (parent != NULL) {
        *task = *parent;
        userns_retain(task->userns);
        // A new task gets its own pids; sys_clone attaches namespaces.
        task->pidns = NULL;
        memset(task->ns_chain, 0, sizeof(task->ns_chain));
        memset(task->ns_pid, 0, sizeof(task->ns_pid));
    } else {
        task->cap_effective = task->cap_permitted = CAP_FULL_SET_;
        task->cap_bounding = CAP_FULL_SET_;
    }
    task->pid = pid->id;
    task->capture_shm = NULL; // explicitly retained by clone(CLONE_THREAD) only
    task->start_ticks = get_uptime().uptime_ticks * 100;
    task->diagnostic_faults = task->diagnostic_resolved_faults = 0;
    task->diagnostic_fault_ns = task->diagnostic_fault_address = 0;
    task->diagnostic_memory_stage = 0;
    pid->task = task;
    last_allocated_pid = task->pid;
    // Offload state belongs to one execution, never a copied/forked child.
    task->native_pid = 0;
    task->is_native_proxy = false;
    task->native_cancel = NULL;

#ifdef GUEST_ARM64
    // Invalidate exclusive monitor after copying parent state.
    // Child must not inherit parent's reservation, as any context switch or
    // interrupt (including fork/clone) invalidates exclusive state.
    task->cpu.excl_addr = UINT64_MAX;
    task->cpu.excl_pair_addr = UINT64_MAX;
    task->cpu.excl_pair_size = 0;
#endif

    // The poke flag must not be inherited: poked_ptr would alias the parent's
    // _poked byte (cross-task pokes), and a copied _poked=true seeds a
    // permanent one-interrupt-per-block storm in the child.
    task->cpu.poked_ptr = &task->cpu._poked;
    task->cpu._poked = false;

    // Initialize blocking state for deadlock detection.
    task->blocking = false;
    {
        struct timespec _ts;
        clock_gettime(CLOCK_MONOTONIC, &_ts);
        task->last_unblocked_ns = (uint64_t)_ts.tv_sec * 1000000000ULL + _ts.tv_nsec;
    }
    list_init(&task->children);
    list_init(&task->siblings);
    if (parent != NULL) {
        task->parent = parent;
        list_add(&parent->children, &task->siblings);
    }
    unlock(&pids_lock);

    task->pending = 0;
    list_init(&task->queue);
    task->clear_tid = 0;
    task->robust_list = 0;
    task->rseq_addr = 0;
    task->rseq_len = 0;
    task->rseq_sig = 0;
    task->rseq_registered = false;
    task->futex_pipe[0] = -1;
    task->futex_pipe[1] = -1;
    task->did_exec = false;
    lock_init(&task->general_lock);

    task->sockrestart = (struct task_sockrestart) {};
    list_init(&task->sockrestart.listen);

    task->waiting_cond = NULL;
    task->waiting_lock = NULL;
    lock_init(&task->waiting_cond_lock);
    cond_init(&task->pause);

    // Tracing is not inherited (no PTRACE_O_TRACEFORK/CLONE support), and a
    // new task never starts inside its parent's ptrace stop or single-step.
    task->ptrace = (typeof(task->ptrace)) {};
    list_init(&task->ptrace.tracer_link);
    list_init(&task->ptraced);
    list_init(&task->ptrace_exits);
#ifdef GUEST_ARM64
    task->cpu.single_step = false;
#endif
    lock_init(&task->ptrace.lock);
    cond_init(&task->ptrace.cond);
    return task;
}

// Deferred-free list for task structs.
// When a task is destroyed, its struct is not immediately freed — instead it's
// placed on this list. The NEXT call to task_destroy will free previously
// deferred structs. This gives leaked/exiting pthreads time to finish accessing
// `current` before the memory is recycled by malloc, preventing use-after-free
// heap corruption.
#define DEFERRED_FREE_MAX 64
static struct task *deferred_free_list[DEFERRED_FREE_MAX];
static int deferred_free_count = 0;
// Must be called with pids_lock held (task_destroy already requires this).
static void flush_deferred_frees(void) {
    for (int i = 0; i < deferred_free_count; i++) {
        free(deferred_free_list[i]);
        deferred_free_list[i] = NULL;
    }
    deferred_free_count = 0;
}

void task_destroy(struct task *task) {
    list_remove(&task->siblings);
    pid_get(task->pid)->task = NULL;

    // Flush old deferred frees first — they've had time to quiesce.
    flush_deferred_frees();

    userns_release(task->userns);
    task->userns = NULL;
    pidns_detach(task);
    // Zero the struct to poison stale `current` references, then defer the
    // actual free. This way if a leaked pthread is still running, it will
    // hit zeroed fields (NULL group, NULL mem) and crash cleanly rather than
    // silently corrupting a newly-allocated task at the same address.
    memset(task, 0, sizeof(struct task));

    if (deferred_free_count < DEFERRED_FREE_MAX) {
        deferred_free_list[deferred_free_count++] = task;
    } else {
        // Overflow — free immediately (rare, only with 64+ concurrent exits)
        free(task);
    }
}

static void task_run_tlb_cleanup(void *arg) {
    guest_profile_unregister((struct tlb *)arg);
    tlb_free((struct tlb *)arg);
    // [T-ish-mm-leak-refcount-handoff] If the do_exit_group safety valve
    // orphaned this thread (stuck in an uninterruptible host syscall) it
    // deferred the mm_release to here — this runs only when the host pthread
    // actually terminates, i.e. AFTER the thread has left the read_wrlock
    // critical section, so releasing now can't UAF the mem lock. This reclaims
    // the whole guest address space instead of leaking it forever. `current`
    // is a __thread TLS pointer, still valid inside pthread cleanup.
    // This must not dereference current: guest teardown may already have
    // cleared it or destroyed the task. TLS owns the running thread's lease.
    ipc_capture_shm_exit_current();
    struct task *self = current;
    if (self != NULL && self->mm_release_deferred && self->mm != NULL) {
        self->mm_release_deferred = false;
        mm_release(self->mm);
        self->mm = NULL;
        self->mem = NULL;
    }
}

// Writers of an address space poke the process's other threads so they leave
// guest code and release mem->lock (see wrlock_writer_waiting_hook). Never
// blocks: a writer may already hold pids_lock.
static void mem_writer_waiting(wrlock_t *lock) {
    struct task *self = current;
    if (self == NULL || self->mem == NULL || self->group == NULL || lock != &self->mem->lock)
        return;
    if (trylock(&pids_lock) != 0)
        return;
    struct task *task;
    list_for_each_entry(&self->group->threads, task, group_links) {
        if (task != self && task->mem == self->mem && !task->exiting)
            cpu_poke(&task->cpu);
    }
    unlock(&pids_lock);
}
__attribute__((constructor)) static void mem_writer_waiting_install(void) {
    wrlock_writer_waiting_hook = mem_writer_waiting;
}

void task_run_current() {
    ipc_capture_shm_enter_current();
    // Publish the real host owner even for PID 1 running on the calling thread.
    lock(&pids_lock); current->thread = pthread_self();
#ifdef __APPLE__
    pthread_threadid_np(NULL, &current->threadid);
#endif
    unlock(&pids_lock);
    native_offload_freeze_registry();
    struct cpu_state *cpu = &current->cpu;
    struct tlb *tlb = calloc(1, sizeof(struct tlb));
    if (!tlb) die("could not allocate TLB");

    // Register cleanup so the TLB (and its fiber_frame) is freed even when
    // the thread exits via pthread_exit() from deep in handle_interrupt()
    // (e.g. do_exit() after a native-offloaded execve, or SIGKILL path).
    // Without this, every guest process leaks ~304KB of TLB + ~48KB of
    // fiber_frame, dominating app memory after repeated ffmpeg invocations.
    pthread_cleanup_push(task_run_tlb_cleanup, tlb);
    guest_profile_register(tlb, current->tgid);

    while (true) {
        // Check for group exit before entering JIT — this catches threads
        // returning from blocking host syscalls (futex, nanosleep, etc.)
        // that were interrupted by SIGUSR1 from do_exit_group.
        // Also bail if our task struct was destroyed (current zeroed or NULLed).
        struct task *self = current;
        if (self == NULL || self->group == NULL) {
            // Task struct was destroyed under us (leaked thread).
            // Exit the host thread silently; cleanup handler frees tlb.
            pthread_exit(NULL);
        }
        if (self->group->doing_group_exit) {
            do_exit(self->group->group_exit_code);
        }
        if (self->mem == NULL) {
            pthread_exit(NULL);
        }
        read_wrlock_yield_to_writers(&self->mem->lock);
        tlb_refresh(tlb, &self->mem->mmu);
        guest_profile_state(1);
        int interrupt = cpu_run_to_interrupt(cpu, tlb);
        guest_profile_state(2);
        read_wrunlock(&self->mem->lock);
        asbestos_collect(self->mem->mmu.asbestos);
        handle_interrupt(interrupt);
    }

    // Never reached in practice (loop only exits via pthread_exit/do_exit),
    // but the pop is required for pthread_cleanup_push/pop balance.
    pthread_cleanup_pop(1);
}

static void *task_thread(void *vtask) {
    current = vtask;
    update_thread_name();
    task_run_current();
    die("task_thread returned"); // above function call should never return
}

static pthread_attr_t task_thread_attr;
__attribute__((constructor)) static void create_attr() {
    pthread_attr_init(&task_thread_attr);
    pthread_attr_setdetachstate(&task_thread_attr, PTHREAD_CREATE_DETACHED);
}

// Dispose of a fully initialised task for which task_start failed. Unlike
// task_destroy (the final PID/struct teardown), this drops all owned resources.
// The task has never run, so there are no children or per-thread futex pipes.
void task_discard_unstarted(struct task *task) {
    ipc_capture_shm_discard(task);
    lock(&pids_lock);
    lock(&task->general_lock);
    struct mm *mm = task->mm;
    struct fdtable *files = task->files;
    struct fs_info *fs = task->fs;
    task->mm = NULL;
    task->mem = NULL;
    task->files = NULL;
    task->fs = NULL;
    unlock(&task->general_lock);
    unlock(&pids_lock);
    // Resource destructors may take other locks and need a live current/group.
    mm_release(mm);
    fdtable_release(files);
    fs_info_release(fs);
    lock(&pids_lock);
    sighand_release(task->sighand);
    struct sigqueue *entry, *tmp;
    list_for_each_entry_safe(&task->queue, entry, tmp, queue) {
        list_remove(&entry->queue);
        free(entry);
    }
    struct tgroup *group = task->group;
    list_remove(&task->group_links);
    if (list_empty(&group->threads)) {
        task_leave_session(task);
        list_remove(&group->pgroup);
        cond_destroy(&group->child_exit);
        cond_destroy(&group->stopped_cond);
        pthread_mutex_destroy(&group->lock.m);
        free(group);
    }
    cond_destroy(&task->pause);
    cond_destroy(&task->ptrace.cond);
    pthread_mutex_destroy(&task->ptrace.lock.m);
    pthread_mutex_destroy(&task->waiting_cond_lock.m);
    pthread_mutex_destroy(&task->general_lock.m);
    task_destroy(task);
    unlock(&pids_lock);
}

int task_start(struct task *task) {
    native_offload_freeze_registry();
    int err = pthread_create(&task->thread, &task_thread_attr, task_thread, task);
    if (err != 0)
        return err == EAGAIN ? _EAGAIN : _ENOMEM;
    return 0;
}

int_t sys_sched_yield() {
    STRACE("sched_yield()");
    sched_yield();
    return 0;
}

void update_thread_name() {
    char name[16]; // As long as Linux will let us make this
    snprintf(name, sizeof(name), "-%d", current->pid);
    size_t pid_width = strlen(name);
    size_t name_width = snprintf(name, sizeof(name), "%s", current->comm);
    sprintf(name + (name_width < sizeof(name) - 1 - pid_width ? name_width : sizeof(name) - 1 - pid_width), "-%d", current->pid);
    platform_set_thread_name(name);
}

// Caller keeps the target task alive (pids_lock or a stopped traced child).
bool task_may_inspect(struct task *task) {
    if (task->tgid == current->tgid || current->euid == 0) return true;
    if (current->euid != task->uid || current->euid != task->euid || current->euid != task->suid ||
        current->egid != task->gid || current->egid != task->egid || current->egid != task->sgid)
        return false;
    lock(&task->general_lock);
    bool allowed = task->mm && atomic_load(&task->mm->dumpable) == 1;
    unlock(&task->general_lock);
    return allowed;
}
