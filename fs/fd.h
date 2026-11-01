#ifndef FD_H
#define FD_H
#include <dirent.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include "kernel/memory.h"
#include "util/list.h"
#include "util/sync.h"
#include "util/bits.h"
#include "fs/stat.h"
#include "fs/proc.h"
#include "fs/sockrestart.h"

// FIXME almost everything that uses the structs in this file does so without any kind of sane locking

// Inode state shared by independent descriptions as well as dup/fork/SCM.
// Native backing is inode-owned; offsets/status belong to each struct fd.
struct memfd_state {
    lock_t lock;
    unsigned seals;
    atomic_uint descriptions;
    // Shared mappings that are or may become writable (Linux i_mmap_writable).
    // F_SEAL_WRITE is refused (EBUSY) while any exist. Changed under lock.
    unsigned maywrite_maps;
    int backing_fd;
    struct statbuf stat;
    char name[256];
};
#define F_SEAL_SEAL_ 1U
#define F_SEAL_SHRINK_ 2U
#define F_SEAL_GROW_ 4U
#define F_SEAL_WRITE_ 8U
#define F_SEAL_FUTURE_WRITE_ 16U
#define F_SEAL_ANY_WRITE_ (F_SEAL_WRITE_ | F_SEAL_FUTURE_WRITE_)

struct fd {
    atomic_uint refcount;
    struct memfd_state *memfd;
    unsigned flags;
    mode_t_ type; // just the S_IFMT part, it can't change
    const struct fd_ops *ops;
    struct list poll_fds;
    lock_t poll_lock;
    unsigned long offset;
    // Links this fd into the global pidfd registry (pidfd_open only).
    struct list pidfd_links;

    // fd data
    union {
        // pidfd (pidfd_open): references a process by pid
        struct {
            pid_t_ pid;
            // Set true by pidfd_notify_exit when `pid` becomes a zombie. Read
            // lock-free by pidfd_poll so polling never takes pids_lock (which
            // would create a poll->lock <-> pids_lock ABBA against the exit
            // path that wakes pidfd pollers while holding pids_lock).
            _Atomic bool exited;
        } pidfd;
        // tty
        struct {
            struct tty *tty;
            // links together fds pointing to the same tty
            // locked by the tty
            struct list tty_other_fds;
        };
        struct {
            struct poll *poll;
        } epollfd;
        struct {
            uint64_t val;
            bool semaphore;
        } eventfd;
        struct {
            struct timer *timer;
            uint64_t expirations;
        } timerfd;
        struct {
            int domain;
            int type;
            int protocol;

            // These are only used as strong references, to keep the inode
            // alive while there is a listener.
            struct inode_data *unix_name_inode;
            struct unix_abstract *unix_name_abstract;
            uint8_t unix_name_len;
            char unix_name[108];
            struct fd *unix_peer; // locked by peer_lock, for simplicity
            _Atomic bool unix_passcred; // locked by peer_lock
            lock_t unix_send_lock; // orders native ancillary markers and metadata
            // Opt-in route-netlink state (socketpair reply endpoint).
            struct uevent_socket *uevent; // protocol-15 state, independent of route dumps
            int netlink_peer_fd;
            uint32_t netlink_port, netlink_groups;
            bool netlink_bound;
            struct lr_netlink_replies *netlink_replies;
            struct lr_netlink_replies *netlink_replies_tail;
            size_t netlink_reply_bytes;
            struct list netlink_link;
            cond_t unix_got_peer;
            // Queue of struct scm for sending file descriptors
            // locked by fd->lock
            struct list unix_scm;
            struct ucred_ {
                pid_t_ pid;
                uid_t_ uid;
                uid_t_ gid;
            } unix_cred;
            struct ucred_ unix_last_cred; // last ancillary segment on a stream
            bool unix_last_cred_valid;
            // Linux connection shutdown state, protected by peer_lock.
            // Native Darwin poll reports HUP on a write-half-closed socket.
            unsigned unix_shutdown; // receive=1, send=2
            bool unix_peer_tracked;
        } socket;

        // See app/Pasteboard.m
        struct {
            // UIPasteboard.changeCount
            uint64_t generation;
            // Buffer for written data
            void* buffer;
            // its capacity
            size_t buffer_cap;
            // length of actual data stored in the buffer
            size_t buffer_len;
        } clipboard;

        // can fit anything in here
        void *data;
    };
    // fs data
    union {
        struct {
            struct proc_entry entry;
            char *path; // proc metadata can appear under PID and task/TID paths
            unsigned dir_index;
            struct proc_data data;
        } proc;
        struct {
            int num;
        } devpts;
        struct {
            struct tmp_dirent *dirent;
            struct tmp_dirent *dir_pos;
        } tmpfs;
        void *fs_data;
    };

    // fs/inode data
    struct mount *mount;
    int real_fd; // seeks on this fd require the lock TODO think about making a special lock just for that
    DIR *dir;
    struct inode_data *inode;
    ino_t fake_inode;
    struct statbuf stat; // for adhoc fs
    struct fd_sockrestart sockrestart; // argh

    // these are used for a variety of things related to the fd
    lock_t lock;
    cond_t cond;
};

typedef sdword_t fd_t;
#define AT_FDCWD_ -100

struct fd *fd_create(const struct fd_ops *ops);
struct fd *fd_create_at(const struct fd_ops *ops, const char *site);
#define fd_create(ops) fd_create_at((ops), __func__)
void fd_diagnostic_snapshot(void);
void fd_report_mapping_failure(struct fd *fd, int guest_fd, const char *stage,
        uint64_t address, uint64_t length, uint64_t offset, unsigned prot,
        unsigned flags, int native_error, int linux_result);
void fd_global_census(void (*emit)(const char *, void *), void *context);
void fd_exhaustion_report(unsigned syscall, int result);
void fd_discard_unopened(struct fd *fd);
void f_close_matching(fd_t f, struct fd *expected);
struct fd *fd_retain(struct fd *fd);
// O_PATH descriptors (fs/generic.c): path operations only, I/O is EBADF.
extern const struct fd_ops opath_fdops;
bool fd_is_path_only(struct fd *fd);
// Host RLIMIT_NOFILE soft limit, raised once at startup (fs/fd.c).
struct host_nofile_state { rlim_t initial_soft, soft, hard, target, per_process; int error; bool disabled; };
const struct host_nofile_state *host_nofile_raise(void);
const char *host_nofile_describe(char *buf, size_t size);
// File mapping identity without a host descriptor (fs/fd.c).
struct fd *fd_mapping_identity(struct fd *source);
bool fd_is_mapping_identity(struct fd *fd);
int fd_mapping_identity_path(struct fd *fd, char *buf);
bool fd_mapping_identity_stat(struct fd *fd, dev_t *dev, ino_t *ino);
int fd_close(struct fd *fd);

int fd_getflags(struct fd *fd);
int fd_setflags(struct fd *fd, int flags);

#define NAME_MAX 255
struct dir_entry {
    qword_t inode;
    byte_t type; // Linux DT_* dirent type, or DT_UNKNOWN when unavailable
    char name[NAME_MAX + 1];
};

static inline byte_t dirent_type_from_mode(mode_t_ mode) {
    if (S_ISREG(mode)) return DT_REG;
    if (S_ISDIR(mode)) return DT_DIR;
    if (S_ISLNK(mode)) return DT_LNK;
    if (S_ISCHR(mode)) return DT_CHR;
    if (S_ISBLK(mode)) return DT_BLK;
    if (S_ISFIFO(mode)) return DT_FIFO;
    if (S_ISSOCK(mode)) return DT_SOCK;
    return DT_UNKNOWN;
}

#define LSEEK_SET 0
#define LSEEK_CUR 1
#define LSEEK_END 2

struct fd_ops {
    // required for files
    // TODO make optional for non-files
    ssize_t (*read)(struct fd *fd, void *buf, size_t bufsize);
    ssize_t (*write)(struct fd *fd, const void *buf, size_t bufsize);
    ssize_t (*pread)(struct fd *fd, void *buf, size_t bufsize, off_t off);
    ssize_t (*pwrite)(struct fd *fd, const void *buf, size_t bufsize, off_t off);
    off_t_ (*lseek)(struct fd *fd, off_t_ off, int whence);

    // Reads a directory entry from the stream
    // required for directories
    int (*readdir)(struct fd *fd, struct dir_entry *entry);
    // Return an opaque value representing the current point in the directory stream
    // optional, fd->offset will be used instead
    unsigned long (*telldir)(struct fd *fd);
    // Seek to the location represented by a pointer returned from telldir
    // optional, fd->offset will be used instead
    void (*seekdir)(struct fd *fd, unsigned long ptr);

    // map the file
    int (*mmap)(struct fd *fd, struct mem *mem, page_t start, pages_t pages, off_t offset, int prot, int flags);

    // returns a bitmask of operations that won't block
    int (*poll)(struct fd *fd);

    // returns the size needed for the output of ioctl, 0 if the arg is not a
    // pointer, -1 for invalid command
    ssize_t (*ioctl_size)(int cmd);
    // if ioctl_size returns non-zero, arg must point to ioctl_size valid bytes
    int (*ioctl)(struct fd *fd, int cmd, void *arg);

    int (*fsync)(struct fd *fd);
    int (*close)(struct fd *fd);

    // handle F_GETFL, i.e. return open flags for this fd
    int (*getflags)(struct fd *fd);
    // Independent open-file description for a procfs magic link.
    struct fd *(*reopen)(struct fd *fd, int flags);
    // handle F_SETFL, i.e. set O_NONBLOCK
    int (*setflags)(struct fd *fd, dword_t arg);
};

struct fdtable {
    atomic_uint refcount;
    unsigned size;
    struct fd **files;
    bits_t *cloexec;
    lock_t lock;
};

struct fdtable *fdtable_new(int size);
void fdtable_release(struct fdtable *table);
struct fdtable *fdtable_copy(struct fdtable *table);
int fdtable_unshare_current(struct fdtable **displaced);
void fdtable_restore_current(struct fdtable *old);
void fdtable_free(struct fdtable *table);
void fdtable_do_cloexec(struct fdtable *table);
struct fd *fdtable_get(struct fdtable *table, fd_t f);

struct fd *f_get(fd_t f);
// The caller owns one reference, acquired under the descriptor-table lock.
struct fd *f_get_retain(fd_t f);
// steals a reference to the fd, gives it to the table on success and destroys it on error
// flags is checked for O_CLOEXEC and O_NONBLOCK
fd_t f_install(struct fd *fd, int flags);
int f_close(fd_t f);

#endif
