#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>

#include "kernel/calls.h"
#include "kernel/memory.h"
#include "kernel/mm.h"
#include "kernel/ipc.h"
#include "kernel/time.h"
#include "platform/platform.h"
#include "util/sync.h"

#define IPC_PRIVATE_ 0
#define IPC_CREAT_ 01000
#define IPC_EXCL_ 02000
#define IPC_RMID_ 0
#define IPC_SET_ 1
#define IPC_STAT_ 2
#define IPC_NOWAIT_ 04000

#define MSG_NOERROR_ 010000
#define MSG_EXCEPT_ 020000

#define SHM_RDONLY_ 010000
#define SHM_RND_ 020000
#define SHM_REMAP_ 040000
#define SHM_DEST_ 01000
#define SHM_EXEC_ 0100000

#define GETPID_ 11
#define GETVAL_ 12
#define GETALL_ 13
#define GETNCNT_ 14
#define GETZCNT_ 15
#define SETVAL_ 16
#define SETALL_ 17

#define SEM_UNDO_ 010000

struct shm_segment {
    int id;
    int key;
    size_t size;
    pages_t pages;
    int fd;
    bool removed;
    uid_t_ uid, gid, cuid, cgid;
    unsigned mode;
    int64_t atime, dtime, ctime;
    pid_t_ cpid, lpid;
    atomic_uint_fast64_t attaches;
    uint64_t capture_scope;
    struct shm_segment *next;
};

struct shm_attach {
    struct shm_segment *segment;
    addr_t addr;
    pages_t pages;
    struct shm_attach *next;
};

struct guest_ipc_perm {
    int32_t key;
    uint32_t uid;
    uint32_t gid;
    uint32_t cuid;
    uint32_t cgid;
    uint32_t mode;
    int32_t seq;
    int64_t pad1;
    int64_t pad2;
};

struct guest_shmid_ds {
    struct guest_ipc_perm shm_perm;
    uint64_t shm_segsz;
    int64_t shm_atime;
    int64_t shm_dtime;
    int64_t shm_ctime;
    int32_t shm_cpid;
    int32_t shm_lpid;
    uint64_t shm_nattch;
    uint64_t pad1;
    uint64_t pad2;
};

struct msg_message {
    int64_t type;
    size_t size;
    char *data;
    struct msg_message *next;
};

struct msg_queue {
    int id;
    int key;
    struct msg_message *messages;
    struct msg_queue *next;
};

struct sem_set {
    int id;
    int key;
    int nsems;
    unsigned short *values;
    pid_t_ *pids;
    struct sem_set *next;
};

struct semop_ {
    unsigned short num;
    short op;
    short flags;
};

struct mq_message {
    uint_t prio;
    size_t size;
    char *data;
    struct mq_message *next;
};

struct mq_queue {
    char name[256];
    int unlinked;
    int open_count;
    int64_t flags;
    int64_t maxmsg;
    int64_t msgsize;
    int64_t curmsgs;
    struct mq_message *messages;
    struct mq_queue *next;
};

struct mq_attr_ {
    int64_t flags;
    int64_t maxmsg;
    int64_t msgsize;
    int64_t curmsgs;
};

static ssize_t mqfd_read(struct fd *fd, void *buf, size_t bufsize);
static ssize_t mqfd_write(struct fd *fd, const void *buf, size_t bufsize);
static int mqfd_close(struct fd *fd);
static int mqfd_getflags(struct fd *fd);
static int mqfd_setflags(struct fd *fd, dword_t flags);
static const struct fd_ops mqfd_ops = {
    .read = mqfd_read,
    .write = mqfd_write,
    .close = mqfd_close,
    .getflags = mqfd_getflags,
    .setflags = mqfd_setflags,
};

static lock_t shm_lock = LOCK_INITIALIZER;
static cond_t msg_cond = COND_INITIALIZER;
static cond_t sem_cond = COND_INITIALIZER;
static cond_t mq_cond = COND_INITIALIZER;
static int next_shmid = 1;
static int next_msgid = 1;
static int next_semid = 1;
static struct shm_segment *shm_segments;
static struct msg_queue *msg_queues;
static struct sem_set *sem_sets;
static struct mq_queue *mq_queues;
struct capture_shm_scope { uint64_t id; unsigned threads; };
// Running owners live in host TLS, independent of the guest task struct. The
// group-exit safety valve may destroy/zero that struct before pthread cleanup.
// A genuinely stuck host thread retains its lease and any live mappings until
// it really exits; reclaiming those mappings earlier would be unsafe.
static __thread struct capture_shm_scope *current_capture_shm;
static uint64_t next_capture_scope = 1;
static unsigned long long capture_scopes, shm_backing_fds, shm_close_failures;

static void shm_close_backing(int fd) {
    if (close(fd) == 0) shm_backing_fds--;
    else shm_close_failures++;
}

// Existing runtime privilege model: effective UID zero, not host credentials.
static int shm_access(struct shm_segment *seg, unsigned requested) {
    if (superuser()) return 0;
    unsigned mode = seg->mode;
    if (current->euid == seg->uid || current->euid == seg->cuid) mode >>= 6;
    else if (task_in_group(seg->gid) || task_in_group(seg->cgid)) mode >>= 3;
    return (mode & requested) == requested ? 0 : _EACCES;
}

static bool shm_owner(struct shm_segment *seg) {
    return superuser() || current->euid == seg->uid || current->euid == seg->cuid;
}

static struct shm_segment *shm_find_id(int shmid) {
    for (struct shm_segment *seg = shm_segments; seg; seg = seg->next)
        if (seg->id == shmid)
            return seg;
    return NULL;
}

static struct shm_segment *shm_find_key(int key) {
    for (struct shm_segment *seg = shm_segments; seg; seg = seg->next)
        if (!seg->removed && seg->key == key)
            return seg;
    return NULL;
}

// IPC_RMID removes the key immediately, but an attached segment and its ID
// remain valid until the last detach. Host mappings also survive fd close,
// so freeing the descriptor early hid broken Linux attachment semantics.
static void shm_destroy_if_unused(struct shm_segment *seg) {
    if (!seg || !seg->removed) return;
    if (atomic_load(&seg->attaches)) return;
    struct shm_segment **p = &shm_segments;
    while (*p && *p != seg) p = &(*p)->next;
    if (*p) {
        *p = seg->next;
        shm_close_backing(seg->fd);
        free(seg);
    }
}

int ipc_capture_shm_enable(void) {
    if (current_capture_shm) return 0;
    // No other thread can clone into this group once this check observes
    // exactly the caller. Never acquire the IPC lock while holding pids_lock.
    lock(&pids_lock);
    bool single = current == current->group->leader &&
        current->group->threads.next == &current->group_links &&
        current->group_links.next == &current->group->threads;
    unlock(&pids_lock);
    if (!single) return _EBUSY;
    struct capture_shm_scope *scope = malloc(sizeof(*scope));
    if (!scope) return _ENOMEM;
    lock(&shm_lock);
    if (!next_capture_scope) { unlock(&shm_lock); free(scope); return _EOVERFLOW; }
    scope->id = next_capture_scope++;
    scope->threads = 1;
    current_capture_shm = scope;
    capture_scopes++;
    unlock(&shm_lock);
    return 0;
}

void ipc_capture_shm_inherit_thread(struct task *child) {
    if (!current_capture_shm) return;
    lock(&shm_lock);
    child->capture_shm = current_capture_shm;
    child->capture_shm->threads++;
    unlock(&shm_lock);
}

void ipc_capture_shm_enter_current(void) {
    current_capture_shm = current->capture_shm;
    current->capture_shm = NULL; // transfer the unstarted task's ownership to TLS
}

static void capture_shm_release(struct capture_shm_scope *scope) {
    if (!scope) return;
    lock(&shm_lock);
    if (--scope->threads == 0) {
        struct shm_segment *seg = shm_segments;
        while (seg) {
            struct shm_segment *next = seg->next;
            if (seg->capture_scope == scope->id) {
                seg->removed = true;
                shm_destroy_if_unused(seg);
            }
            seg = next;
        }
        capture_scopes--;
        free(scope);
    }
    unlock(&shm_lock);
}

void ipc_capture_shm_exit_current(void) {
    struct capture_shm_scope *scope = current_capture_shm;
    current_capture_shm = NULL;
    capture_shm_release(scope);
}

void ipc_capture_shm_discard(struct task *task) {
    struct capture_shm_scope *scope = task->capture_shm;
    task->capture_shm = NULL;
    capture_shm_release(scope);
}

void ipc_shm_usage(struct ipc_shm_usage *usage) {
    *usage = (struct ipc_shm_usage) {};
    lock(&shm_lock);
    usage->backing_fds = shm_backing_fds;
    usage->scopes = capture_scopes;
    usage->close_failures = shm_close_failures;
    for (struct shm_segment *s = shm_segments; s; s = s->next) {
        usage->segments++; usage->bytes += s->size;
        usage->attachments += atomic_load(&s->attaches);
        if (s->capture_scope) usage->capture_segments++;
    }
    unlock(&shm_lock);
}

// The caller holds source->mem.lock, the same lock protecting shmat/shmdt
// records. Parent attachments retain each segment while their references are
// copied, so fork does not take the global IPC/semaphore lock.
int ipc_shm_mm_copy(struct mm *source, struct mm *destination) {
    struct shm_attach *copies = NULL;
    for (struct shm_attach *a = source->sysv_shm; a; a = a->next) {
        struct shm_attach *copy = malloc(sizeof(*copy));
        if (!copy) {
            while (copies) { copy = copies; copies = copy->next; free(copy); }
            return _ENOMEM;
        }
        *copy = *a; copy->next = copies; copies = copy;
    }
    for (struct shm_attach *a = copies; a; a = a->next)
        atomic_fetch_add(&a->segment->attaches, 1);
    destination->sysv_shm = copies;
    return 0;
}

void ipc_shm_mm_release(struct mm *mm) {
    if (!mm->sysv_shm) return; // Normal non-SysV processes need no IPC lock.
    lock(&shm_lock);
    while (mm->sysv_shm) {
        struct shm_attach *a = mm->sysv_shm;
        mm->sysv_shm = a->next;
        struct shm_segment *seg = a->segment;
        atomic_fetch_sub(&seg->attaches, 1);
        free(a);
        shm_destroy_if_unused(seg);
    }
    unlock(&shm_lock);
}

static struct msg_queue *msg_find_id(int msqid) {
    for (struct msg_queue *queue = msg_queues; queue; queue = queue->next)
        if (queue->id == msqid)
            return queue;
    return NULL;
}

static struct msg_queue *msg_find_key(int key) {
    for (struct msg_queue *queue = msg_queues; queue; queue = queue->next)
        if (queue->key == key)
            return queue;
    return NULL;
}

static struct sem_set *sem_find_id(int semid) {
    for (struct sem_set *set = sem_sets; set; set = set->next)
        if (set->id == semid)
            return set;
    return NULL;
}

static struct sem_set *sem_find_key(int key) {
    for (struct sem_set *set = sem_sets; set; set = set->next)
        if (set->key == key)
            return set;
    return NULL;
}

static struct mq_queue *mq_find_name(const char *name) {
    for (struct mq_queue *queue = mq_queues; queue; queue = queue->next)
        if (!queue->unlinked && strcmp(queue->name, name) == 0)
            return queue;
    return NULL;
}

static void mq_free_messages(struct mq_queue *queue) {
    struct mq_message *msg = queue->messages;
    while (msg != NULL) {
        struct mq_message *next = msg->next;
        free(msg->data);
        free(msg);
        msg = next;
    }
}

static void mq_maybe_free_unlocked(struct mq_queue *queue) {
    if (!queue->unlinked || queue->open_count > 0)
        return;
    struct mq_queue **qp = &mq_queues;
    while (*qp != NULL && *qp != queue)
        qp = &(*qp)->next;
    if (*qp == queue)
        *qp = queue->next;
    mq_free_messages(queue);
    free(queue);
}

static void mq_fill_attr(struct mq_queue *queue, struct mq_attr_ *attr) {
    attr->flags = queue->flags;
    attr->maxmsg = queue->maxmsg;
    attr->msgsize = queue->msgsize;
    attr->curmsgs = queue->curmsgs;
}

static int mqfd_close(struct fd *fd) {
    lock(&shm_lock);
    struct mq_queue *queue = fd->data;
    if (queue != NULL) {
        queue->open_count--;
        mq_maybe_free_unlocked(queue);
    }
    unlock(&shm_lock);
    return 0;
}

static int mqfd_getflags(struct fd *fd) {
    struct mq_queue *queue = fd->data;
    if (queue == NULL)
        return 0;
    return queue->flags & O_NONBLOCK_;
}

static int mqfd_setflags(struct fd *fd, dword_t flags) {
    lock(&shm_lock);
    struct mq_queue *queue = fd->data;
    if (queue != NULL)
        queue->flags = flags & O_NONBLOCK_;
    unlock(&shm_lock);
    return 0;
}

static ssize_t mqfd_read(struct fd *fd, void *buf, size_t bufsize) {
    struct mq_queue *queue = fd->data;
    if (queue == NULL)
        return _EBADF;
    lock(&shm_lock);
    for (;;) {
        if (queue->messages != NULL)
            break;
        if (queue->flags & O_NONBLOCK_) {
            unlock(&shm_lock);
            return _EAGAIN;
        }
        int err = wait_for(&mq_cond, &shm_lock, NULL);
        if (err < 0 && err != _EINTR) {
            unlock(&shm_lock);
            return err;
        }
        if (err == _EINTR) {
            unlock(&shm_lock);
            return err;
        }
    }
    struct mq_message *msg = queue->messages;
    if (bufsize < msg->size) {
        unlock(&shm_lock);
        return _EMSGSIZE;
    }
    queue->messages = msg->next;
    queue->curmsgs--;
    size_t size = msg->size;
    memcpy(buf, msg->data, size);
    free(msg->data);
    free(msg);
    notify(&mq_cond);
    unlock(&shm_lock);
    return size;
}

static ssize_t mqfd_write(struct fd *fd, const void *buf, size_t bufsize) {
    struct mq_queue *queue = fd->data;
    if (queue == NULL)
        return _EBADF;
    if ((int64_t)bufsize > queue->msgsize)
        return _EMSGSIZE;
    struct mq_message *msg = calloc(1, sizeof(*msg));
    if (msg == NULL)
        return _ENOMEM;
    msg->data = malloc(bufsize ? bufsize : 1);
    if (msg->data == NULL) {
        free(msg);
        return _ENOMEM;
    }
    memcpy(msg->data, buf, bufsize);
    msg->size = bufsize;
    lock(&shm_lock);
    while (queue->curmsgs >= queue->maxmsg) {
        if (queue->flags & O_NONBLOCK_) {
            unlock(&shm_lock);
            free(msg->data);
            free(msg);
            return _EAGAIN;
        }
        int err = wait_for(&mq_cond, &shm_lock, NULL);
        if (err < 0) {
            unlock(&shm_lock);
            free(msg->data);
            free(msg);
            return err;
        }
    }
    struct mq_message **tail = &queue->messages;
    while (*tail != NULL)
        tail = &(*tail)->next;
    *tail = msg;
    queue->curmsgs++;
    notify(&mq_cond);
    unlock(&shm_lock);
    return bufsize;
}

int_t sys_mq_open(addr_t name_addr, int_t oflag, mode_t_ mode, addr_t attr_addr) {
    char name[256];
    if (user_read_string(name_addr, name, sizeof(name)))
        return _EFAULT;
    STRACE("mq_open(\"%s\", %#x, %#x, %#llx)", name, oflag, mode, (unsigned long long)attr_addr);
    if (name[0] == '\0')
        return _EINVAL;
    lock(&shm_lock);
    struct mq_queue *queue = mq_find_name(name);
    if (queue != NULL && (oflag & O_CREAT_) && (oflag & O_EXCL_)) {
        unlock(&shm_lock);
        return _EEXIST;
    }
    if (queue == NULL) {
        if (!(oflag & O_CREAT_)) {
            unlock(&shm_lock);
            return _ENOENT;
        }
        struct mq_attr_ attr = {.maxmsg = 10, .msgsize = 8192};
        if (attr_addr != 0) {
            unlock(&shm_lock);
            if (user_read(attr_addr, &attr, sizeof(attr)))
                return _EFAULT;
            lock(&shm_lock);
        }
        if (attr.maxmsg <= 0 || attr.msgsize <= 0) {
            unlock(&shm_lock);
            return _EINVAL;
        }
        queue = calloc(1, sizeof(*queue));
        if (queue == NULL) {
            unlock(&shm_lock);
            return _ENOMEM;
        }
        strlcpy(queue->name, name, sizeof(queue->name));
        queue->maxmsg = attr.maxmsg;
        queue->msgsize = attr.msgsize;
        queue->next = mq_queues;
        mq_queues = queue;
    }
    queue->open_count++;
    queue->flags = oflag & O_NONBLOCK_;
    unlock(&shm_lock);
    struct fd *fd = adhoc_fd_create(&mqfd_ops);
    if (fd == NULL) {
        lock(&shm_lock);
        queue->open_count--;
        mq_maybe_free_unlocked(queue);
        unlock(&shm_lock);
        return _ENOMEM;
    }
    fd->data = queue;
    fd->stat.mode = S_IFIFO | (mode & 0777);
    return f_install(fd, oflag);
}

int_t sys_mq_unlink(addr_t name_addr) {
    char name[256];
    if (user_read_string(name_addr, name, sizeof(name)))
        return _EFAULT;
    STRACE("mq_unlink(\"%s\")", name);
    lock(&shm_lock);
    struct mq_queue *queue = mq_find_name(name);
    if (queue == NULL) {
        unlock(&shm_lock);
        return _ENOENT;
    }
    queue->unlinked = 1;
    mq_maybe_free_unlocked(queue);
    unlock(&shm_lock);
    return 0;
}

int_t sys_mq_timedsend(fd_t mqdes, addr_t msg_ptr, size_t msg_len, uint_t msg_prio, addr_t abs_timeout_addr) {
    (void)abs_timeout_addr;
    STRACE("mq_timedsend(%d, %#llx, %#zx, %u, %#llx)", mqdes, (unsigned long long)msg_ptr, msg_len, msg_prio, (unsigned long long)abs_timeout_addr);
    struct fd *fd = f_get(mqdes);
    if (fd == NULL || fd->ops != &mqfd_ops)
        return _EBADF;
    char *buf = malloc(msg_len ? msg_len : 1);
    if (buf == NULL)
        return _ENOMEM;
    if (user_read(msg_ptr, buf, msg_len)) {
        free(buf);
        return _EFAULT;
    }
    struct mq_queue *queue = fd->data;
    struct mq_message *msg = calloc(1, sizeof(*msg));
    if (msg == NULL) {
        free(buf);
        return _ENOMEM;
    }
    msg->data = buf;
    msg->size = msg_len;
    msg->prio = msg_prio;
    lock(&shm_lock);
    if ((int64_t)msg_len > queue->msgsize) {
        unlock(&shm_lock);
        free(msg->data);
        free(msg);
        return _EMSGSIZE;
    }
    while (queue->curmsgs >= queue->maxmsg) {
        if (queue->flags & O_NONBLOCK_) {
            unlock(&shm_lock);
            free(msg->data);
            free(msg);
            return _EAGAIN;
        }
        int err = wait_for(&mq_cond, &shm_lock, NULL);
        if (err < 0) {
            unlock(&shm_lock);
            free(msg->data);
            free(msg);
            return err;
        }
    }
    struct mq_message **slot = &queue->messages;
    while (*slot != NULL && (*slot)->prio >= msg_prio)
        slot = &(*slot)->next;
    msg->next = *slot;
    *slot = msg;
    queue->curmsgs++;
    notify(&mq_cond);
    unlock(&shm_lock);
    return 0;
}

ssize_t sys_mq_timedreceive(fd_t mqdes, addr_t msg_ptr, size_t msg_len, addr_t msg_prio_addr, addr_t abs_timeout_addr) {
    (void)abs_timeout_addr;
    STRACE("mq_timedreceive(%d, %#llx, %#zx, %#llx, %#llx)", mqdes, (unsigned long long)msg_ptr, msg_len, (unsigned long long)msg_prio_addr, (unsigned long long)abs_timeout_addr);
    struct fd *fd = f_get(mqdes);
    if (fd == NULL || fd->ops != &mqfd_ops)
        return _EBADF;
    struct mq_queue *queue = fd->data;
    lock(&shm_lock);
    while (queue->messages == NULL) {
        if (queue->flags & O_NONBLOCK_) {
            unlock(&shm_lock);
            return _EAGAIN;
        }
        int err = wait_for(&mq_cond, &shm_lock, NULL);
        if (err < 0) {
            unlock(&shm_lock);
            return err;
        }
    }
    struct mq_message *msg = queue->messages;
    if (msg_len < msg->size) {
        unlock(&shm_lock);
        return _EMSGSIZE;
    }
    queue->messages = msg->next;
    queue->curmsgs--;
    size_t size = msg->size;
    uint_t prio = msg->prio;
    char *data = msg->data;
    free(msg);
    notify(&mq_cond);
    unlock(&shm_lock);
    int err = user_write(msg_ptr, data, size);
    free(data);
    if (err)
        return _EFAULT;
    if (msg_prio_addr != 0 && user_put(msg_prio_addr, prio))
        return _EFAULT;
    return size;
}

int_t sys_mq_notify(fd_t mqdes, addr_t notification_addr) {
    (void)notification_addr;
    struct fd *fd = f_get(mqdes);
    if (fd == NULL || fd->ops != &mqfd_ops)
        return _EBADF;
    return 0;
}

int_t sys_mq_getsetattr(fd_t mqdes, addr_t newattr_addr, addr_t oldattr_addr) {
    struct fd *fd = f_get(mqdes);
    if (fd == NULL || fd->ops != &mqfd_ops)
        return _EBADF;
    lock(&shm_lock);
    struct mq_queue *queue = fd->data;
    struct mq_attr_ old;
    mq_fill_attr(queue, &old);
    if (newattr_addr != 0) {
        struct mq_attr_ newattr;
        unlock(&shm_lock);
        if (user_read(newattr_addr, &newattr, sizeof(newattr)))
            return _EFAULT;
        lock(&shm_lock);
        queue = fd->data;
        queue->flags = newattr.flags & O_NONBLOCK_;
    }
    unlock(&shm_lock);
    if (oldattr_addr != 0 && user_write(oldattr_addr, &old, sizeof(old)))
        return _EFAULT;
    return 0;
}

int_t sys_msgget(int_t key, int_t msgflg) {
    STRACE("msgget(%#x, %#x)", key, msgflg);
    lock(&shm_lock);
    struct msg_queue *queue = key == IPC_PRIVATE_ ? NULL : msg_find_key(key);
    if (queue != NULL) {
        if ((msgflg & IPC_CREAT_) && (msgflg & IPC_EXCL_)) {
            unlock(&shm_lock);
            return _EEXIST;
        }
        int id = queue->id;
        unlock(&shm_lock);
        return id;
    }
    if (key != IPC_PRIVATE_ && !(msgflg & IPC_CREAT_)) {
        unlock(&shm_lock);
        return _ENOENT;
    }
    queue = calloc(1, sizeof(*queue));
    if (queue == NULL) {
        unlock(&shm_lock);
        return _ENOMEM;
    }
    queue->id = next_msgid++;
    if (next_msgid <= 0)
        next_msgid = 1;
    queue->key = key;
    queue->next = msg_queues;
    msg_queues = queue;
    int id = queue->id;
    unlock(&shm_lock);
    return id;
}

int_t sys_msgctl(int_t msqid, int_t cmd, addr_t buf_addr) {
    STRACE("msgctl(%d, %d, %#llx)", msqid, cmd, (unsigned long long) buf_addr);
    lock(&shm_lock);
    struct msg_queue **queuep = &msg_queues;
    while (*queuep != NULL && (*queuep)->id != msqid)
        queuep = &(*queuep)->next;
    struct msg_queue *queue = *queuep;
    if (queue == NULL) {
        unlock(&shm_lock);
        return _EINVAL;
    }
    if (cmd == IPC_RMID_) {
        *queuep = queue->next;
        struct msg_message *msg = queue->messages;
        while (msg != NULL) {
            struct msg_message *next = msg->next;
            free(msg->data);
            free(msg);
            msg = next;
        }
        free(queue);
        notify(&msg_cond);
        unlock(&shm_lock);
        return 0;
    }
    unlock(&shm_lock);
    if (cmd == IPC_STAT_ || cmd == IPC_SET_)
        return 0;
    return _EINVAL;
}

int_t sys_msgsnd(int_t msqid, addr_t msgp, size_t msgsz, int_t msgflg) {
    STRACE("msgsnd(%d, %#llx, %#zx, %#x)", msqid, (unsigned long long) msgp, msgsz, msgflg);
    (void) msgflg;
    int64_t type;
    if (user_read(msgp, &type, sizeof(type)) < 0)
        return _EFAULT;
    if (type <= 0)
        return _EINVAL;
    struct msg_message *msg = calloc(1, sizeof(*msg));
    if (msg == NULL)
        return _ENOMEM;
    msg->data = malloc(msgsz ? msgsz : 1);
    if (msg->data == NULL) {
        free(msg);
        return _ENOMEM;
    }
    if (msgsz && user_read(msgp + sizeof(type), msg->data, msgsz) < 0) {
        free(msg->data);
        free(msg);
        return _EFAULT;
    }
    msg->type = type;
    msg->size = msgsz;

    lock(&shm_lock);
    struct msg_queue *queue = msg_find_id(msqid);
    if (queue == NULL) {
        unlock(&shm_lock);
        free(msg->data);
        free(msg);
        return _EINVAL;
    }
    struct msg_message **tail = &queue->messages;
    while (*tail != NULL)
        tail = &(*tail)->next;
    *tail = msg;
    notify(&msg_cond);
    unlock(&shm_lock);
    return 0;
}

static bool msg_matches(int64_t have, int64_t want, int_t flags) {
    if (want == 0)
        return true;
    if (want > 0) {
        if (flags & MSG_EXCEPT_)
            return have != want;
        return have == want;
    }
    return have <= -want;
}

ssize_t sys_msgrcv(int_t msqid, addr_t msgp, size_t msgsz, int64_t msgtyp, int_t msgflg) {
    STRACE("msgrcv(%d, %#llx, %#zx, %lld, %#x)", msqid, (unsigned long long) msgp, msgsz, (long long) msgtyp, msgflg);
    lock(&shm_lock);
    for (;;) {
        struct msg_queue *queue = msg_find_id(msqid);
        if (queue == NULL) {
            unlock(&shm_lock);
            return _EINVAL;
        }
        struct msg_message **msgp_link = &queue->messages;
        struct msg_message **best_link = NULL;
        int64_t best_type = INT64_MAX;
        while (*msgp_link != NULL) {
            struct msg_message *candidate = *msgp_link;
            if (msg_matches(candidate->type, msgtyp, msgflg)) {
                if (msgtyp < 0) {
                    if (candidate->type < best_type) {
                        best_type = candidate->type;
                        best_link = msgp_link;
                    }
                } else {
                    best_link = msgp_link;
                    break;
                }
            }
            msgp_link = &candidate->next;
        }
        if (best_link != NULL) {
            struct msg_message *msg = *best_link;
            if (msg->size > msgsz && !(msgflg & MSG_NOERROR_)) {
                unlock(&shm_lock);
                return _E2BIG;
            }
            *best_link = msg->next;
            size_t copy = msg->size < msgsz ? msg->size : msgsz;
            int64_t type = msg->type;
            char *data = msg->data;
            size_t original_size = msg->size;
            free(msg);
            unlock(&shm_lock);
            int err = user_write(msgp, &type, sizeof(type));
            if (err == 0 && copy)
                err = user_write(msgp + sizeof(type), data, copy);
            free(data);
            if (err < 0)
                return _EFAULT;
            return copy < original_size ? (ssize_t) copy : (ssize_t) original_size;
        }
        if (msgflg & IPC_NOWAIT_) {
            unlock(&shm_lock);
            return _ENODATA;
        }
        current->blocking = true;
        int err = wait_for(&msg_cond, &shm_lock, NULL);
        current->blocking = false;
        if (err < 0) {
            unlock(&shm_lock);
            return err;
        }
    }
}

int_t sys_semget(int_t key, int_t nsems, int_t semflg) {
    STRACE("semget(%#x, %d, %#x)", key, nsems, semflg);
    lock(&shm_lock);
    struct sem_set *set = key == IPC_PRIVATE_ ? NULL : sem_find_key(key);
    if (set != NULL) {
        if ((semflg & IPC_CREAT_) && (semflg & IPC_EXCL_)) {
            unlock(&shm_lock);
            return _EEXIST;
        }
        if (nsems > 0 && nsems > set->nsems) {
            unlock(&shm_lock);
            return _EINVAL;
        }
        int id = set->id;
        unlock(&shm_lock);
        return id;
    }
    if (key != IPC_PRIVATE_ && !(semflg & IPC_CREAT_)) {
        unlock(&shm_lock);
        return _ENOENT;
    }
    if (nsems <= 0) {
        unlock(&shm_lock);
        return _EINVAL;
    }
    set = calloc(1, sizeof(*set));
    if (set == NULL) {
        unlock(&shm_lock);
        return _ENOMEM;
    }
    set->values = calloc(nsems, sizeof(unsigned short));
    set->pids = calloc(nsems, sizeof(pid_t_));
    if (set->values == NULL || set->pids == NULL) {
        free(set->values);
        free(set->pids);
        free(set);
        unlock(&shm_lock);
        return _ENOMEM;
    }
    set->id = next_semid++;
    if (next_semid <= 0)
        next_semid = 1;
    set->key = key;
    set->nsems = nsems;
    set->next = sem_sets;
    sem_sets = set;
    int id = set->id;
    unlock(&shm_lock);
    return id;
}

int_t sys_semctl(int_t semid, int_t semnum, int_t cmd, addr_t arg) {
    STRACE("semctl(%d, %d, %d, %#llx)", semid, semnum, cmd, (unsigned long long)arg);
    lock(&shm_lock);
    struct sem_set **setp = &sem_sets;
    while (*setp != NULL && (*setp)->id != semid)
        setp = &(*setp)->next;
    struct sem_set *set = *setp;
    if (set == NULL) {
        unlock(&shm_lock);
        return _EINVAL;
    }
    if (cmd == IPC_RMID_) {
        *setp = set->next;
        free(set->values);
        free(set->pids);
        free(set);
        notify(&sem_cond);
        unlock(&shm_lock);
        return 0;
    }
    if (semnum < 0 || semnum >= set->nsems) {
        unlock(&shm_lock);
        return _EINVAL;
    }
    switch (cmd) {
        case GETVAL_: {
            int val = set->values[semnum];
            unlock(&shm_lock);
            return val;
        }
        case SETVAL_: {
            set->values[semnum] = (unsigned short)(arg & 0xffff);
            set->pids[semnum] = current->pid;
            notify(&sem_cond);
            unlock(&shm_lock);
            return 0;
        }
        case GETPID_: {
            int pid = set->pids[semnum];
            unlock(&shm_lock);
            return pid;
        }
        case GETNCNT_:
        case GETZCNT_:
            unlock(&shm_lock);
            return 0;
        case GETALL_: {
            unsigned short *vals = malloc(sizeof(unsigned short) * set->nsems);
            if (vals == NULL) {
                unlock(&shm_lock);
                return _ENOMEM;
            }
            memcpy(vals, set->values, sizeof(unsigned short) * set->nsems);
            unlock(&shm_lock);
            int err = user_write(arg, vals, sizeof(unsigned short) * set->nsems);
            free(vals);
            return err ? _EFAULT : 0;
        }
        case SETALL_: {
            unsigned short *vals = malloc(sizeof(unsigned short) * set->nsems);
            if (vals == NULL) {
                unlock(&shm_lock);
                return _ENOMEM;
            }
            unlock(&shm_lock);
            if (user_read(arg, vals, sizeof(unsigned short) * set->nsems)) {
                free(vals);
                return _EFAULT;
            }
            lock(&shm_lock);
            set = sem_find_id(semid);
            if (set == NULL) {
                unlock(&shm_lock);
                free(vals);
                return _EINVAL;
            }
            memcpy(set->values, vals, sizeof(unsigned short) * set->nsems);
            for (int i = 0; i < set->nsems; i++)
                set->pids[i] = current->pid;
            notify(&sem_cond);
            unlock(&shm_lock);
            free(vals);
            return 0;
        }
        case IPC_STAT_:
        case IPC_SET_:
            unlock(&shm_lock);
            return 0;
        default:
            unlock(&shm_lock);
            return _EINVAL;
    }
}

static int sem_try_apply(struct sem_set *set, struct semop_ *ops, size_t nsops, bool apply) {
    for (size_t i = 0; i < nsops; i++) {
        if (ops[i].num >= set->nsems)
            return _EFBIG;
        int val = set->values[ops[i].num];
        if (ops[i].op < 0 && val < -ops[i].op)
            return _EAGAIN;
        if (ops[i].op == 0 && val != 0)
            return _EAGAIN;
    }
    if (apply) {
        for (size_t i = 0; i < nsops; i++) {
            int idx = ops[i].num;
            set->values[idx] = (unsigned short)(set->values[idx] + ops[i].op);
            set->pids[idx] = current->pid;
        }
    }
    return 0;
}

int_t sys_semtimedop(int_t semid, addr_t sops_addr, size_t nsops, addr_t timeout_addr) {
    STRACE("semtimedop(%d, %#llx, %#zx, %#llx)", semid, (unsigned long long)sops_addr, nsops, (unsigned long long)timeout_addr);
    if (nsops == 0 || nsops > 1024)
        return _EINVAL;
    struct semop_ *ops = malloc(sizeof(*ops) * nsops);
    if (ops == NULL)
        return _ENOMEM;
    if (user_read(sops_addr, ops, sizeof(*ops) * nsops)) {
        free(ops);
        return _EFAULT;
    }
    struct timespec timeout, *timeout_ptr = NULL;
    if (timeout_addr != 0) {
        struct timespec_ ts;
        if (user_get(timeout_addr, ts)) {
            free(ops);
            return _EFAULT;
        }
        timeout.tv_sec = ts.sec;
        timeout.tv_nsec = ts.nsec;
        timeout_ptr = &timeout;
    }

    lock(&shm_lock);
    int err;
    for (;;) {
        struct sem_set *set = sem_find_id(semid);
        if (set == NULL) {
            err = _EINVAL;
            break;
        }
        err = sem_try_apply(set, ops, nsops, true);
        if (err == 0) {
            notify(&sem_cond);
            break;
        }
        bool nowait = false;
        for (size_t i = 0; i < nsops; i++)
            if (ops[i].flags & IPC_NOWAIT_)
                nowait = true;
        if (err != _EAGAIN || nowait)
            break;
        current->blocking = true;
        err = wait_for(&sem_cond, &shm_lock, timeout_ptr);
        current->blocking = false;
        if (err == _ETIMEDOUT) {
            err = _EAGAIN;
            break;
        }
        if (err < 0 && err != _EINTR)
            break;
        if (err == _EINTR)
            break;
    }
    unlock(&shm_lock);
    free(ops);
    return err;
}

int_t sys_semop(int_t semid, addr_t sops_addr, size_t nsops) {
    return sys_semtimedop(semid, sops_addr, nsops, 0);
}

int_t sys_shmget(int_t key, size_t size, int_t shmflg) {
    STRACE("shmget(%#x, %#zx, %#x)", key, size, shmflg);
    lock(&shm_lock);

    struct shm_segment *seg = key == IPC_PRIVATE_ ? NULL : shm_find_key(key);
    if (seg != NULL) {
        if ((shmflg & IPC_CREAT_) && (shmflg & IPC_EXCL_)) {
            unlock(&shm_lock);
            return _EEXIST;
        }
        if (size > seg->size) {
            unlock(&shm_lock);
            return _EINVAL;
        }
        unsigned requested = (shmflg | (shmflg >> 3) | (shmflg >> 6)) & 7;
        int err = shm_access(seg, requested);
        int id = seg->id;
        unlock(&shm_lock);
        return err ? err : id;
    }

    if (key != IPC_PRIVATE_ && !(shmflg & IPC_CREAT_)) {
        unlock(&shm_lock);
        return _ENOENT;
    }
    if (size == 0) {
        unlock(&shm_lock);
        return _EINVAL;
    }

    pages_t pages = PAGE_ROUND_UP(size);
    size_t map_size = (size_t) pages << PAGE_BITS;
    int fd = platform_create_shared_memory_fd(map_size);
    if (fd < 0) {
        int err = errno_map();
        unlock(&shm_lock);
        return err;
    }
    shm_backing_fds++;
    fcntl(fd, F_SETFD, FD_CLOEXEC);

    seg = calloc(1, sizeof(*seg));
    if (seg == NULL) {
        shm_close_backing(fd);
        unlock(&shm_lock);
        return _ENOMEM;
    }
    seg->id = next_shmid++;
    if (next_shmid <= 0)
        next_shmid = 1;
    seg->key = key;
    seg->size = size;
    seg->pages = pages;
    seg->fd = fd;
    seg->uid = seg->cuid = current->euid;
    seg->gid = seg->cgid = current->egid;
    seg->mode = shmflg & 0777;
    seg->cpid = current->tgid;
    if (key == IPC_PRIVATE_ && current_capture_shm)
        seg->capture_scope = current_capture_shm->id;
    seg->ctime = time(NULL);
    atomic_init(&seg->attaches, 0);
    seg->next = shm_segments;
    shm_segments = seg;

    int id = seg->id;
    unlock(&shm_lock);
    return id;
}

int_t sys_shmctl(int_t shmid, int_t cmd, addr_t buf_addr) {
    STRACE("shmctl(%d, %d, %#llx)", shmid, cmd, (unsigned long long) buf_addr);
    // Copy before locking: a guest fault must leave the object unchanged.
    struct guest_shmid_ds update;
    if (cmd == IPC_SET_ && user_read(buf_addr, &update, sizeof(update)))
        return _EFAULT;
    lock(&shm_lock);
    struct shm_segment *seg = shm_find_id(shmid);
    if (!seg) { unlock(&shm_lock); return _EINVAL; }
    int err = 0;
    switch (cmd) {
        case IPC_RMID_:
            if (!shm_owner(seg)) { err = _EPERM; break; }
            seg->removed = true;
            seg->key = IPC_PRIVATE_;
            shm_destroy_if_unused(seg);
            break;
        case IPC_SET_:
            if (!shm_owner(seg)) { err = _EPERM; break; }
            if (update.shm_perm.uid == UINT32_MAX || update.shm_perm.gid == UINT32_MAX) {
                err = _EINVAL; break;
            }
            seg->uid = update.shm_perm.uid;
            seg->gid = update.shm_perm.gid;
            seg->mode = update.shm_perm.mode & 0777;
            seg->ctime = time(NULL);
            break;
        case IPC_STAT_: {
            err = shm_access(seg, 4);
            if (err) break;
            struct guest_shmid_ds ds = {
                .shm_perm = {.key = seg->key, .uid = seg->uid, .gid = seg->gid,
                    .cuid = seg->cuid, .cgid = seg->cgid,
                    .mode = seg->mode | (seg->removed ? SHM_DEST_ : 0)},
                .shm_segsz = seg->size, .shm_nattch = atomic_load(&seg->attaches),
                .shm_atime = seg->atime, .shm_dtime = seg->dtime,
                .shm_ctime = seg->ctime, .shm_cpid = seg->cpid, .shm_lpid = seg->lpid,
            };
            unlock(&shm_lock);
            return user_write(buf_addr, &ds, sizeof(ds)) ? _EFAULT : 0;
        }
        default: err = _EINVAL; break;
    }
    unlock(&shm_lock);
    return err;
}

addr_t sys_shmat(int_t shmid, addr_t shmaddr, int_t shmflg) {
    STRACE("shmat(%d, %#llx, %#x)", shmid, (unsigned long long) shmaddr, shmflg);
    lock(&shm_lock);
    struct shm_segment *seg = shm_find_id(shmid);
    if (seg == NULL) {
        unlock(&shm_lock);
        return _EINVAL;
    }

    unsigned requested = (shmflg & SHM_RDONLY_) ? 4 : 6;
    if (shmflg & SHM_EXEC_) requested |= 1;
    int access_err = shm_access(seg, requested);
    if (access_err) { unlock(&shm_lock); return access_err; }
    pages_t pages = seg->pages;
    size_t map_size = (size_t) pages << PAGE_BITS;
    int host_prot = PROT_READ;
    unsigned guest_prot = P_READ | P_SHARED;
    if (!(shmflg & SHM_RDONLY_)) {
        host_prot |= PROT_WRITE;
        guest_prot |= P_WRITE;
    }

    // Guest execute permission is interpreter metadata; never PROT_EXEC on host.
    if (shmflg & SHM_EXEC_) guest_prot |= P_EXEC;
    void *host_map = mmap(NULL, map_size, host_prot, MAP_SHARED, seg->fd, 0);
    if (host_map == MAP_FAILED) {
        if (runtime_memory_failure) runtime_memory_failure("mmap IPC shared", 0, map_size, host_prot, MAP_SHARED, errno);
        int err = errno_map();
        unlock(&shm_lock);
        return err;
    }

    write_wrlock(&current->mem->lock);
    page_t page;
    if (shmaddr != 0) {
        if (shmflg & SHM_RND_)
            shmaddr = BYTES_ROUND_DOWN(shmaddr);
        if (PGOFFSET(shmaddr) != 0) {
            write_wrunlock(&current->mem->lock);
            munmap(host_map, map_size);
            unlock(&shm_lock);
            return _EINVAL;
        }
        page = PAGE(shmaddr);
        if (!(shmflg & SHM_REMAP_) && !pt_is_hole(current->mem, page, pages)) {
            write_wrunlock(&current->mem->lock);
            munmap(host_map, map_size);
            unlock(&shm_lock);
            return _EINVAL;
        }
    } else {
        page = pt_find_hole(current->mem, pages);
        if (page == BAD_PAGE) {
            write_wrunlock(&current->mem->lock);
            munmap(host_map, map_size);
            unlock(&shm_lock);
            return _ENOMEM;
        }
    }

    struct shm_attach *attach = calloc(1, sizeof(*attach));
    if (!attach) {
        write_wrunlock(&current->mem->lock);
        munmap(host_map, map_size);
        unlock(&shm_lock);
        return _ENOMEM;
    }
    int err = pt_map(current->mem, page, pages, host_map, 0, guest_prot);
    if (err < 0) {
        write_wrunlock(&current->mem->lock);
        free(attach);
        munmap(host_map, map_size);
        unlock(&shm_lock);
        return err;
    }
    attach->segment = seg;
    attach->addr = page << PAGE_BITS;
    attach->pages = pages;
    attach->next = current->mm->sysv_shm;
    current->mm->sysv_shm = attach;
    atomic_fetch_add(&seg->attaches, 1);
    seg->atime = time(NULL);
    seg->lpid = current->tgid;
    write_wrunlock(&current->mem->lock);

    addr_t result = page << PAGE_BITS;
    unlock(&shm_lock);
    return result;
}

int_t sys_shmdt(addr_t shmaddr) {
    STRACE("shmdt(%#llx)", (unsigned long long) shmaddr);
    if (PGOFFSET(shmaddr) != 0)
        return _EINVAL;

    lock(&shm_lock);
    write_wrlock(&current->mem->lock);
    struct shm_attach **attachp = &current->mm->sysv_shm;
    while (*attachp != NULL && (*attachp)->addr != shmaddr)
        attachp = &(*attachp)->next;
    struct shm_attach *attach = *attachp;
    if (attach == NULL) {
        write_wrunlock(&current->mem->lock);
        unlock(&shm_lock);
        return _EINVAL;
    }
    pages_t pages = attach->pages;
    struct shm_segment *seg = attach->segment;

    int err = pt_unmap(current->mem, PAGE(shmaddr), pages);
    if (err < 0) {
        write_wrunlock(&current->mem->lock);
        unlock(&shm_lock);
        return _EINVAL;
    }
    *attachp = attach->next;
    atomic_fetch_sub(&seg->attaches, 1);
    seg->dtime = time(NULL);
    seg->lpid = current->tgid;
    free(attach);
    write_wrunlock(&current->mem->lock);
    shm_destroy_if_unused(seg);
    unlock(&shm_lock);
    return 0;
}

int_t sys_ipc(uint_t call, int_t first, int_t second, int_t third, addr_t ptr, int_t fifth) {
    STRACE("ipc(%u, %d, %d, %d, %#x, %d)", call, first, second, third, ptr, fifth);
    return _ENOSYS;
}
