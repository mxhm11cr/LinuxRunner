#ifndef KERNEL_IPC_H
#define KERNEL_IPC_H

struct mm;
struct task;
struct capture_shm_scope;
// Caller holds source->mem.lock through attachment and page-table copying.
// Parent records retain segment objects; no global IPC lock is needed here.
int ipc_shm_mm_copy(struct mm *source, struct mm *destination);
void ipc_shm_mm_release(struct mm *mm);
int ipc_capture_shm_enable(void);
void ipc_capture_shm_inherit_thread(struct task *child);
// Transfer a newly started task's lease to host TLS before running guest code.
void ipc_capture_shm_enter_current(void);
// Idempotent; also called by pthread cleanup even when current has been cleared.
// Call outside pids_lock, after this thread can no longer allocate guest IPC.
void ipc_capture_shm_exit_current(void);
// Release an unstarted task's lease outside pids_lock.
void ipc_capture_shm_discard(struct task *task);
struct ipc_shm_usage {
    unsigned long long segments, backing_fds, bytes, attachments;
    unsigned long long capture_segments, scopes, close_failures;
};
void ipc_shm_usage(struct ipc_shm_usage *usage);

#endif
