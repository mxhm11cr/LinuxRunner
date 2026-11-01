#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include "kernel/calls.h"
#include "fs/fd.h"
#include "fs/inode.h"
#include "fs/dmabuf.h"
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

#define FUTEX_WAIT_ 0
#define FUTEX_WAKE_ 1
#define FUTEX_REQUEUE_ 3
#define FUTEX_CMP_REQUEUE_ 4
#define FUTEX_WAKE_OP_ 5
#define FUTEX_LOCK_PI_ 6
#define FUTEX_UNLOCK_PI_ 7
#define FUTEX_TRYLOCK_PI_ 8
#define FUTEX_WAIT_BITSET_ 9
#define FUTEX_WAKE_BITSET_ 10
#define FUTEX_LOCK_PI2_ 13
#define FUTEX_PRIVATE_FLAG_ 128
#define FUTEX_CLOCK_REALTIME_ 256
#define FUTEX_CMD_MASK_ ~(FUTEX_PRIVATE_FLAG_ | FUTEX_CLOCK_REALTIME_)

// FUTEX_WAKE_OP val3 encoding (see Linux kernel futex.h)
#define FUTEX_OP_SET   0
#define FUTEX_OP_ADD   1
#define FUTEX_OP_OR    2
#define FUTEX_OP_ANDN  3
#define FUTEX_OP_XOR   4
#define FUTEX_OP_OPARG_SHIFT 8

#define FUTEX_OP_CMP_EQ 0
#define FUTEX_OP_CMP_NE 1
#define FUTEX_OP_CMP_LT 2
#define FUTEX_OP_CMP_LE 3
#define FUTEX_OP_CMP_GT 4
#define FUTEX_OP_CMP_GE 5

struct pi_key { void *object; uint64_t offset; int kind; uint64_t file_dev, file_ino; };
// PRIVATE keys use mm/address. Shared mappings use backing-object + byte offset,
// so an inherited MAP_SHARED lock is the same lock across different guest mms.
// File aliases with inode metadata use inode/file offset, not their host VA.
static int pi_key_locked(struct mem *mem, addr_t addr, bool private, int access, struct pi_key *key, uint32_t **ptr) {
    *ptr = mem_ptr(mem, addr, access);
    if (!*ptr) return _EFAULT;
    struct pt_entry *pte = mem_pt(mem, PAGE(addr));
    if (private || !(pte->flags & P_SHARED)) {
        *key = (struct pi_key){.object=mem, .offset=addr, .kind=0};
    } else if (fd_is_software_dmabuf(pte->data->fd)) {
        // udmabuf uses PFN mappings, which cannot supply a shared futex key.
        return _EFAULT;
    } else if (pte->data->fd && pte->data->fd->inode) {
        *key = (struct pi_key){.object=pte->data->fd->inode,
            .offset=pte->data->file_offset + pte->offset - pte->data->mapping_offset + PGOFFSET(addr), .kind=2};
    } else if (pte->data->fd) {
        struct stat stat;
        if (fd_is_mapping_identity(pte->data->fd)) {
            if (!fd_mapping_identity_stat(pte->data->fd, &stat.st_dev, &stat.st_ino)) return _EINVAL;
        } else if (fstat(pte->data->fd->real_fd, &stat)) return errno_map();
        *key = (struct pi_key){pte->data->fd,
            pte->data->file_offset + pte->offset - pte->data->mapping_offset + PGOFFSET(addr), 3,
            stat.st_dev, stat.st_ino};
    } else {
        *key = (struct pi_key){.object=pte->data,
            .offset=pte->offset + PGOFFSET(addr), .kind=1};
    }
    return 0;
}
static bool pi_key_equal(struct pi_key a, struct pi_key b) {
    return a.kind == b.kind && a.offset == b.offset &&
        (a.kind == 3 ? a.file_dev == b.file_dev && a.file_ino == b.file_ino : a.object == b.object);
}
static uintptr_t futex_key_hash(struct pi_key key) {
    return key.offset ^ key.kind ^ (key.kind == 3 ? key.file_dev ^ key.file_ino : (uintptr_t)key.object);
}
static void futex_key_retain(struct pi_key key) {
    if (key.kind == 1) ((struct data *)key.object)->refcount++;
    else if (key.kind == 2) inode_retain(key.object);
    else if (key.kind == 3) fd_retain(key.object);
}
static void futex_key_release(struct pi_key key) {
    if (key.kind == 1) data_release(key.object);
    else if (key.kind == 2) inode_release(key.object);
    else if (key.kind == 3) fd_close(key.object);
}
static bool pi_has_waiters(addr_t addr, bool private);
struct futex {
    atomic_uint refcount;
    struct mem *mem;
    struct mm *mm;
    struct pi_key key;
    addr_t addr;
    struct list queue;
    struct list chain; // locked by futex_hash_lock
};

struct futex_wait {
    cond_t cond;
    struct futex *futex; // will be changed by a requeue
    struct list queue;
    dword_t bitset;
    struct task *task;   // owning task
};

#define FUTEX_HASH_BITS 12
#define FUTEX_HASH_SIZE (1 << FUTEX_HASH_BITS)
static lock_t futex_lock = LOCK_INITIALIZER;
static struct list futex_hash[FUTEX_HASH_SIZE];

static void __attribute__((constructor)) init_futex_hash() {
    for (int i = 0; i < FUTEX_HASH_SIZE; i++)
        list_init(&futex_hash[i]);
}

static __thread int futex_get_error;
static struct futex *futex_get_unlocked(addr_t addr, bool private) {
    struct pi_key key; uint32_t *ptr;
    read_wrlock(&current->mem->lock);
    futex_get_error = pi_key_locked(current->mem, addr, private, MEM_READ, &key, &ptr);
    if (futex_get_error) { read_wrunlock(&current->mem->lock); return NULL; }
    int hash = futex_key_hash(key) % FUTEX_HASH_SIZE;
    struct list *bucket = &futex_hash[hash];
    struct futex *futex;
    list_for_each_entry(bucket, futex, chain) {
        if (pi_key_equal(futex->key, key)) {
            futex->refcount++;
            read_wrunlock(&current->mem->lock); return futex;
        }
    }

    futex = malloc(sizeof(struct futex));
    if (futex == NULL) { futex_get_error = _ENOMEM; read_wrunlock(&current->mem->lock); return NULL; }
    futex->refcount = 1;
    futex->mem = current->mem;
    futex->mm = current->mm; mm_retain(futex->mm);
    futex->key = key; futex_key_retain(key);
    read_wrunlock(&current->mem->lock);
    futex->addr = addr;
    list_init(&futex->queue);
    list_add(bucket, &futex->chain);
    return futex;
}

// Returns the futex for the current process at the given addr, and locks it
// Unlocked variant is available for times when you need to get two futexes at once
static struct futex *futex_get(addr_t addr, bool private) {
    lock(&futex_lock);
    struct futex *futex = futex_get_unlocked(addr, private);
    if (futex == NULL)
        unlock(&futex_lock);
    return futex;
}

static void futex_put_unlocked(struct futex *futex) {
    if (--futex->refcount == 0) {
        assert(list_empty(&futex->queue));
        list_remove(&futex->chain);
        struct mm *mm = futex->mm; futex_key_release(futex->key); free(futex); mm_release(mm);
    }
}

// Must be called on the result of futex_get when you're done with it
// Also has an unlocked version, for releasing the result of futex_get_unlocked
static void futex_put(struct futex *futex) {
    futex_put_unlocked(futex);
    unlock(&futex_lock);
}

static int futex_load(struct futex *UNUSED(futex), addr_t addr, dword_t *out) {
    read_wrlock(&current->mem->lock);
    dword_t *ptr = mem_ptr(current->mem, addr, MEM_READ);
    if (ptr) *out = __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
    read_wrunlock(&current->mem->lock);
    return ptr == NULL;
}

static int futex_wait(addr_t uaddr, dword_t val, struct timespec *timeout,
                      clockid_t clock, bool absolute, dword_t bitset, bool private) {
    struct futex *futex = futex_get(uaddr, private);
    if (!futex) return futex_get_error;
    if (pi_has_waiters(uaddr, private)) { futex_put(futex); return _EINVAL; }
    int err = 0;
    dword_t tmp;
    if (futex_load(futex, uaddr, &tmp))
        err = _EFAULT;
    else if (tmp != val)
        err = _EAGAIN;
    else {
        // Wait state is protected by futex_lock. Unlike a per-thread pipe,
        // a condition variable consumes no host file descriptors: hundreds of
        // guest threads share the iOS process's descriptor budget.
        struct futex_wait wait = {.futex = futex, .task = current, .bitset = bitset};
        cond_init(&wait.cond);
        list_add_tail(&futex->queue, &wait.queue);

        int64_t deadline_ns = 0;
        if (timeout) {
            struct timespec now;
            clock_gettime(clock, &now);
            int64_t base = absolute ? 0 : (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
            deadline_ns = timeout->tv_sec > (INT64_MAX - base - timeout->tv_nsec) / 1000000000LL
                ? INT64_MAX : base + (int64_t)timeout->tv_sec * 1000000000LL + timeout->tv_nsec;
        }
        current->blocking = true;
        while (!list_null(&wait.queue)) {
            // Bounded waits retain signal/group-exit and mapping invalidation
            // checks, including signals racing with wait_for's publication.
            struct timespec interval = {.tv_nsec = 100000000};
            if (timeout) {
                struct timespec now;
                clock_gettime(clock, &now);
                int64_t remain_ns = deadline_ns - ((int64_t)now.tv_sec * 1000000000LL + now.tv_nsec);
                if (remain_ns <= 0) { err = _ETIMEDOUT; break; }
                if (remain_ns < interval.tv_nsec) interval.tv_nsec = remain_ns;
            }
            if (current->group->doing_group_exit) { err = _EINTR; break; }
            int result = wait_for(&wait.cond, &futex_lock, &interval);
            // Only FUTEX_WAKE/REQUEUE wake operations remove the queue entry;
            // unrelated condvar notifications must not consume a bitset wake.
            if (list_null(&wait.queue)) break;
            if (result == _EINTR) { err = _EINTR; break; }
            if (futex_load(wait.futex, uaddr, &tmp)) { err = _EFAULT; break; }
        }
        current->blocking = false;
        list_remove_safe(&wait.queue);
        futex_put_unlocked(wait.futex);
        cond_destroy(&wait.cond);
        unlock(&futex_lock);
        goto futex_wait_done;
    }
    futex_put(futex);
futex_wait_done:
    STRACE("%d end futex(FUTEX_WAIT)", current->pid);
    return err;
}

// Wake up to `max` waiters on the given futex. Caller must hold futex_lock.
static unsigned futex_wake_queue(struct futex *futex, dword_t max, dword_t bitset) {
    struct futex_wait *wait, *tmp;
    unsigned woken = 0;
    list_for_each_entry_safe(&futex->queue, wait, tmp, queue) {
        if (woken >= max)
            break;
        if (!(wait->bitset & bitset))
            continue;
        list_remove(&wait->queue);
        notify(&wait->cond);
        woken++;
    }
    return woken;
}

static int futex_wakelike(int op, addr_t uaddr, dword_t wake_max, dword_t requeue_max, addr_t requeue_addr, dword_t bitset, bool private) {
    struct futex *futex = futex_get(uaddr, private);
    if (!futex) return futex_get_error;
    if (pi_has_waiters(uaddr, private) || (op == FUTEX_REQUEUE_ && pi_has_waiters(requeue_addr, private))) { futex_put(futex); return _EINVAL; }

    unsigned woken = futex_wake_queue(futex, wake_max, bitset);

    if (op == FUTEX_REQUEUE_) {
        struct futex *futex2 = futex_get_unlocked(requeue_addr, private);
        if (!futex2) { futex_put(futex); return futex_get_error; }
        struct futex_wait *wait, *tmp;
        unsigned requeued = 0;
        list_for_each_entry_safe(&futex->queue, wait, tmp, queue) {
            if (requeued >= requeue_max)
                break;
            // sketchy as hell
            list_remove(&wait->queue);
            list_add_tail(&futex2->queue, &wait->queue);
            assert(futex->refcount > 1); // should be true because this function keeps a reference
            futex->refcount--;
            futex2->refcount++;
            wait->futex = futex2;
            requeued++;
        }
        futex_put_unlocked(futex2);
        woken += requeued;
    }

    futex_put(futex);
    return woken;
}

int futex_wake(addr_t uaddr, dword_t wake_max) {
    return futex_wakelike(FUTEX_WAKE_, uaddr, wake_max, 0, 0, UINT32_MAX, false);
}

// FUTEX_CMP_REQUEUE: like FUTEX_REQUEUE, but first atomically check *uaddr == expected.
// Returns total woken+requeued, or _EAGAIN on mismatch, or _EFAULT on bad addr.
static int futex_cmp_requeue(addr_t uaddr, dword_t wake_max, dword_t requeue_max,
                             addr_t requeue_addr, dword_t expected, bool private) {
    struct futex *futex = futex_get(uaddr, private);
    if (futex == NULL)
        return futex_get_error;
    if (pi_has_waiters(uaddr, private) || pi_has_waiters(requeue_addr, private)) { futex_put(futex); return _EINVAL; }

    dword_t cur;
    if (futex_load(futex, uaddr, &cur)) {
        futex_put(futex);
        return _EFAULT;
    }
    if (cur != expected) {
        futex_put(futex);
        return _EAGAIN;
    }

    unsigned woken = futex_wake_queue(futex, wake_max, UINT32_MAX);

    struct futex *futex2 = futex_get_unlocked(requeue_addr, private);
    if (!futex2) { futex_put(futex); return futex_get_error; }
    struct futex_wait *wait, *tmp;
    unsigned requeued = 0;
    list_for_each_entry_safe(&futex->queue, wait, tmp, queue) {
        if (requeued >= requeue_max)
            break;
        list_remove(&wait->queue);
        list_add_tail(&futex2->queue, &wait->queue);
        assert(futex->refcount > 1);
        futex->refcount--;
        futex2->refcount++;
        wait->futex = futex2;
        requeued++;
    }
    futex_put_unlocked(futex2);

    futex_put(futex);
    return woken + requeued;
}

// FUTEX_WAKE_OP: atomic RMW on *uaddr2, then wake up to val waiters on uaddr,
// and — if the old value of *uaddr2 satisfies the comparison — wake up to val2
// waiters on uaddr2. val3 encodes the op, oparg, cmp, and cmparg (see Linux).
static int futex_wake_op(addr_t uaddr, dword_t wake_max1, addr_t uaddr2,
                         dword_t wake_max2, dword_t val3, bool private) {
    int op_code = (val3 >> 28) & 0xf;
    int cmp_code = (val3 >> 24) & 0xf;
    // oparg/cmparg are 12-bit sign-extended.
    int32_t oparg  = (int32_t)((val3 << 8)) >> 20;  // bits [23:12] sign-extended
    int32_t cmparg = (int32_t)((val3 << 20)) >> 20; // bits [11:0] sign-extended

    bool shift = (op_code & FUTEX_OP_OPARG_SHIFT) != 0;
    op_code &= ~FUTEX_OP_OPARG_SHIFT;
    if (shift) {
        if (oparg < 0 || oparg > 31)
            return _EINVAL;
        oparg = 1 << oparg;
    }

    if (op_code > FUTEX_OP_XOR || cmp_code > FUTEX_OP_CMP_GE) return _ENOSYS;
    lock(&futex_lock);
    if (pi_has_waiters(uaddr, private) || pi_has_waiters(uaddr2, private)) { unlock(&futex_lock); return _EINVAL; }
    // Atomic RMW on *uaddr2.
    // We must hold mem->lock across mem_ptr+atomic op so the mapping can't vanish.
    read_wrlock(&current->mem->lock);
    uint32_t *ptr = mem_ptr(current->mem, uaddr2, MEM_WRITE);
    if (ptr == NULL) {
        read_wrunlock(&current->mem->lock);
        unlock(&futex_lock); return _EFAULT;
    }
    uint32_t oldval;
    switch (op_code) {
        case FUTEX_OP_SET:
            oldval = __atomic_exchange_n(ptr, (uint32_t)oparg, __ATOMIC_SEQ_CST);
            break;
        case FUTEX_OP_ADD:
            oldval = __atomic_fetch_add(ptr, (uint32_t)oparg, __ATOMIC_SEQ_CST);
            break;
        case FUTEX_OP_OR:
            oldval = __atomic_fetch_or(ptr, (uint32_t)oparg, __ATOMIC_SEQ_CST);
            break;
        case FUTEX_OP_ANDN:
            oldval = __atomic_fetch_and(ptr, (uint32_t)~oparg, __ATOMIC_SEQ_CST);
            break;
        case FUTEX_OP_XOR:
            oldval = __atomic_fetch_xor(ptr, (uint32_t)oparg, __ATOMIC_SEQ_CST);
            break;
        default:
            read_wrunlock(&current->mem->lock);
            unlock(&futex_lock); return _ENOSYS;
    }
    read_wrunlock(&current->mem->lock);

    int32_t sold = (int32_t)oldval;
    bool cmp_true;
    switch (cmp_code) {
        case FUTEX_OP_CMP_EQ: cmp_true = (sold == cmparg); break;
        case FUTEX_OP_CMP_NE: cmp_true = (sold != cmparg); break;
        case FUTEX_OP_CMP_LT: cmp_true = (sold <  cmparg); break;
        case FUTEX_OP_CMP_LE: cmp_true = (sold <= cmparg); break;
        case FUTEX_OP_CMP_GT: cmp_true = (sold >  cmparg); break;
        case FUTEX_OP_CMP_GE: cmp_true = (sold >= cmparg); break;
        default: unlock(&futex_lock); return _ENOSYS;
    }

    // Wake waiters on both queues under the metadata lock.
    struct futex *f1 = futex_get_unlocked(uaddr, private);
    if (f1 == NULL) { unlock(&futex_lock); return futex_get_error; }
    unsigned total = futex_wake_queue(f1, wake_max1, UINT32_MAX);
    if (cmp_true) {
        struct futex *f2 = futex_get_unlocked(uaddr2, private);
        if (f2 != NULL) {
            total += futex_wake_queue(f2, wake_max2, UINT32_MAX);
            futex_put_unlocked(f2);
        }
    }
    futex_put_unlocked(f1);
    unlock(&futex_lock);
    return total;
}

// The PI graph shares the short-lived futex queue metadata lock. No guest execution
// or blocking host wait holds it. Per-waiter conditions avoid polling for handoff.
// Lock order: pi_lock -> mem.lock / pids_lock. Exit drops pids_lock first.
#define PI_TID_MASK 0x3fffffffu
#define PI_OWNER_DIED 0x40000000u
#define PI_WAITERS 0x80000000u
#define PI_GRAPH_LIMIT 4096
struct pi_waiter {
    struct list link;
    struct task *task;
    addr_t addr;
    cond_t cond;
    bool granted;
    int result;
#ifdef __APPLE__
    qos_class_t qos;
    int relative;
#endif
};
struct pi_state {
    struct list link, queue;
    struct pi_key key;
    pid_t_ owner;
    unsigned users;
    struct mm *mm; // keeps the key's backing object alive until the last waiter leaves
#ifdef __APPLE__
    qos_class_t qos, applied_qos;
    int relative, applied_relative;
    pthread_override_t donation;
    pid_t_ donated_owner;
#endif
};
// Share the existing queue metadata lock so mixed PI/non-PI waits are rejected atomically.
#define pi_lock futex_lock
static struct list pi_states = LIST_INITIALIZER(pi_states);
static unsigned pi_state_count;
static uint64_t pi_contentions, pi_handoffs, pi_timeouts, pi_interrupts, pi_deadlocks, pi_donation_starts, pi_donation_failures;
static atomic_uint pi_trace_count;
atomic_bool runtime_futex_pi_trace_enabled;

static struct pi_state *pi_find(struct pi_key key) {
    struct pi_state *s;
    list_for_each_entry(&pi_states, s, link) if (pi_key_equal(s->key, key)) return s;
    return NULL;
}
static bool pi_has_waiters(addr_t addr, bool private) {
    struct pi_key key; uint32_t *ptr;
    read_wrlock(&current->mem->lock);
    int err = pi_key_locked(current->mem, addr, private, MEM_READ, &key, &ptr);
    read_wrunlock(&current->mem->lock);
    if (err) return false;
    struct pi_state *s = pi_find(key);
    return s && !list_empty(&s->queue);
}
static void pi_trace(addr_t addr, int cmd, uint32_t before, uint32_t after, int result,
                     bool private, clockid_t clock, const struct timespec *deadline, uint64_t started) {
    const char *trace = getenv("ISH_FUTEX_PI_TRACE");
    bool verbose = atomic_load(&runtime_futex_pi_trace_enabled) || trace;
    if (!verbose && result != _ESRCH && result != _EINVAL) return;
    // By default trace only slow paths/errors. Explicit "all" includes fast paths.
    if (verbose && (!trace || strcmp(trace, "all")) && result == 0 && !((before | after) & PI_WAITERS)) return;
    unsigned n = atomic_fetch_add(&pi_trace_count, 1);
    if (n >= 512) return;
    uint64_t pc, lr = 0;
#ifdef GUEST_ARM64
    pc = current->cpu.pc; lr = current->cpu.regs[30];
#else
    pc = current->cpu.eip;
#endif
    struct timespec now; uint64_t elapsed_us = 0;
    if (started) { clock_gettime(CLOCK_MONOTONIC, &now); elapsed_us =
        ((uint64_t)now.tv_sec * 1000000000 + now.tv_nsec - started) / 1000; }
    printk("FUTEX_PI: pid=%d tid=%d comm=%.16s cmd=%d addr=%#llx before=%#x after=%#x owner=%u result=%d pc=%#llx host_thread=%p host_tid=%llu lr=%#llx private=%d clock=%s absolute_deadline=%lld.%09ld duration_us=%llu event=%u\n",
        current->tgid, current->pid, current->comm, cmd, (unsigned long long)addr,
        before, after, after & PI_TID_MASK, result, (unsigned long long)pc,
        (void *)pthread_self(), (unsigned long long)current->threadid,
        (unsigned long long)lr, private, clock == CLOCK_REALTIME ? "realtime" : "monotonic",
        deadline ? (long long)deadline->tv_sec : -1LL, deadline ? deadline->tv_nsec : 0L,
        (unsigned long long)elapsed_us, n);
}
static struct pi_state *pi_waiting_on(pid_t_ tid) {
    struct pi_state *s; struct pi_waiter *w;
    list_for_each_entry(&pi_states, s, link)
        list_for_each_entry(&s->queue, w, link) if (w->task->pid == tid) return s;
    return NULL;
}
static bool pi_cycle(pid_t_ owner, pid_t_ waiter) {
    for (unsigned depth = 0; owner && depth <= pi_state_count; depth++) {
        if (owner == waiter) return true;
        struct pi_state *s = pi_waiting_on(owner);
        if (!s) return false;
        owner = s->owner;
    }
    return owner != 0;
}
#ifdef __APPLE__
static bool pi_qos_higher(qos_class_t a, int ar, qos_class_t b, int br) {
    return a > b || (a == b && ar > br);
}
// Explicit public Darwin QoS overrides support proxy owners: the waiter need
// not have acquired a host mutex. Propagate dependencies through the guest
// graph ourselves; Darwin does not know these guest edges. This is QoS donation,
// NOT Linux realtime scheduling or a claim of rt-mutex latency guarantees.
static void pi_update_donations(void) {
    struct pi_state *s, *held; struct pi_waiter *w;
    list_for_each_entry(&pi_states, s, link) { s->qos = QOS_CLASS_UNSPECIFIED; s->relative = 0; }
    for (unsigned pass = 0; pass <= pi_state_count; pass++) {
        bool changed = false;
        list_for_each_entry(&pi_states, s, link) {
            list_for_each_entry(&s->queue, w, link) {
                qos_class_t qos = w->qos; int relative = w->relative;
                list_for_each_entry(&pi_states, held, link) {
                    if (held->owner == w->task->pid && pi_qos_higher(held->qos, held->relative, qos, relative)) {
                        qos = held->qos; relative = held->relative;
                    }
                }
                if (pi_qos_higher(qos, relative, s->qos, s->relative)) {
                    s->qos = qos; s->relative = relative; changed = true;
                }
            }
        }
        if (!changed) break;
    }
    list_for_each_entry(&pi_states, s, link) {
        if (s->donation && s->donated_owner == s->owner &&
            s->qos == s->applied_qos && s->relative == s->applied_relative) continue;
        pthread_override_t next = NULL;
        if (s->owner && s->qos != QOS_CLASS_UNSPECIFIED) {
            lock(&pids_lock);
            struct task *owner = pid_get_task(s->owner);
            if (owner && !owner->exiting)
                next = pthread_override_qos_class_start_np(owner->thread, s->qos, s->relative);
            unlock(&pids_lock);
            if (next) pi_donation_starts++;
            else pi_donation_failures++;
            if (!next && atomic_fetch_add(&pi_trace_count, 1) < 512)
                printk("FUTEX_PI_DONATION: owner=%d qos=%u relative=%d failed errno=%d; ownership/wait semantics retained\n", s->owner, s->qos, s->relative, errno);
        }
        // Start replacement before ending the old donation to avoid a dip.
        if (s->donation) pthread_override_qos_class_end_np(s->donation);
        s->donation = next; s->donated_owner = s->owner;
        s->applied_qos = s->qos; s->applied_relative = s->relative;
    }
}
#else
static void pi_update_donations(void) {}
#endif
static void pi_put(struct pi_state *s) {
    if (--s->users) return;
    assert(list_empty(&s->queue));
#ifdef __APPLE__
    if (s->donation) pthread_override_qos_class_end_np(s->donation);
#endif
    list_remove(&s->link); pi_state_count--;
    struct mm *mm = s->mm; futex_key_release(s->key); free(s);
    // Caller must not hold this mm's memory lock.
    mm_release(mm);
}
static bool pi_signal_pending(void) {
    if (current->group->doing_group_exit) return true;
    lock(&current->sighand->lock);
    bool pending = !!(current->pending & ~current->blocked);
    unlock(&current->sighand->lock); return pending;
}
// Store through a live waiter's mapping. The exiting owner may have unmapped
// its own alias; the retained mm only prevents a dangling backing-object key.
static int pi_handoff(struct pi_state *s, bool owner_died) {
    struct pi_waiter *w = list_first_entry(&s->queue, struct pi_waiter, link);
    read_wrlock(&w->task->mem->lock);
    struct pi_key key; uint32_t *ptr;
    int err = pi_key_locked(w->task->mem, w->addr, s->key.kind == 0, MEM_WRITE, &key, &ptr);
    if (!err && !pi_key_equal(key, s->key)) err = _EFAULT;
    if (!err) {
        uint32_t word = __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
        if ((word & PI_TID_MASK) != s->owner) err = _EINVAL;
        else {
            uint32_t next = w->task->pid | PI_WAITERS | (owner_died ? PI_OWNER_DIED : 0);
            if (!__atomic_compare_exchange_n(ptr, &word, next, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) err = _EINVAL;
        }
    }
    read_wrunlock(&w->task->mem->lock);
    if (!err) { s->owner = w->task->pid; list_remove(&w->link); w->granted = true; pi_handoffs++; notify(&w->cond); }
    return err;
}
static int futex_pi(addr_t addr, int cmd, bool private, struct timespec *deadline, clockid_t clock) {
    uint64_t started = 0;
    if (atomic_load(&runtime_futex_pi_trace_enabled) || getenv("ISH_FUTEX_PI_TRACE")) {
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        started = (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
    }
retry:
    lock(&pi_lock);
    read_wrlock(&current->mem->lock);
    struct pi_key key; uint32_t *ptr;
    int result = pi_key_locked(current->mem, addr, private, MEM_WRITE, &key, &ptr);
    uint32_t before = 0, word = 0;
    struct pi_state *s = NULL;
    if (result) goto memory_done;
    word = before = __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
    if (cmd == FUTEX_UNLOCK_PI_ && (word & PI_TID_MASK) != current->pid) { result = _EPERM; goto memory_done; }
    if (cmd != FUTEX_UNLOCK_PI_ && (word & PI_TID_MASK) == current->pid) { result = _EDEADLK; goto memory_done; }
    struct futex *ordinary;
    unsigned hash = futex_key_hash(key) % FUTEX_HASH_SIZE;
    list_for_each_entry(&futex_hash[hash], ordinary, chain) {
        if (pi_key_equal(ordinary->key, key) && !list_empty(&ordinary->queue)) { result = _EINVAL; goto memory_done; }
    }
    s = pi_find(key);
    if (s) s->users++;
    if (cmd == FUTEX_UNLOCK_PI_) {
        if ((word & PI_TID_MASK) != current->pid) { result = _EPERM; goto memory_done; }
        if (s && !list_empty(&s->queue)) {
            if (s->owner != current->pid || !(word & PI_WAITERS)) { result = _EINVAL; goto memory_done; }
            read_wrunlock(&current->mem->lock);
            result = pi_handoff(s, false); pi_update_donations();
            goto done;
        }
        if (!__atomic_compare_exchange_n(ptr, &word, 0, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) result = _EAGAIN;
        goto memory_done;
    }
    for (;;) {
        pid_t_ owner = word & PI_TID_MASK;
        if (owner == current->pid) { result = _EDEADLK; goto memory_done; }
        if (!owner && (!s || list_empty(&s->queue))) {
            uint32_t next = current->pid | (word & PI_OWNER_DIED);
            if (!__atomic_compare_exchange_n(ptr, &word, next, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) continue;
            if (s) s->owner = current->pid;
            word = next; goto memory_done;
        }
        if (s && s->owner != owner) { result = _EINVAL; goto memory_done; }
        // Force the fast-path owner into the syscall slow path before owner
        // lookup, including TRYLOCK and ESRCH failures, as Linux does.
        uint32_t marked = word | PI_WAITERS;
        if (!__atomic_compare_exchange_n(ptr, &word, marked, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) continue;
        word = marked;
        lock(&pids_lock);
        struct task *owner_task = pid_get_task(owner);
        bool exists = owner_task != NULL, exiting = exists && owner_task->exiting;
        unlock(&pids_lock);
        if (!exists) {
            uint32_t observed = __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
            if (observed != word) { word = observed; continue; }
            result = _ESRCH; goto memory_done;
        }
        if (exiting) {
            // Linux retries an owner-exit race internally; do not leak EAGAIN
            // to a pthread implementation that expects the lock operation to finish.
            read_wrunlock(&current->mem->lock);
            if (s) pi_put(s);
            unlock(&pi_lock);
            if (pi_signal_pending()) { current->syscall_restart_always = true; return _EINTR; }
            struct timespec retry_delay = {.tv_nsec = 1000000}; nanosleep(&retry_delay, NULL);
            goto retry;
        }
        if (pi_cycle(owner, current->pid)) { result = _EDEADLK; goto memory_done; }
        if (cmd == FUTEX_TRYLOCK_PI_) {
            uint32_t next = word | PI_WAITERS;
            if (!__atomic_compare_exchange_n(ptr, &word, next, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) continue;
            result = _EAGAIN; goto memory_done;
        }
        if (!s) {
            if (pi_state_count >= PI_GRAPH_LIMIT) { result = _ENOMEM; goto memory_done; }
            s = calloc(1, sizeof(*s));
            if (!s) { result = _ENOMEM; goto memory_done; }
            s->key = key; s->owner = owner; s->users = 1;
            s->mm = current->mm; mm_retain(s->mm); futex_key_retain(key);
            list_init(&s->queue); list_add_tail(&pi_states, &s->link); pi_state_count++;
        }
        uint32_t next = word | PI_WAITERS;
        if (!__atomic_compare_exchange_n(ptr, &word, next, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            if (list_empty(&s->queue)) s->owner = word & PI_TID_MASK;
            continue;
        }
        word = next; break;
    }
    read_wrunlock(&current->mem->lock);
    struct pi_waiter wait = {.task = current, .addr = addr};
    cond_init(&wait.cond);
#ifdef __APPLE__
    pthread_get_qos_class_np(pthread_self(), &wait.qos, &wait.relative);
    if (wait.qos == QOS_CLASS_UNSPECIFIED) { wait.qos = QOS_CLASS_DEFAULT; wait.relative = 0; }
#endif
    list_add_tail(&s->queue, &wait.link); pi_contentions++;
    pi_trace(addr, cmd, before, word, 1, private, clock, deadline, started); pi_update_donations();
    current->blocking = true;
    while (!wait.granted && !wait.result) {
        if (pi_signal_pending()) { current->syscall_restart_always = true; result = _EINTR; break; }
        struct timespec slice = {.tv_nsec = 100000000};
        if (deadline) {
            struct timespec now; clock_gettime(clock, &now);
            if (now.tv_sec > deadline->tv_sec || (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec)) { result = _ETIMEDOUT; break; }
            if (deadline->tv_sec - now.tv_sec <= 1) {
                int64_t ns = (deadline->tv_sec - now.tv_sec) * 1000000000LL + deadline->tv_nsec - now.tv_nsec;
                if (ns < slice.tv_nsec) slice.tv_nsec = ns;
            }
        }
        int err = wait_for_ignore_signals(&wait.cond, &pi_lock, &slice);
        (void)err; // spurious wakeups and clock changes recheck the predicate
    }
    current->blocking = false;
    if (wait.granted) result = 0; // handoff wins a simultaneous timeout/signal
    else { list_remove(&wait.link); if (wait.result) result = wait.result; }
    // WAITERS may remain set when the last waiter times out, like Linux. The
    // owner can always call UNLOCK_PI to clear a stale WAITERS bit.
    cond_destroy(&wait.cond); pi_update_donations();
    goto done;
memory_done:
    read_wrunlock(&current->mem->lock);
done:
    if (result == _ETIMEDOUT) pi_timeouts++;
    if (result == _EINTR) pi_interrupts++;
    if (result == _EDEADLK) pi_deadlocks++;
    if (s) pi_put(s);
    unlock(&pi_lock);
    uint32_t after = 0; if (user_get(addr, after)) after = 0;
    pi_trace(addr, cmd, before, after, result, private, clock, deadline, started);
    return result;
}
// Invoked before mm teardown and clear_child_tid. Robust lists additionally
// cover fast-path owners for which no kernel PI state has ever been allocated.
void futex_pi_exit(void) {
    lock(&pi_lock);
    struct pi_state *s;
    list_for_each_entry(&pi_states, s, link) {
        if (s->owner != current->pid || list_empty(&s->queue)) continue;
        int err = pi_handoff(s, true);
        if (err) {
            struct pi_waiter *w, *tmp;
            list_for_each_entry_safe(&s->queue, w, tmp, link) { w->result = err; notify(&w->cond); }
            s->owner = 0;
        }
    }
    pi_update_donations(); unlock(&pi_lock);
}

int futex_pi_show(struct proc_data *buf) {
    lock(&pi_lock);
    unsigned waiters = 0; struct pi_state *s; struct pi_waiter *w;
    list_for_each_entry(&pi_states, s, link) waiters += list_size(&s->queue);
    proc_printf(buf, "PI_STATUS: states=%u waiters=%u contentions=%llu handoffs=%llu timeouts=%llu interrupted_restarts=%llu deadlocks=%llu donation_starts=%llu donation_failures=%llu realtime_guarantees=not-implemented\n",
        pi_state_count, waiters, (unsigned long long)pi_contentions, (unsigned long long)pi_handoffs,
        (unsigned long long)pi_timeouts, (unsigned long long)pi_interrupts, (unsigned long long)pi_deadlocks,
        (unsigned long long)pi_donation_starts, (unsigned long long)pi_donation_failures);
    unsigned rows = 0;
    list_for_each_entry(&pi_states, s, link) {
        list_for_each_entry(&s->queue, w, link) {
            if (rows++ >= 128) break;
            proc_printf(buf, "PI_WAIT: owner=%d tid=%d pid=%d addr=%#llx key_kind=%d",
                s->owner, w->task->pid, w->task->tgid, (unsigned long long)w->addr, s->key.kind);
#ifdef __APPLE__
            proc_printf(buf, " donated_qos=%u relative=%d override_active=%d", s->qos, s->relative, s->donation != NULL);
#endif
            proc_printf(buf, "\n");
        }
    }
    unlock(&pi_lock); return 0;
}

dword_t sys_futex(addr_t uaddr, dword_t op, dword_t val, addr_t timeout_or_val2, addr_t uaddr2, dword_t val3) {
    if (!(op & FUTEX_PRIVATE_FLAG_)) {
        STRACE("!FUTEX_PRIVATE ");
    }
    int cmd = op & FUTEX_CMD_MASK_;
    if ((op & FUTEX_CLOCK_REALTIME_) && cmd != FUTEX_WAIT_ && cmd != FUTEX_WAIT_BITSET_ && cmd != FUTEX_LOCK_PI2_)
        return _ENOSYS;
    struct timespec timeout = {0};
    if ((cmd == FUTEX_WAIT_ || cmd == FUTEX_WAIT_BITSET_ || cmd == FUTEX_LOCK_PI_ || cmd == FUTEX_LOCK_PI2_) && timeout_or_val2) {
        struct timespec_ timeout_;
        if (user_get(timeout_or_val2, timeout_))
            return _EFAULT;
        if (timeout_.sec < 0 || timeout_.nsec < 0 || timeout_.nsec >= 1000000000)
            return _EINVAL;
        timeout.tv_sec = timeout_.sec;
        timeout.tv_nsec = timeout_.nsec;
    }
    if ((cmd == FUTEX_WAIT_BITSET_ || cmd == FUTEX_WAKE_BITSET_) && !val3)
        return _EINVAL;
    if (uaddr & 3) return _EINVAL;
    clockid_t clock = op & FUTEX_CLOCK_REALTIME_ ? CLOCK_REALTIME : CLOCK_MONOTONIC;

    switch (cmd) {
        case FUTEX_LOCK_PI_:
        case FUTEX_LOCK_PI2_:
        case FUTEX_UNLOCK_PI_:
        case FUTEX_TRYLOCK_PI_:
            return futex_pi(uaddr, cmd, op & FUTEX_PRIVATE_FLAG_,
                (cmd == FUTEX_LOCK_PI_ || cmd == FUTEX_LOCK_PI2_) && timeout_or_val2 ? &timeout : NULL,
                cmd == FUTEX_LOCK_PI_ ? CLOCK_REALTIME : clock);
        case FUTEX_WAIT_:
            STRACE("futex(FUTEX_WAIT, %#x, %d, 0x%x {%ds %dns}) = ...\n", uaddr, val, timeout_or_val2, timeout.tv_sec, timeout.tv_nsec);
            return futex_wait(uaddr, val, timeout_or_val2 ? &timeout : NULL,
                              clock, false, UINT32_MAX, op & FUTEX_PRIVATE_FLAG_);
        case FUTEX_WAIT_BITSET_:
            // Absolute deadline uses MONOTONIC unless CLOCK_REALTIME is requested.
            STRACE("futex(FUTEX_WAIT_BITSET, %#x, %d, mask=%#x) = ...\n", uaddr, val, val3);
            return futex_wait(uaddr, val, timeout_or_val2 ? &timeout : NULL,
                              clock, true, val3, op & FUTEX_PRIVATE_FLAG_);
        case FUTEX_WAKE_:
            STRACE("futex(FUTEX_WAKE, %#x, %d)", uaddr, val);
            return futex_wakelike(FUTEX_WAKE_, uaddr, val, 0, 0, UINT32_MAX, op & FUTEX_PRIVATE_FLAG_);
        case FUTEX_WAKE_BITSET_:
            // FUTEX_WAKE_BITSET with MATCH_ANY is equivalent to FUTEX_WAKE.
            STRACE("futex(FUTEX_WAKE_BITSET, %#x, %d, mask=%#x)", uaddr, val, val3);
            return futex_wakelike(FUTEX_WAKE_, uaddr, val, 0, 0, val3, op & FUTEX_PRIVATE_FLAG_);
        case FUTEX_REQUEUE_:
            STRACE("futex(FUTEX_REQUEUE, %#x, %d, %#x)", uaddr, val, uaddr2);
            return futex_wakelike(FUTEX_REQUEUE_, uaddr, val, timeout_or_val2, uaddr2, UINT32_MAX, op & FUTEX_PRIVATE_FLAG_);
        case FUTEX_CMP_REQUEUE_:
            STRACE("futex(FUTEX_CMP_REQUEUE, %#x, %d, %#x, expected=%d)", uaddr, val, uaddr2, val3, op & FUTEX_PRIVATE_FLAG_);
            return futex_cmp_requeue(uaddr, val, timeout_or_val2, uaddr2, val3, op & FUTEX_PRIVATE_FLAG_);
        case FUTEX_WAKE_OP_:
            STRACE("futex(FUTEX_WAKE_OP, %#x, %d, %#x, %d, %#x)", uaddr, val, uaddr2, timeout_or_val2, val3, op & FUTEX_PRIVATE_FLAG_);
            return futex_wake_op(uaddr, val, uaddr2, timeout_or_val2, val3, op & FUTEX_PRIVATE_FLAG_);
    }
    STRACE("futex(%#x, %d, %d, timeout=%#x, %#x, %d) ", uaddr, op, val, timeout_or_val2, uaddr2, val3, op & FUTEX_PRIVATE_FLAG_);
    // Linux reports ENOSYS for unavailable operations; do not manufacture a
    // different error to influence a particular library's fallback strategy.
    printk("SYS_FUTEX: unsupported op=%d (cmd=%d) uaddr=%#x val=%d uaddr2=%#x val3=%#x pid=%d comm=%s\n",
           op, cmd, uaddr, val, uaddr2, val3, current->pid, current->comm);
    return _ENOSYS;
}

struct robust_list_head_ {
    addr_t list;
    #ifdef GUEST_ARM64
    int64_t offset;
#else
    int32_t offset;
#endif
    addr_t list_op_pending;
};

static void robust_mark_dead(addr_t entry, intptr_t offset) {
    bool pi = entry & 1;
    addr_t addr = (entry & ~(addr_t)1) + offset;
    if (addr & 3) return;
    read_wrlock(&current->mem->lock);
    uint32_t *ptr = mem_ptr(current->mem, addr, MEM_WRITE);
    uint32_t old = ptr ? __atomic_load_n(ptr, __ATOMIC_SEQ_CST) : 0;
    bool wake = false;
    while (ptr && (old & PI_TID_MASK) == current->pid) {
        uint32_t next = (old & PI_WAITERS) | PI_OWNER_DIED;
        if (__atomic_compare_exchange_n(ptr, &old, next, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            wake = !pi && (old & PI_WAITERS); break;
        }
    }
    read_wrunlock(&current->mem->lock);
    if (wake) futex_wake(addr, 1);
}

void futex_robust_exit(void) {
    struct robust_list_head_ head;
    if (!current->robust_list || user_get(current->robust_list, head)) return;
    addr_t entry = head.list;
    // Bounded traversal: a malformed guest list must not prevent task exit.
    for (unsigned count = 0; count < 2048 && (entry & ~(addr_t)1) != current->robust_list; count++) {
        addr_t next;
        if (user_get(entry & ~(addr_t)1, next)) break;
        if ((entry & ~(addr_t)1) != (head.list_op_pending & ~(addr_t)1))
            robust_mark_dead(entry, head.offset);
        entry = next;
    }
    if (head.list_op_pending) robust_mark_dead(head.list_op_pending, head.offset);
}

int_t sys_set_robust_list(addr_t robust_list, dword_t len) {
    STRACE("set_robust_list(%#x, %d)", robust_list, len);
    if (len != sizeof(struct robust_list_head_))
        return _EINVAL;
    current->robust_list = robust_list;
    return 0;
}

int_t sys_get_robust_list(pid_t_ pid, addr_t robust_list_ptr, addr_t len_ptr) {
    STRACE("get_robust_list(%d, %#x, %#x)", pid, robust_list_ptr, len_ptr);

    lock(&pids_lock);
    struct task *task = pid_get_task(pid ? pid : current->pid);
    unlock(&pids_lock);
    if (task != current)
        return _EPERM;

    if (user_put(robust_list_ptr, current->robust_list))
        return _EFAULT;
    if (user_put(len_ptr, (addr_t[]) {sizeof(struct robust_list_head_)}))
        return _EFAULT;
    return 0;
}
