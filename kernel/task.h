#ifndef TASK_H
#define TASK_H

#include <pthread.h>
#include "emu/cpu.h"
#include "kernel/mm.h"
#include "kernel/fs.h"
#include "kernel/signal.h"
#include "kernel/resource.h"
#include "fs/sockrestart.h"
#include "util/list.h"
#include "util/timer.h"
#include "util/sync.h"

// everything here is private to the thread executing this task and needs no
// locking, unless otherwise specified
struct task {
    struct cpu_state cpu;
    struct mm *mm; // locked by general_lock
    struct mem *mem; // pointer to mm.mem, for convenience
    pthread_t thread;
    uint64_t threadid;
    uint64_t start_ticks; // USER_HZ=100 since host uptime origin; preserved by exec
    // Numeric-only, bounded diagnostics. Written by this host thread and read
    // with relaxed atomics by the explicit /proc/ish/tasks snapshot.
    uint64_t diagnostic_faults, diagnostic_resolved_faults, diagnostic_fault_ns;
    uint64_t diagnostic_fault_address;
    unsigned diagnostic_memory_stage; // 0 idle, 1 upgrade, 2 write wait, 3 materialize

    struct tgroup *group; // immutable
    struct list group_links;
    pid_t_ pid, tgid; // immutable
    uid_t_ uid, gid;
    uid_t_ euid, egid;
    uid_t_ suid, sgid;
#define MAX_GROUPS 32
    unsigned ngroups;
    uid_t_ groups[MAX_GROUPS];
    char comm[16] __strncpy_safe; // locked by general_lock
    bool did_exec; // for that one annoying setsid edge case
    // A send with MSG_NOSIGNAL is running: EPIPE is returned without SIGPIPE.
    bool send_nosignal; // only touched by the task itself

    struct fdtable *files;
    struct fs_info *fs;

    // locked by sighand->lock
    struct sighand *sighand;
    // Linux sigaltstack state is per-thread, not shared with the signal-action
    // table. Runtimes such as Go install one alternate signal stack per M/thread;
    // sharing this through sighand corrupts signal delivery between threads.
    addr_t altstack;
    dword_t altstack_size;
    sigset_t_ blocked;
    sigset_t_ pending;
    sigset_t_ waiting; // if nonzero, an ongoing call to sigtimedwait is waiting on these
    struct list queue;
    cond_t pause; // please don't signal this
    // private
    sigset_t_ saved_mask;
    bool has_saved_mask;

    // Set when thread is in a blocking syscall (futex_wait, poll_wait, etc.)
    // Used for deadlock detection: if all threads are blocking, it's a hang.
    bool blocking;
    // The syscall this thread is executing (/proc/<pid>/task/<tid>/syscall):
    // number + 1 while inside it, 0 in user mode. Written only by the thread.
    _Atomic int current_syscall;
    uint64_t current_syscall_args[6], current_syscall_pc;
    // Per-thread pipe for futex_wait wakeup (reused across calls to avoid
    // pipe creation overhead in Go runtime spin loops)
    int futex_pipe[2]; // [0]=read, [1]=write; -1 if not yet created
    // Timestamp (CLOCK_MONOTONIC ns) of last time blocking became false.
    // Used by deadlock detector to check how long ALL threads have been stuck.
    uint64_t last_unblocked_ns;

    struct {
        // Locks all ptrace-related things
        lock_t lock;
        cond_t cond;

        bool traced;
        bool stopped;
        int signal;          // nonzero: a stop not yet reported to the tracer
        int status;          // wait status bits 15..8 (signal | 0x80 | event << 8)
        struct siginfo_ info;
        int trap_event;
        int inject;          // signal the tracer resumed with (0: suppress)
        unsigned options;    // PTRACE_O_* accepted by PTRACE_SETOPTIONS
        bool syscall_trace;  // PTRACE_SYSCALL: stop at syscall entry and exit
        bool in_syscall;     // between the entry and exit syscall-stops
        bool seized;         // PTRACE_SEIZE: no implicit SIGSTOP, INTERRUPT allowed
        uint64_t event_msg;  // PTRACE_GETEVENTMSG
        struct task *tracer; // NULL: the parent traces us (pids_lock)
        struct list tracer_link; // in tracer->ptraced when tracer != parent
    } ptrace;
    // Tasks this task traces that are not its children (auto-attached
    // descendants and threads), and their pending exit reports. pids_lock.
    struct list ptraced;
    struct list ptrace_exits;

    // locked by pids_lock
    struct task *parent;
    struct list children;
    struct list siblings;

    addr_t clear_tid;
    addr_t robust_list;
    addr_t rseq_addr;
    dword_t rseq_len;
    dword_t rseq_sig;
    bool rseq_registered;

    // SA_RESTART support: on each syscall entry we snapshot the number and the
    // first arg (x0/ebx), which the return value overwrites. If the syscall is
    // interrupted (returns EINTR) and a signal with SA_RESTART is then
    // delivered, receive_signals rewinds PC to the syscall instruction and
    // restores the original first arg so the syscall re-executes — matching
    // Linux. Without it, JSC's SIGPWR-based GC safepoint handshake (which
    // registers SA_RESTART) livelocks on a futex that keeps returning EINTR.
    unsigned syscall_restart_num;
    uint64_t syscall_restart_arg0;
    uint64_t syscall_restart_pc; // cpu.pc at syscall entry (== svc_addr+4); the
                                 // SA_RESTART rewind only fires when cpu.pc still
                                 // equals this, so a stale syscall_restartable
                                 // flag can't corrupt PC on a later interrupt.
    bool syscall_restartable;
    bool syscall_restart_always; // Linux ERESTARTNOINTR, e.g. LOCK_PI/LOCK_PI2 // this syscall returned EINTR and may restart

    // locked by pids_lock
    dword_t exit_code;
    bool zombie;
    bool exiting;
    // [T-ish-mm-leak-refcount-handoff] Set by the do_exit_group safety valve
    // when it orphans a thread stuck in an uninterruptible host syscall: the
    // valve can't mm_release() now (the thread may still hold mem->lock inside
    // user_write → UAF, see T-ish-mem-uaf-user-write), so it hands the release
    // off to this thread's own pthread cleanup handler, which runs only after
    // the thread has left the read_wrlock critical section. Prevents the mm
    // (whole guest address space) from leaking forever when the thread never
    // re-enters do_exit(). Cleared once released so it never double-frees.
    bool mm_release_deferred;

    // this structure is allocated on the stack of the parent's clone() call
    struct vfork_info {
        bool done;
        cond_t cond;
        lock_t lock;
    } *vfork;
    int exit_signal;

    // lock for anything that needs locking but is not covered by some other lock
    // specifically: comm, mm
    lock_t general_lock;

    struct task_sockrestart sockrestart;

    // Native offload: when is_native_proxy is true, this task is waiting
    // for a host-native process instead of running emulated code.
    pid_t native_pid;
    bool is_native_proxy;
    // Opt-in cooperative token, published/withdrawn under sighand->lock.
    struct native_cancel *native_cancel;

    // current condition/lock, so it can be notified in case of a signal
    cond_t *waiting_cond;
    lock_t *waiting_lock;
    lock_t waiting_cond_lock;
    bool no_new_privs; // sticky Linux-visible promise; inherited by fork/clone and exec
    // PR_SET_PDEATHSIG: signal sent to this thread when its parent exits. Not
    // inherited by fork/clone children; cleared by a privilege-changing exec.
    int pdeath_signal;

    // Linux user namespace (NULL = initial namespace) and capability sets.
    // Credentials above stay global ids; guest-visible values are translated
    // through userns. Capabilities are authoritative only inside a non-initial
    // namespace; the initial namespace keeps superuser() semantics.
    struct user_ns *userns;
    uint64_t cap_effective, cap_permitted, cap_inheritable;

    // Linux PID namespace membership (NULL = initial namespace). The global
    // pid above stays the runtime's identity; ns_pid[i] is this task's pid as
    // seen in ns_chain[i] (level i+1). Guarded by pids_lock.
#define PIDNS_LEVELS_ 8
    struct pid_ns *pidns;
    struct pid_ns *ns_chain[PIDNS_LEVELS_];
    pid_t_ ns_pid[PIDNS_LEVELS_];
};

// kernel/task.c: PID namespaces. All functions require pids_lock.
struct pid_ns;
struct pid_ns *pidns_create(struct pid_ns *parent);
void pidns_drop_creation_ref(struct pid_ns *ns);
int pidns_attach(struct task *task, struct pid_ns *ns);
pid_t_ task_vnr(struct task *task, struct pid_ns *viewer);
pid_t_ pid_view(pid_t_ global, struct pid_ns *viewer);   // global pid -> viewer's pid (0 if invisible)
pid_t_ tgid_view(pid_t_ global_tgid, struct pid_ns *viewer);
pid_t_ pid_from_view(pid_t_ local, struct pid_ns *viewer); // viewer's pid -> global (0 if none)
pid_t_ pid_arg(pid_t_ pid); // syscall pid argument -> global (no lock needed)
#define PID_ARG_INVISIBLE_ 0x7ffffff0
struct task *pidns_init_task(struct pid_ns *ns);
unsigned long long pidns_inum(struct pid_ns *ns);
bool pidns_is_init(struct task *task);
void pidns_kill_members(struct pid_ns *ns, struct task *except);

// kernel/getset.c: user namespaces (default on; ISH_USERNS=0 disables).
struct user_ns;
bool userns_enabled(void);
struct user_ns *userns_create(struct user_ns *parent, uid_t_ owner_uid, uid_t_ owner_gid);
void userns_retain(struct user_ns *ns);
void userns_release(struct user_ns *ns);
uid_t_ userns_view_uid(struct user_ns *ns, uid_t_ global);
uid_t_ userns_view_gid(struct user_ns *ns, uid_t_ global);
unsigned long long userns_inum(struct user_ns *ns);
int userns_show_map(struct task *task, int kind, char *buf, size_t size); // kind 0 uid, 1 gid, 2 setgroups
int userns_write_map(struct task *task, int kind, const char *data, size_t size);
bool ns_capable(int cap);
void userns_exec_capabilities(void);
#define CAP_FULL_SET_ ((1ull << 41) - 1)
#define CAP_SETUID_ 7
#define CAP_SYS_CHROOT_ 18
#define CAP_SYS_ADMIN_ 21

bool task_may_inspect(struct task *task); // guest ptrace/proc access, not host privileges

// current will always give the process that is currently executing
// if I have to stop using __thread, current will become a macro
extern __thread struct task *current;

static inline void task_set_mm(struct task *task, struct mm *mm) {
    task->mm = mm;
    task->mem = &task->mm->mem;
    task->cpu.mmu = &task->mem->mmu;
}

// Creates a new process, initializes most fields from the parent. Specify
// parent as NULL to create the init process. Returns NULL if out of memory.
// Ends with an underscore because there's a mach function by the same name
struct task *task_create_(struct task *parent);
// Removes the process from the process table and frees it. Must be called with pids_lock.
void task_destroy(struct task *task);

// misc
void vfork_notify(struct task *task);
pid_t_ task_setsid(struct task *task);
void task_leave_session(struct task *task);

struct posix_timer {
    struct timer *timer;
    int_t timer_id;
    struct tgroup *tgroup;
    pid_t_ thread_pid;
    int_t signal;
    union sigval_ sig_value;
};

// struct thread_group is way too long to type comfortably
struct tgroup {
    struct list threads; // locked by pids_lock, by majority vote
    struct task *leader; // immutable
    struct rusage_ rusage;

    // locked by pids_lock
    pid_t_ sid, pgid;
    struct list session;
    struct list pgroup;

    bool stopped;
    cond_t stopped_cond;

    struct tty *tty;
    struct timer *itimer;
#define TIMERS_MAX 16
    struct posix_timer posix_timers[TIMERS_MAX];

    struct rlimit_ limits[RLIMIT_NLIMITS_];

    // From https://twitter.com/tblodt/status/957706819236904960
    // > there are two distinct ways for a p̶r̶o̶c̶e̶s̶s̶ thread group to exit:
    // > 
    // > - each thread calls exit
    // > wait will return the exit code for the group leader
    // > 
    // > - any thread calls exit_group
    // > the SIGNAL_GROUP_EXIT flag will be set and wait will return the status passed to exit_group
    //
    // TODO locking
    bool doing_group_exit;
    dword_t group_exit_code;

    struct rusage_ children_rusage;
    cond_t child_exit;

    dword_t personality;

    // Monotonically increasing syscall counter for deadlock detection.
    atomic_uint syscall_count;

    // Timestamp of last real progress (non-blocking syscall completion).
    // Updated atomically. Used by futex/poll safety valves to detect
    // process-wide hangs where all threads are idle.
    _Atomic uint64_t last_progress_ns;

    // /proc/<pid>/oom_score_adj (-1000..1000), inherited across fork. Recorded
    // for Linux compatibility only; iOS memory pressure is not driven by it.
    _Atomic int oom_score_adj;

    // for everything in this struct not locked by something else
    lock_t lock;
};

static inline bool task_is_leader(struct task *task) {
    return task->group->leader == task;
}

// Linux reports the parent process, never the pthread that cloned this task.
// The creator-task relationship remains intact for waiting/reaping. Hold
// pids_lock while reading this identity, as with the parent pointer itself.
static inline pid_t_ task_parent_pid(struct task *task) {
    struct task *leader = task->group->leader;
    return leader->parent ? leader->parent->tgid : 0;
}

struct pid {
    dword_t id;
    struct task *task;
    struct list session;
    struct list pgroup;
};

// synchronizes obtaining a pointer to a task and freeing that task
extern lock_t pids_lock;
// these functions must be called with pids_lock
struct pid *pid_get(dword_t pid);
struct task *pid_get_task(dword_t pid);
struct task *pid_get_task_zombie(dword_t id); // don't return null if the task exists as a zombie

#define MAX_PID (1 << 15) // oughta be enough

// TODO document
// pthread_create returns positive error codes; every caller must handle failure.
int task_start(struct task *task) __attribute__((warn_unused_result));
// Only for fully initialised tasks whose host thread never started; no locks held.
void task_discard_unstarted(struct task *task);
void task_run_current(void);

extern void (*exit_hook)(struct task *task, int code);

#define superuser() (current != NULL && current->euid == 0)

// Linux group access includes the effective and supplementary group IDs.
static inline bool task_in_group(uid_t_ gid) {
    if (current->egid == gid) return true;
    for (unsigned i = 0; i < current->ngroups; i++)
        if (current->groups[i] == gid) return true;
    return false;
}


// Update the thread name to match the current task, in the format "comm-pid".
// Will ensure that the -pid part always fits, then will fit as much of comm as possible.
void update_thread_name(void);

// Caller holds pids_lock. Last allocated guest identity, including exited tasks.
unsigned task_last_pid_locked(void);

#endif
