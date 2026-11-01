#include <string.h>
#include <sys/stat.h>
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "fs/proc.h"
#include "fs/path.h"

// procfs and sysfs share this implementation; the mount picks the tree.
static struct proc_dir_entry *mount_root(struct mount *mount) {
    return mount && mount->fs == &sysfs ? &sys_root : &proc_root;
}

static int proc_lookup(struct proc_dir_entry *root_entry, const char *path, struct proc_entry *entry) {
    entry->meta = root_entry;
    char component[MAX_NAME + 1];
    int err = 0;
    while (path_next_component(&path, component, &err)) {
        if (!S_ISDIR(proc_entry_mode(entry))) {
            err = _ENOTDIR;
            break;
        }

        unsigned long index = 0;
        struct proc_entry next_entry = {0};
        char entry_name[MAX_NAME];
        // Root lookup must not scan every PID below the requested name.
        // Dynamic subdirectories retain their normal enumeration semantics.
        bool root = entry->meta == &proc_root;
        while (root ? proc_root_lookup(component, &next_entry) :
                proc_dir_read(entry, &index, &next_entry)) {
            if (root)
                index = next_entry.index + 1;
            // tack on some dynamically generated attributes
            // A process file is reachable through both /PID and /PID/task/TID.
            // Its opened path belongs to the fd, not the shared metadata node.
            if (next_entry.meta->inode == 0)
                next_entry.meta->inode = index + 1;

            if (root)
                goto found;
            proc_entry_getname(&next_entry, entry_name);
            if (strcmp(entry_name, component) == 0)
                goto found;
            proc_entry_cleanup(&next_entry);
        }
        err = _ENOENT;
        break;
found:
        proc_entry_cleanup(entry);
        *entry = next_entry;
    }
    if (err < 0)
        proc_entry_cleanup(entry);
    return err;
}

static int proc_permission(struct mount *mount, const char *path,
        struct statbuf *stat, int check) {
    int err = access_check(stat, check);
    if (err != _EACCES) return err;
    struct proc_entry entry = {0};
    if (proc_lookup(mount_root(mount), path, &entry) < 0) return err;
    // fs/proc/fd.c: a nondumpable process still needs its own fd directory,
    // including /proc/TID/fd and /proc/PID/task/TID/fd. Foreign groups and
    // fdinfo retain their ordinary DAC/ptrace restrictions.
    if (proc_pid_fd_same_group(&entry)) err = 0;
    proc_entry_cleanup(&entry);
    return err;
}

extern const struct fd_ops procfs_fdops;

static struct fd *proc_open(struct mount *mount, const char *path, int flags, int UNUSED(mode)) {
    struct proc_entry entry = {0};
    int err = proc_lookup(mount_root(mount), path, &entry);
    if (err < 0)
        return ERR_PTR(err);
    struct statbuf st;
    err = proc_entry_stat(&entry, &st);
    if (!err) {
        unsigned access = (flags & 3) == O_RDONLY_ ? AC_R :
            (flags & 3) == O_WRONLY_ ? AC_W : AC_R | AC_W;
        err = proc_permission(mount, path, &st, access);
    }
    if (err < 0) { proc_entry_cleanup(&entry); return ERR_PTR(err); }
    struct fd *fd = fd_create(&procfs_fdops);
    if (!fd) { proc_entry_cleanup(&entry); return ERR_PTR(_ENOMEM); }
    fd->proc.entry = entry;
    fd->proc.path = strdup(path);
    if (!fd->proc.path) { fd_close(fd); return ERR_PTR(_ENOMEM); }
    fd->proc.data.data = NULL;
    return fd;
}

static int proc_getpath(struct fd *fd, char *buf) {
    if (strlen(fd->proc.path) >= MAX_PATH) return _ENAMETOOLONG;
    strcpy(buf, fd->proc.path);
    return 0;
}

static int proc_stat(struct mount *mount, const char *path, struct statbuf *stat) {
    struct proc_entry entry = {0};
    int err = proc_lookup(mount_root(mount), path, &entry);
    if (err < 0)
        return err;
    int ret = proc_entry_stat(&entry, stat);
    proc_entry_cleanup(&entry);
    return ret;
}

static int proc_fstat(struct fd *fd, struct statbuf *stat) {
    return proc_entry_stat(&fd->proc.entry, stat);
}

static int proc_refresh_data(struct fd *fd) {
    mode_t_ mode = proc_entry_mode(&fd->proc.entry);
    if (S_ISDIR(mode))
        return _EISDIR;
    assert(S_ISREG(mode));

    if (fd->proc.data.data == NULL) {
        fd->proc.data.capacity = 4096;
        fd->proc.data.data = malloc(fd->proc.data.capacity); // default size
    }
    fd->proc.data.size = 0;
    struct proc_entry *entry = &fd->proc.entry;
    int err = entry->meta->show(entry, &fd->proc.data);
    if (err < 0)
        return err;
    return 0;
}

static off_t_ proc_seek(struct fd *fd, off_t_ off, int whence) {
    struct proc_entry *entry = &fd->proc.entry;
    if (entry->meta->seek != NULL) {
        qword_t position = fd->offset;
        int err = entry->meta->seek(entry, &position, off, whence);
        if (err < 0)
            return err;
        fd->offset = position;
        return (off_t_) position;
    }

    int err = proc_refresh_data(fd);
    if (err < 0)
        return err;

    err = generic_seek(fd, off, whence, fd->proc.data.size);
    if (err < 0)
        return err;

    return fd->offset;
}

static ssize_t proc_pread(struct fd *fd, void *buf, size_t bufsize, off_t off) {
    if (fd->proc.entry.meta->pread) {
        struct proc_data data = {buf, bufsize, bufsize};
        return fd->proc.entry.meta->pread(&fd->proc.entry, &data, off);
    }
    
    int err = proc_refresh_data(fd);
    if (err < 0)
        return err;

    const char *data = fd->proc.data.data;
    assert(data != NULL);

    size_t remaining = fd->proc.data.size - off;
    if ((size_t) off > fd->proc.data.size)
        remaining = 0;
    size_t n = bufsize;
    if (n > remaining)
        n = remaining;

    memcpy(buf, data + off, n);
    return n;
}

static void proc_buf_write(struct proc_data *buf, const void *data, size_t size, size_t off) {
    assert(off <= buf->size);
    size_t tidemark = off + size;
    if (tidemark > buf->capacity) {
        size_t new_capacity = buf->capacity + 1;
        while (tidemark > new_capacity)
            new_capacity *= 2;
        char *new_data = realloc(buf->data, new_capacity);
        if (new_data == NULL) {
            // just give up, error reporting is a pain
            return;
        }
        buf->data = new_data;
        buf->capacity = new_capacity;
    }
    assert(tidemark <= buf->capacity);
    memcpy(buf->data + off, data, size);
    if (tidemark > buf->size) {
        buf->size = tidemark;
    }
}

static ssize_t proc_pwrite(struct fd *fd, const void *buf, size_t bufsize, off_t off) {
    mode_t_ mode = proc_entry_mode(&fd->proc.entry);
    if (S_ISDIR(mode))
        return _EISDIR;
    assert(S_ISREG(mode));
    
    if (fd->proc.entry.meta->pwrite) {
        struct proc_data data = {(char *) buf, bufsize, bufsize};
        return fd->proc.entry.meta->pwrite(&fd->proc.entry, &data, off);
    }
    
    if (!fd->proc.entry.meta->update) {
        return _EPERM;
    }
    
    struct proc_data data = {(char *)buf, bufsize, bufsize};
    fd->proc.entry.meta->update(&fd->proc.entry, &data);
    
    return bufsize;
}

static int proc_readdir(struct fd *fd, struct dir_entry *entry) {
    struct proc_entry proc_entry = {0};
    bool any_left = proc_dir_read(&fd->proc.entry, &fd->offset, &proc_entry);
    if (!any_left)
        return 0;
    proc_entry_getname(&proc_entry, entry->name);
    // Same lazy numbering as proc_lookup (the post-read directory index + 1),
    // so an entry listed before it was ever looked up still has an inode.
    if (proc_entry.meta->inode == 0)
        proc_entry.meta->inode = fd->offset + 1;
    entry->inode = proc_entry_inode(&proc_entry);
    entry->type = dirent_type_from_mode(proc_entry_mode(&proc_entry));
    proc_entry_cleanup(&proc_entry);
    return 1;
}

static int proc_close(struct fd *fd) {
    free(fd->proc.path);
    if (fd->proc.data.data != NULL)
        free(fd->proc.data.data);
    proc_entry_cleanup(&fd->proc.entry);
    return 0;
}

const struct fd_ops procfs_fdops = {
    .pread = proc_pread,
    .pwrite = proc_pwrite,
    .lseek = proc_seek,
    .readdir = proc_readdir,
    .close = proc_close,
};

static ssize_t proc_readlink(struct mount *mount, const char *path, char *buf, size_t bufsize) {
    struct proc_entry entry = {0};
    int err = proc_lookup(mount_root(mount), path, &entry);
    if (err < 0)
        return err;
    if (!S_ISLNK(proc_entry_mode(&entry))) {
        proc_entry_cleanup(&entry);
        return _EINVAL;
    }

    char target[MAX_PATH + 1];
    err = entry.meta->readlink(&entry, target);
    proc_entry_cleanup(&entry);
    if (err < 0)
        return err;
    if (bufsize > strlen(target))
        bufsize = strlen(target);
    memcpy(buf, target, bufsize);
    return bufsize;
}

static struct fd *proc_follow_link(struct mount *mount, const char *path, bool for_stat) {
    struct proc_entry entry = {0};
    int err = proc_lookup(mount_root(mount), path, &entry);
    if (err < 0) return NULL; // ordinary lookup reports its own error
    struct fd *fd = entry.meta->follow_link ? entry.meta->follow_link(&entry, for_stat) : NULL;
    proc_entry_cleanup(&entry);
    // Synthetic procfs targets (ns objects) are served by this mount.
    if (fd != NULL && !IS_ERR(fd) && fd->mount == NULL) {
        mount_retain(mount);
        fd->mount = mount;
    }
    return fd;
}

static int proc_unlink(struct mount *mount, const char *path) {
    struct proc_entry entry = {0};
    int err = proc_lookup(mount_root(mount), path, &entry);
    if (err < 0)
        return err;
    err = _EPERM;
    if (entry.meta->unlink)
        err = entry.meta->unlink(&entry);
    proc_entry_cleanup(&entry);
    return err;
}

void proc_buf_append(struct proc_data *buf, const void *data, size_t size) {
    proc_buf_write(buf, data, size, buf->size);
}

void proc_printf(struct proc_data *buf, const char *format, ...) {
    char data[4096];
    va_list args;
    va_start(args, format);
    size_t size = vsnprintf(data, sizeof(data), format, args);
    va_end(args);
    proc_buf_append(buf, data, size);
}

// Linux ignores O_TRUNC on writable proc files (e.g. echo 300 > oom_score_adj);
// ownership and mode changes remain unsupported.
static int proc_fsetattr(struct fd *UNUSED(fd), struct attr attr) {
    return attr.type == attr_size ? 0 : _EPERM;
}

// sysfs: the same synthetic-file machinery over fs/sys.c's tree (the software
// DRM device's entries), with sysfs's filesystem magic that udev checks.
const struct fs_ops sysfs = {
    .name = "sysfs", .magic = 0x62656572,
    .fsetattr = proc_fsetattr,
    .permission = proc_permission,
    .open = proc_open,
    .getpath = proc_getpath,
    .stat = proc_stat,
    .fstat = proc_fstat,
    .readlink = proc_readlink,
    .follow_link = proc_follow_link,
    .unlink = proc_unlink,
};

const struct fs_ops procfs = {
    .name = "proc", .magic = 0x9fa0,
    .fsetattr = proc_fsetattr,
    .permission = proc_permission,
    .open = proc_open,
    .getpath = proc_getpath,
    .stat = proc_stat,
    .fstat = proc_fstat,
    .readlink = proc_readlink,
    .follow_link = proc_follow_link,
    .unlink = proc_unlink,
};
