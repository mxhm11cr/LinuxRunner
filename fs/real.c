#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <sys/mman.h>
#include <sys/xattr.h>
#include "kernel/filecaps.h"
#include <sys/file.h>
#include <sys/statvfs.h>
#include <poll.h>

#include "debug.h"
#include "kernel/errno.h"
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "fs/dev.h"
#include "fs/devices.h"
#include "fs/real.h"
#include "fs/inode.h"
#define ISH_INTERNAL
#include "fs/fake.h"
#include "fs/tty.h"
#include "platform/platform.h"
#include "util/fchdir.h"

int realfs_getpath(struct fd *fd, char *buf);

static int open_flags_real_from_fake(int flags) {
    int real_flags = 0;
    if (flags & O_RDONLY_) real_flags |= O_RDONLY;
    if (flags & O_WRONLY_) real_flags |= O_WRONLY;
    if (flags & O_RDWR_) real_flags |= O_RDWR;
    if (flags & O_CREAT_) real_flags |= O_CREAT;
    if (flags & O_EXCL_) real_flags |= O_EXCL;
    if (flags & O_TRUNC_) real_flags |= O_TRUNC;
    if (flags & O_APPEND_) real_flags |= O_APPEND;
    if (flags & O_NONBLOCK_) real_flags |= O_NONBLOCK;
    if (flags & O_NOFOLLOW_) real_flags |= O_NOFOLLOW;
    /* O_DIRECTORY is deliberately not forwarded: generic_openat() already
     * enforces ENOTDIR against the guest's view of the type, and Darwin's
     * open() has no O_DIRECTORY. O_PATH/O_DIRECT have no Darwin equivalent
     * either (F_NOCACHE is the closest to O_DIRECT and is advisory), so they
     * stay guest-side only. */
    return real_flags;
}

static int open_flags_fake_from_real(int flags) {
    int fake_flags = 0;
    if (flags & O_RDONLY) fake_flags |= O_RDONLY_;
    if (flags & O_WRONLY) fake_flags |= O_WRONLY_;
    if (flags & O_RDWR) fake_flags |= O_RDWR_;
    if (flags & O_CREAT) fake_flags |= O_CREAT_;
    if (flags & O_EXCL) fake_flags |= O_EXCL_;
    if (flags & O_TRUNC) fake_flags |= O_TRUNC_;
    if (flags & O_APPEND) fake_flags |= O_APPEND_;
    if (flags & O_NONBLOCK) fake_flags |= O_NONBLOCK_;
    if (flags & O_NOFOLLOW) fake_flags |= O_NOFOLLOW_;
    return fake_flags;
}

// Map well-known /dev paths to device numbers. Returns 0 if not recognized.
static dev_t_ realfs_devnum_for_path(const char *path) {
    if (strncmp(path, "/dev/", 5) != 0)
        return 0;
    const char *devname = path + 5;
    if (strcmp(devname, "null") == 0)
        return dev_make(MEM_MAJOR, DEV_NULL_MINOR);
    if (strcmp(devname, "zero") == 0)
        return dev_make(MEM_MAJOR, DEV_ZERO_MINOR);
    if (strcmp(devname, "full") == 0)
        return dev_make(MEM_MAJOR, DEV_FULL_MINOR);
    if (strcmp(devname, "random") == 0)
        return dev_make(MEM_MAJOR, DEV_RANDOM_MINOR);
    if (strcmp(devname, "urandom") == 0)
        return dev_make(MEM_MAJOR, DEV_URANDOM_MINOR);
    if (strcmp(devname, "tty") == 0)
        return dev_make(TTY_ALTERNATE_MAJOR, DEV_TTY_MINOR);
    if (strcmp(devname, "console") == 0)
        return dev_make(TTY_ALTERNATE_MAJOR, DEV_CONSOLE_MINOR);
    if (strcmp(devname, "ptmx") == 0)
        return dev_make(TTY_ALTERNATE_MAJOR, DEV_PTMX_MINOR);
    return 0;
}

struct fd *realfs_open(struct mount *mount, const char *path, int flags, int mode) {
    // generic_openat performs truncation only after guest permission checks
    // and privilege metadata invalidation. Host open must not truncate first.
    flags &= ~O_TRUNC_;
    int real_flags = open_flags_real_from_fake(flags);
    // Allocate before O_CREAT/O_TRUNC can change the backing filesystem.
    struct fd *fd = fd_create(&realfs_fdops);
    if (fd == NULL)
        return ERR_PTR(_ENOMEM);
    int fd_no = openat(mount->root_fd, fix_path(path), real_flags, mode);
    if (fd_no < 0) {
        int err = errno_map();
        fd_discard_unopened(fd);
        return ERR_PTR(err);
    }
    fd->real_fd = fd_no;
    fd->dir = NULL;
    return fd;
}

int realfs_close(struct fd *fd) {
    if (fd->dir != NULL)
        closedir(fd->dir);
    if (fd->memfd) return 0; // shared inode owns the native backing
    int err = close(fd->real_fd);
    if (err < 0)
        return errno_map();
    return 0;
}

static void copy_stat(struct statbuf *fake_stat, struct stat *real_stat) {
    fake_stat->dev = dev_fake_from_real(real_stat->st_dev);
    fake_stat->inode = real_stat->st_ino;
    fake_stat->mode = real_stat->st_mode;
    fake_stat->nlink = real_stat->st_nlink;
    fake_stat->uid = real_stat->st_uid;
    fake_stat->gid = real_stat->st_gid;
    fake_stat->rdev = dev_fake_from_real(real_stat->st_rdev);
    fake_stat->size = real_stat->st_size;
    fake_stat->blksize = real_stat->st_blksize;
    fake_stat->blocks = real_stat->st_blocks;
    fake_stat->atime = platform_stat_atime_sec(real_stat);
    fake_stat->mtime = platform_stat_mtime_sec(real_stat);
    fake_stat->ctime = platform_stat_ctime_sec(real_stat);
    fake_stat->atime_nsec = platform_stat_atime_nsec(real_stat);
    fake_stat->mtime_nsec = platform_stat_mtime_nsec(real_stat);
    fake_stat->ctime_nsec = platform_stat_ctime_nsec(real_stat);
}

int realfs_stat(struct mount *mount, const char *path, struct statbuf *fake_stat) {
    struct stat real_stat;
    if (fstatat(mount->root_fd, fix_path(path), &real_stat, AT_SYMLINK_NOFOLLOW) < 0)
        return errno_map();
    copy_stat(fake_stat, &real_stat);
    // Override mode/rdev for well-known device paths backed by placeholder files
    dev_t_ devnum = realfs_devnum_for_path(path);
    if (devnum != 0) {
        fake_stat->mode = S_IFCHR | 0666;
        fake_stat->rdev = devnum;
    }
    return 0;
}

int realfs_fstat(struct fd *fd, struct statbuf *fake_stat) {
    struct stat real_stat;
    if (fstat(fd->real_fd, &real_stat) < 0)
        return errno_map();
    copy_stat(fake_stat, &real_stat);

    // For fakefs/rootfs images we often ship placeholder regular files under
    // /dev rather than real device nodes. Preserve the Linux-visible device
    // semantics by reclassifying well-known device paths on fstat(), just like
    // realfs_stat() already does for path-based lookup.
    char path[MAX_PATH];
    if (fd->mount != NULL && realfs_getpath(fd, path) == 0) {
        dev_t_ devnum = realfs_devnum_for_path(path);
        if (devnum != 0) {
            fake_stat->mode = S_IFCHR | 0666;
            fake_stat->rdev = devnum;
        }
    }
    return 0;
}

static bool realfs_should_surface_eintr(void) {
    if (current->group->doing_group_exit)
        return true;
    if (current->sighand != NULL) {
        lock(&current->sighand->lock);
        bool pending = !!(current->pending & ~current->blocked);
        unlock(&current->sighand->lock);
        if (pending)
            return true;
    }
    return false;
}

// Caller holds memfd->lock. Match shmem_write_begin: a sealed final partial
// page cannot grow; preceding complete guest pages may still be written.
static ssize_t memfd_write_size(struct fd *fd, size_t count, off_t offset) {
    if ((fd->memfd->seals & F_SEAL_ANY_WRITE_) && count) return _EPERM;
    if (!(fd->memfd->seals & F_SEAL_GROW_) || count == 0) return count;
    struct stat st;
    if (fstat(fd->real_fd, &st) < 0) return errno_map();
    if (fd_getflags(fd) & O_APPEND_) offset = st.st_size;
    if (offset < 0) return _EINVAL;
    if (offset <= st.st_size && count <= (uint64_t)(st.st_size-offset)) return count;
    off_t end = st.st_size - (st.st_size % PAGE_SIZE);
    return end > offset ? (ssize_t)(end-offset) : _EPERM;
}

ssize_t realfs_read(struct fd *fd, void *buf, size_t bufsize) {
    ssize_t res;
    if (fd->memfd) lock(&fd->memfd->lock);
    if (fd->memfd && (fd_getflags(fd) & O_ACCMODE_) == O_WRONLY_) {
        unlock(&fd->memfd->lock); return _EBADF;
    }
    do {
        res = fd->memfd ? pread(fd->real_fd, buf, bufsize, fd->offset) :
            read(fd->real_fd, buf, bufsize);
    } while (res < 0 && errno == EINTR && !realfs_should_surface_eintr());
    int error = res < 0 ? errno_map() : 0;
    if (fd->memfd && res > 0) fd->offset += res;
    if (fd->memfd) unlock(&fd->memfd->lock);
    return error ? error : res;
}

ssize_t realfs_write(struct fd *fd, const void *buf, size_t bufsize) {
    if (S_ISREG(fd->type) && (fd_getflags(fd) & O_ACCMODE_) == O_RDONLY_) return _EBADF;
    if (bufsize) { int err = file_caps_strip(fd); if (err) return err; }
    off_t off = 0;
    if (fd->memfd) {
        lock(&fd->memfd->lock);
        if ((fd_getflags(fd) & O_ACCMODE_) == O_RDONLY_) { unlock(&fd->memfd->lock); return _EBADF; }
        off = fd->offset;
        if (fd_getflags(fd) & O_APPEND_) {
            struct stat st;
            if (fstat(fd->real_fd, &st)) { int err=errno_map(); unlock(&fd->memfd->lock); return err; }
            off = st.st_size;
        }
        ssize_t allowed = memfd_write_size(fd, bufsize, off);
        if (allowed < 0) { unlock(&fd->memfd->lock); return allowed; }
        bufsize = allowed;
    }
    ssize_t res;
    do { res = fd->memfd ? pwrite(fd->real_fd, buf, bufsize, off) : write(fd->real_fd, buf, bufsize); }
    while (res < 0 && errno == EINTR && !realfs_should_surface_eintr());
    int error = res < 0 ? errno_map() : 0;
    if (fd->memfd && res > 0) fd->offset = off + res;
    if (fd->memfd) unlock(&fd->memfd->lock);
    return error ? error : res;
}

ssize_t realfs_pread(struct fd *fd, void *buf, size_t bufsize, off_t off) {
    if (fd->memfd && (fd_getflags(fd) & O_ACCMODE_) == O_WRONLY_) return _EBADF;
    ssize_t res;
    do {
        res = pread(fd->real_fd, buf, bufsize, off);
    } while (res < 0 && errno == EINTR);
    if (res < 0)
        return errno_map();
    return res;
}

ssize_t realfs_pwrite(struct fd *fd, const void *buf, size_t bufsize, off_t off) {
    if (S_ISREG(fd->type) && (fd_getflags(fd) & O_ACCMODE_) == O_RDONLY_) return _EBADF;
    if (off < 0) return _EINVAL;
    if (bufsize) { int err = file_caps_strip(fd); if (err) return err; }
    if (fd->memfd) {
        lock(&fd->memfd->lock);
        if ((fd_getflags(fd) & O_ACCMODE_) == O_RDONLY_) { unlock(&fd->memfd->lock); return _EBADF; }
        if (off < 0) { unlock(&fd->memfd->lock); return _EINVAL; }
        if (fd_getflags(fd) & O_APPEND_) {
            struct stat st;
            if (fstat(fd->real_fd, &st)) { int err=errno_map(); unlock(&fd->memfd->lock); return err; }
            off = st.st_size;
        }
        ssize_t allowed = memfd_write_size(fd, bufsize, off);
        if (allowed < 0) { unlock(&fd->memfd->lock); return allowed; }
        bufsize = allowed;
    }
    ssize_t res;
    do { res = pwrite(fd->real_fd, buf, bufsize, off); } while (res < 0 && errno == EINTR);
    int error = res < 0 ? errno_map() : 0;
    if (fd->memfd) unlock(&fd->memfd->lock);
    return error ? error : res;
}

void realfs_opendir(struct fd *fd) {
    if (fd->dir == NULL) {
        int dirfd = dup(fd->real_fd);
        if (dirfd < 0) return;
        fd->dir = fdopendir(dirfd);
        if (fd->dir == NULL) {
            // fdopendir failed (fd may not be a directory, or was closed).
            // Close the dup'd fd and leave fd->dir NULL for callers to handle.
            close(dirfd);
        }
    }
}

int realfs_readdir(struct fd *fd, struct dir_entry *entry) {
    realfs_opendir(fd);
    if (fd->dir == NULL) return _EIO;
    // Darwin filenames (APFS/HFS+) can be up to 255 UTF-16 units, which in
    // UTF-8 may exceed Linux NAME_MAX (255 bytes). An unchecked strcpy into
    // entry->name[NAME_MAX + 1] smashes the caller's stack-allocated
    // dir_entry (see sys_getdents64 in fs/dir.c) and trips __stack_chk_fail.
    // Skip any entry whose name doesn't fit so the guest can continue walking.
    for (;;) {
        errno = 0;
        struct dirent *dirent = readdir(fd->dir);
        if (dirent == NULL) {
            if (errno != 0)
                return errno_map();
            return 0;
        }
        size_t namelen = strlen(dirent->d_name);
        if (namelen > NAME_MAX) {
            FIXME("realfs_readdir: skipping entry with name longer than NAME_MAX (%zu bytes)", namelen);
            continue;
        }
        entry->inode = dirent->d_ino;
        entry->type = dirent->d_type;
        memcpy(entry->name, dirent->d_name, namelen + 1);
        return 1;
    }
}

unsigned long realfs_telldir(struct fd *fd) {
    realfs_opendir(fd);
    if (fd->dir == NULL) return 0;
    return telldir(fd->dir);
}

void realfs_seekdir(struct fd *fd, unsigned long ptr) {
    realfs_opendir(fd);
    if (fd->dir == NULL) return;
    seekdir(fd->dir, ptr);
}

off_t realfs_lseek(struct fd *fd, off_t offset, int whence) {
    if (fd->dir != NULL && whence == LSEEK_SET) {
        realfs_seekdir(fd, offset);
        return offset;
    }

    if (whence == LSEEK_SET)
        whence = SEEK_SET;
    else if (whence == LSEEK_CUR)
        whence = SEEK_CUR;
    else if (whence == LSEEK_END)
        whence = SEEK_END;
    else
        return _EINVAL;
    if (fd->memfd) {
        lock(&fd->memfd->lock);
        off_t base = whence == SEEK_CUR ? fd->offset : 0, result;
        int error = 0;
        if (whence == SEEK_END) {
            struct stat st;
            if (fstat(fd->real_fd, &st)) error = errno_map();
            else base = st.st_size;
        }
        if (!error && (__builtin_add_overflow(base, offset, &result) || result < 0)) error = _EINVAL;
        if (!error) fd->offset = result;
        unlock(&fd->memfd->lock);
        return error ? error : result;
    }
    off_t res = lseek(fd->real_fd, offset, whence);
    int error = res < 0 ? errno_map() : 0;
    return error ? error : res;
}

int realfs_poll(struct fd *fd) {
    // Linux poll/select report regular files and directories ready for both
    // directions regardless of open access mode or EOF. No host registration
    // is needed; read/write themselves still enforce mode and file semantics.
    if (fd->type == S_IFREG || fd->type == S_IFDIR)
        return POLLIN | POLLOUT;
    struct pollfd p = {.fd = fd->real_fd, .events = POLLPRI};
    // prevent POLLNVAL
    int flags = fcntl(fd->real_fd, F_GETFL, 0);
    if ((flags & O_ACCMODE) != O_WRONLY)
        p.events |= POLLIN;
    if ((flags & O_ACCMODE) != O_RDONLY)
        p.events |= POLLOUT;
    if (poll(&p, 1, 0) <= 0)
        return 0;
    extern volatile bool g_trace_syscalls;
    static _Thread_local unsigned trace_hup;
    if (g_trace_syscalls && (p.revents & POLLHUP) && trace_hup++ < 16)
        printk("NATIVE_POLL_HUP tid=%d native=%d mode=%o flags=%x requested=%x returned=%x\n",
            current->pid, fd->real_fd, fd->stat.mode, flags, p.events, p.revents);

#if defined(__APPLE__)
    // this is the "WTF is apple smoking" section

    // https://github.com/apple/darwin-xnu/blob/a449c6a3b8014d9406c2ddbdc81795da24aa7443/bsd/kern/sys_generic.c#L1856
    if (p.revents & POLLHUP)
        p.revents |= POLLOUT;
    // apparently you can sometimes get POLLPRI on a pipe??? please ignore how much of a mess this condition is
    if (is_adhoc_fd(fd) && S_ISFIFO(fd->stat.mode))
        p.revents &= ~POLLPRI;

    if (p.revents & POLLNVAL) {
        if (g_trace_syscalls)
            printk("pollnval %d flags %d events %d revents %d\n", fd->real_fd, flags, p.events, p.revents);
        // Darwin poll can report POLLNVAL for broad event masks without
        // telling us which individual readiness bits are still meaningful.
        // Ask for each event class separately and ignore POLLNVAL noise.
        // This is no longer atomic, but it preserves useful readiness bits.
        int events = 0;
        static const int pollbits[] = {POLLIN, POLLOUT, POLLPRI};
        for (unsigned i = 0; i < sizeof(pollbits)/sizeof(pollbits[0]); i++) {
            p.events = pollbits[i];
            if (poll(&p, 1, 0) > 0 && !(p.revents & POLLNVAL))
                events |= p.revents;
        }
        assert(!(events & POLLNVAL));
        return events;
    }
#endif

    assert(!(p.revents & POLLNVAL));
    return p.revents;
}

int realfs_mmap(struct fd *fd, struct mem *mem, page_t start, pages_t pages, off_t offset, int prot, int flags) {
    if ((flags & MMAP_SHARED) && (prot & P_WRITE)) {
        int err = file_caps_strip(fd); if (err) return err;
    }
    int mmap_flags = 0;
    if (flags & MMAP_PRIVATE) mmap_flags |= MAP_PRIVATE;
    if (flags & MMAP_SHARED) mmap_flags |= MAP_SHARED;
    int mmap_prot = PROT_READ;
    if (prot & P_WRITE) mmap_prot |= PROT_WRITE;

    off_t real_offset = (offset / real_page_size) * real_page_size;
    off_t correction = offset - real_offset;
    size_t map_size = (pages * PAGE_SIZE) + correction;

    struct stat st;
    int have_stat = (fstat(fd->real_fd, &st) == 0);

    // Check if the mapping extends beyond the file size.
    if (have_stat && (off_t)(real_offset + map_size) > st.st_size) {
        // Every MAP_SHARED mapping must keep the actual file backing, even
        // read-only partial pages: a copied tail cannot observe other writers.
        // The host kernel handles beyond-EOF correctly: writes within the
        // file are flushed on munmap, and the zero-filled region between
        // file end and page boundary is discarded. This is what apk needs
        // for its posix_fallocate + mmap(MAP_SHARED) extraction pattern.
        // We must NOT extend the file via ftruncate, because apk doesn't
        // truncate it back, leaving trailing null bytes that corrupt files.
        if (mmap_flags & MAP_SHARED) {
            char *memory = mmap(NULL, map_size,
                    mmap_prot, mmap_flags, fd->real_fd, real_offset);
            if (memory != MAP_FAILED) {
                return pt_map(mem, start, pages, memory, correction, prot);
            }
            int saved = errno;
            if (runtime_memory_failure) runtime_memory_failure("mmap shared file", 0, map_size, mmap_prot, mmap_flags, saved);
            errno = saved;
            return errno_map(); // never substitute an unshared anonymous copy
        }

        // Create anonymous backing for the full range (zeros for BSS)
        char *memory = mmap(NULL, map_size,
                PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED) {
            if (runtime_memory_failure) runtime_memory_failure("mmap file anonymous backing", 0, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, errno);
            return _ENOMEM;
        }
        size_t file_bytes = 0;
        if (st.st_size > real_offset)
            file_bytes = st.st_size - real_offset;
        if (file_bytes > map_size)
            file_bytes = map_size;
        size_t file_map_size = (file_bytes / real_page_size) * real_page_size;
        if (file_map_size > 0) {
            char *file_map = mmap(memory, file_map_size,
                    mmap_prot, mmap_flags | MAP_FIXED, fd->real_fd, real_offset);
            if (file_map == MAP_FAILED) {
                if (runtime_memory_failure) runtime_memory_failure("mmap file MAP_FIXED inside reserved backing", (uintptr_t)memory, file_map_size, mmap_prot, mmap_flags | MAP_FIXED, errno);
                file_map_size = 0;
            }
        }
        if (file_map_size < file_bytes) {
            size_t remaining = file_bytes - file_map_size;
            size_t total_read = 0;
            while (total_read < remaining) {
                ssize_t n = pread(fd->real_fd, memory + file_map_size + total_read,
                                  remaining - total_read,
                                  real_offset + file_map_size + total_read);
                if (n <= 0) break;
                total_read += n;
            }
        }
        if (!(mmap_prot & PROT_WRITE) && file_map_size < map_size) {
            if (mprotect(memory + file_map_size, map_size - file_map_size, mmap_prot) < 0 && runtime_memory_failure)
                runtime_memory_failure("mprotect file tail", (uintptr_t)(memory + file_map_size), map_size - file_map_size, mmap_prot, 0, errno);
        }

        return pt_map(mem, start, pages, memory, correction, prot);
    }

    char *memory = mmap(NULL, map_size,
            mmap_prot, mmap_flags, fd->real_fd, real_offset);
    if (memory == MAP_FAILED) {
        int native_error = errno;
        int result = errno_map();
        fd_report_mapping_failure(fd, -1, "host_file_mmap", start << PAGE_BITS,
                pages * PAGE_SIZE, offset, prot, flags, native_error, result);
        if (runtime_memory_failure) runtime_memory_failure("mmap file", 0, map_size, mmap_prot, mmap_flags, native_error);
        return result;
    }

    return pt_map(mem, start, pages, memory, correction, prot);
}

ssize_t realfs_readlink(struct mount *mount, const char *path, char *buf, size_t bufsize) {
    ssize_t size = readlinkat(mount->root_fd, fix_path(path), buf, bufsize);
    if (size < 0)
        return errno_map();
    return size;
}

int realfs_getpath(struct fd *fd, char *buf) {
    int err = platform_fd_get_path(fd->real_fd, buf, MAX_PATH);
    if (err < 0)
        return err;

    /* For bind-mounted dirs, F_GETPATH resolves symlinks and returns the
     * host persistent path (e.g. /Users/.../MinisChat/minis/<sid>/attachments).
     * This won't start with mount->source, so the normal prefix strip fails.
     * Detect and translate back to the Linux path. */
    char linux_path[MAX_PATH];
    if (fakefs_bind_mount_resolve_path(buf, linux_path, sizeof(linux_path))) {
        if (strstr(buf, "minis") != NULL)
            fprintf(stderr, "realfs_getpath: bind_mount_resolve OK: \"%s\" -> \"%s\"\n", buf, linux_path);
        strlcpy(buf, linux_path, MAX_PATH);
        return 0;
    }
    if (strstr(buf, "minis") != NULL)
        fprintf(stderr, "realfs_getpath: bind_mount_resolve MISS: F_GETPATH=\"%s\" source=\"%s\"\n", buf, fd->mount->source);

    if (strcmp(fd->mount->source, "/") != 0 || strcmp(buf, "/") == 0) {
        size_t source_len = strlen(fd->mount->source);
        /* Verify buf actually starts with mount->source before stripping.
         * F_GETPATH resolves symlinks, so bind-mounted paths may point outside
         * mount->source when the bind mount table has been cleared (e.g. after
         * app restart before mountMinis re-registers the mounts). */
        if (strncmp(buf, fd->mount->source, source_len) == 0) {
            memmove(buf, buf + source_len, MAX_PATH - source_len);
        } else if (strncmp(fd->mount->source, "/var/", 5) == 0 &&
                   strncmp(buf, "/private", 8) == 0 &&
                   strncmp(buf + 8, fd->mount->source, source_len) == 0) {
            /* F_GETPATH returns /private/var/... but mount->source is /var/...
             * Strip /private prefix + mount->source from buf. */
            memmove(buf, buf + 8 + source_len, MAX_PATH - 8 - source_len);
        } else {
            /* Path is outside mount source (stale bind mount symlink resolved
             * by F_GETPATH). Return root as a safe fallback. */
            strcpy(buf, "/");
        }
    }
    return 0;
}

int realfs_link(struct mount *mount, const char *src, const char *dst) {
    int res = linkat(mount->root_fd, fix_path(src), mount->root_fd, fix_path(dst), 0);
    if (res < 0)
        return errno_map();
    return res;
}

int realfs_unlink(struct mount *mount, const char *path) {
    int res = unlinkat(mount->root_fd, fix_path(path), 0);
    if (res < 0) {
#ifdef __APPLE__
        // Darwin uses EPERM for directory unlink; Linux callers expect EISDIR.
        int saved_errno = errno;
        struct stat statbuf;
        if (saved_errno == EPERM &&
                fstatat(mount->root_fd, fix_path(path), &statbuf, AT_SYMLINK_NOFOLLOW) == 0 &&
                S_ISDIR(statbuf.st_mode))
            return _EISDIR;
        errno = saved_errno;
#endif
        return errno_map();
    }
    return res;
}

int realfs_rmdir(struct mount *mount, const char *path) {
    int err = unlinkat(mount->root_fd, fix_path(path), AT_REMOVEDIR);
    if (err < 0)
        return errno_map();
    return 0;
}

int realfs_rename(struct mount *mount, const char *src, const char *dst) {
    int err = renameat(mount->root_fd, fix_path(src), mount->root_fd, fix_path(dst));
    if (err < 0)
        return errno_map();
    return err;
}

int realfs_symlink(struct mount *mount, const char *target, const char *link) {
    int err = symlinkat(target, mount->root_fd, fix_path(link));
    if (err < 0)
        return errno_map();
    return err;
}

int realfs_mknod(struct mount *mount, const char *path, mode_t_ mode, dev_t_ UNUSED(dev)) {
    int err;
    if (S_ISFIFO(mode)) {
        lock_fchdir(mount->root_fd);
        err = mkfifo(fix_path(path), mode & ~S_IFMT);
        unlock_fchdir();
    } else if (S_ISREG(mode)) {
        err = openat(mount->root_fd, fix_path(path), O_CREAT|O_EXCL|O_RDONLY, mode & ~S_IFMT);
        if (err >= 0)
            err = close(err);
    } else {
        return _EPERM;
    }
    if (err < 0)
        return errno_map();
    return err;
}

int realfs_truncate(struct mount *mount, const char *path, off_t_ size) {
    int fd = openat(mount->root_fd, fix_path(path), O_RDWR);
    if (fd < 0)
        return errno_map();
    int err = 0;
    if (mount->fs->remove_file_caps) {
        err = file_caps_host_remove(fd);
        if (err == _ENODATA || err == _EOPNOTSUPP) err = 0;
        if (err) { close(fd); return err; }
    }
    if (ftruncate(fd, size) < 0)
        err = errno_map();
    close(fd);
    return err;
}

int realfs_setattr(struct mount *mount, const char *path, struct attr attr) {
    path = fix_path(path);
    int root = mount->root_fd;
    int err;
    switch (attr.type) {
        case attr_uid:
            err = fchownat(root, path, attr.uid, -1, 0);
            if (err < 0 && errno == EPERM)
                return 0; // silently ignore, we're not root on host
            break;
        case attr_gid:
            err = fchownat(root, path, attr.gid, -1, 0);
            if (err < 0 && errno == EPERM)
                return 0;
            break;
        case attr_mode:
            err = fchmodat(root, path, attr.mode, 0);
            break;
        case attr_size:
            return realfs_truncate(mount, path, attr.size);
        default:
            TODO("other attrs");
    }
    if (err < 0)
        return errno_map();
    return err;
}

// Caller holds the memfd inode lock; opening with O_TRUNC checks write
// permission separately and may still return an O_RDONLY description.
static int memfd_truncate_locked(struct fd *fd, off_t size) {
    struct stat st;
    if (size < 0) return _EINVAL;
    if (fstat(fd->real_fd, &st) < 0) return errno_map();
    if ((size < st.st_size && (fd->memfd->seals & F_SEAL_SHRINK_)) ||
            (size > st.st_size && (fd->memfd->seals & F_SEAL_GROW_))) return _EPERM;
    return ftruncate(fd->real_fd, size) < 0 ? errno_map() : 0;
}

int realfs_fsetattr(struct fd *fd, struct attr attr) {
    if (attr.type == attr_size) { int err = file_caps_strip(fd); if (err) return err; }
    int real_fd = fd->real_fd;
    int err;
    switch (attr.type) {
        case attr_uid:
            err = fchown(real_fd, attr.uid, -1);
            if (err < 0 && errno == EPERM)
                return 0;
            break;
        case attr_gid:
            err = fchown(real_fd, attr.gid, -1);
            if (err < 0 && errno == EPERM)
                return 0;
            break;
        case attr_mode:
            err = fchmod(real_fd, attr.mode);
            break;
        case attr_size:
            if (fd->memfd) {
                lock(&fd->memfd->lock);
                if ((fd_getflags(fd) & O_ACCMODE_) == O_RDONLY_) err = _EINVAL;
                else err = memfd_truncate_locked(fd, attr.size);
                unlock(&fd->memfd->lock);
                return err;
            }
            err = ftruncate(real_fd, attr.size);
            break;
        default: abort();
    }
    if (err < 0)
        return errno_map();
    return err;
}

int realfs_utime(struct mount *mount, const char *path, struct timespec atime, struct timespec mtime, bool follow_links) {
    struct timespec times[2] = {atime, mtime};
    int err = utimensat(mount->root_fd, fix_path(path), times, follow_links ? 0 : AT_SYMLINK_NOFOLLOW);
    if (err < 0)
        return errno_map();
    return 0;
}

// futimens() on the host descriptor. Never re-resolves a path: the file may
// have been renamed or unlinked since it was opened (RPM writes "name;XXXX"
// temporaries), and on iOS F_GETPATH need not match the guest mount source.
int realfs_futime(struct fd *fd, struct timespec atime, struct timespec mtime) {
    if (fd->real_fd < 0)
        return _EBADF;
    struct timespec times[2] = {atime, mtime};
    if (futimens(fd->real_fd, times) < 0)
        return errno_map();
    return 0;
}

int realfs_mkdir(struct mount *mount, const char *path, mode_t_ mode) {
    int err = mkdirat(mount->root_fd, fix_path(path), mode);
    if (err < 0)
        return errno_map();
    return 0;
}

int realfs_flock(struct fd *fd, int operation) {
    int real_op = 0;
    if (operation & LOCK_SH_) real_op |= LOCK_SH;
    if (operation & LOCK_EX_) real_op |= LOCK_EX;
    if (operation & LOCK_UN_) real_op |= LOCK_UN;
    if (operation & LOCK_NB_) real_op |= LOCK_NB;
    return flock(fd->real_fd, real_op);
}

int realfs_statfs(struct mount *mount, struct statfsbuf *stat) {
    struct statvfs vfs = {};
    fstatvfs(mount->root_fd, &vfs);
    stat->bsize = vfs.f_bsize;
    stat->blocks = vfs.f_blocks;
    stat->bfree = vfs.f_bfree;
    stat->bavail = vfs.f_bavail;
    stat->files = vfs.f_files;
    stat->ffree = vfs.f_ffree;
    stat->namelen = vfs.f_namemax;
    stat->frsize = vfs.f_frsize;
    return 0;
}

int realfs_mount(struct mount *mount) {
    char *source_realpath = realpath(mount->source, NULL);
    if (source_realpath == NULL)
        return errno_map();
    free((void *) mount->source);
    mount->source = source_realpath;

    mount->root_fd = open(mount->source, O_DIRECTORY);
    if (mount->root_fd < 0)
        return errno_map();
    return 0;
}

int realfs_fsync(struct fd *fd) {
    int err = fsync(fd->real_fd);
    if (err < 0)
        return errno_map();
    return 0;
}

int realfs_getflags(struct fd *fd) {
    if (fd->memfd) return __atomic_load_n(&fd->flags, __ATOMIC_RELAXED);
    int flags = fcntl(fd->real_fd, F_GETFL);
    if (flags < 0)
        return errno_map();
    return open_flags_fake_from_real(flags);
}

int realfs_setflags(struct fd *fd, dword_t flags) {
    if (fd->memfd) {
        lock(&fd->memfd->lock);
        unsigned allowed = O_APPEND_ | O_NONBLOCK_;
        unsigned next = (fd_getflags(fd) & ~allowed) | (flags & allowed);
        __atomic_store_n(&fd->flags, next, __ATOMIC_RELAXED);
        unlock(&fd->memfd->lock);
        return 0;
    }
    int ret = fcntl(fd->real_fd, F_SETFL, open_flags_real_from_fake(flags));
    int saved_error = ret < 0 ? errno_map() : 0;
    return saved_error;
}

ssize_t realfs_ioctl_size(int cmd) {
    if (cmd == FIONREAD_)
        return sizeof(dword_t);
    if (cmd == TCGETS_)
        return sizeof(struct termios_);
    if (cmd == TIOCGWINSZ_)
        return sizeof(struct winsize_);
    return -1;
}

int realfs_ioctl(struct fd *fd, int cmd, void *arg) {
    int err;
    size_t nread;
    switch (cmd) {
        case FIONREAD_:
            if (fd->memfd) {
                struct stat st;
                lock(&fd->memfd->lock);
                int error = fstat(fd->real_fd, &st) < 0 ? errno_map() : 0;
                if (!error) *(dword_t *)arg = (dword_t)((uint64_t)st.st_size - fd->offset);
                unlock(&fd->memfd->lock);
                return error;
            }
            err = ioctl(fd->real_fd, FIONREAD, &nread);
            if (err < 0)
                return errno_map();
            *(dword_t *) arg = nread;
            return 0;
        case TCGETS_:
            // For piped stdio fds backed by a real host TTY, return a
            // plausible termios so that musl isatty() succeeds.
            if (isatty(fd->real_fd)) {
                struct termios host_termios;
                if (tcgetattr(fd->real_fd, &host_termios) == 0) {
                    struct termios_ *guest = (struct termios_ *)arg;
                    memset(guest, 0, sizeof(*guest));
                    guest->iflags = host_termios.c_iflag;
                    guest->oflags = host_termios.c_oflag;
                    guest->cflags = host_termios.c_cflag;
                    guest->lflags = host_termios.c_lflag;
                    return 0;
                }
            }
            return _ENOTTY;
        case TIOCGWINSZ_: {
            // libuv calls TIOCGWINSZ during uv_tty_init to get terminal size.
            if (isatty(fd->real_fd)) {
                struct winsize host_ws;
                if (ioctl(fd->real_fd, TIOCGWINSZ, &host_ws) == 0) {
                    struct winsize_ *guest_ws = (struct winsize_ *)arg;
                    guest_ws->row = host_ws.ws_row;
                    guest_ws->col = host_ws.ws_col;
                    guest_ws->xpixel = host_ws.ws_xpixel;
                    guest_ws->ypixel = host_ws.ws_ypixel;
                    return 0;
                }
            }
            return _ENOTTY;
        }
    }
    return _ENOTTY;
}

const struct fs_ops realfs = {
    .name = "real", .magic = 0x7265616c,
    .mount = realfs_mount,
    .statfs = realfs_statfs,

    .open = realfs_open,
    .readlink = realfs_readlink,
    .link = realfs_link,
    .unlink = realfs_unlink,
    .rmdir = realfs_rmdir,
    .rename = realfs_rename,
    .symlink = realfs_symlink,
    .mknod = realfs_mknod,

    .close = realfs_close,
    .stat = realfs_stat,
    .fstat = realfs_fstat,
    .setattr = realfs_setattr,
    .fsetattr = realfs_fsetattr,
    .utime = realfs_utime,
    .futime = realfs_futime,
    .getpath = realfs_getpath,
    .flock = realfs_flock,

    .mkdir = realfs_mkdir,
};

static struct fd *realfs_reopen(struct fd *source, int flags) {
    if (!source->memfd) return ERR_PTR(_ENXIO);
    // O_PATH needs its own restricted operation set, not a readable substitute.
    if (flags & O_PATH_) return ERR_PTR(_EOPNOTSUPP);
    struct fd *fd = adhoc_fd_create(&realfs_fdops);
    if (!fd) return ERR_PTR(_ENOMEM);
    fd->memfd = source->memfd;
    atomic_fetch_add(&fd->memfd->descriptions, 1);
    fd->real_fd = fd->memfd->backing_fd;
    fd->flags = flags & ~(O_CLOEXEC_ | O_CREAT_ | O_EXCL_ | O_TRUNC_ | O_NOCTTY_);
    fd->type = S_IFREG;
    lock(&fd->memfd->lock);
    fd->stat = fd->memfd->stat;
    unlock(&fd->memfd->lock);
    fd->inode = inode_get(fd->mount, fd->stat.inode);
    if (!fd->inode) { fd_close(fd); return ERR_PTR(_ENOMEM); }
    if (flags & O_TRUNC_) {
        lock(&fd->memfd->lock);
        int error = memfd_truncate_locked(fd, 0);
        unlock(&fd->memfd->lock);
        if (error) { fd_close(fd); return ERR_PTR(error); }
    }
    return fd;
}

const struct fd_ops realfs_fdops = {
    .read = realfs_read,
    .write = realfs_write,
    .pread = realfs_pread,
    .pwrite = realfs_pwrite,
    .readdir = realfs_readdir,
    .telldir = realfs_telldir,
    .seekdir = realfs_seekdir,
    .lseek = realfs_lseek,
    .mmap = realfs_mmap,
    .poll = realfs_poll,
    .ioctl_size = realfs_ioctl_size,
    .ioctl = realfs_ioctl,
    .fsync = realfs_fsync,
    .close = realfs_close,
    .getflags = realfs_getflags,
    .setflags = realfs_setflags,
    .reopen = realfs_reopen,
};
