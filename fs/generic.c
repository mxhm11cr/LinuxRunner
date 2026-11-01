#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "kernel/fs.h"
#include "fs/fd.h"
#include "fs/inode.h"
#include "fs/path.h"
#include "fs/dev.h"
#include "kernel/task.h"
#include "kernel/errno.h"

int mount_access_check(struct mount *mount, const char *path, struct statbuf *stat, int check) {
    return mount->fs->permission ? mount->fs->permission(mount, path, stat, check) :
        access_check(stat, check);
}

struct mount *find_mount_and_trim_path(char *path) {
    struct mount *mount = mount_find(path);
    char *dst = path;
    const char *src = path + strlen(mount->point);
    while (*src != '\0')
        *dst++ = *src++;
    *dst = '\0';
    return mount;
}

bool contains_mount_point(const char *path) {
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        int n = strlen(path);
        if (strncmp(path, mount->point, n) == 0 &&
                (mount->point[n] == '\0' || mount->point[n] == '/'))
            return true;
    }
    return false;
}

static struct fd *generic_openat_impl(struct fs_info *fs, struct fd *at,
        const char *path_raw, int flags, int mode, bool execute) {
    if (flags & O_RDWR_ && flags & O_WRONLY_)
        return ERR_PTR(_EINVAL);

    // TODO really, really, seriously reconsider what I'm doing with the strings
    char path[MAX_PATH];
    // O_NOFOLLOW has to be honored here rather than by handing the flag to the
    // host open(): path_normalize resolves the final component itself, so by
    // the time the fs open() runs the symlink is already gone. Leaving it
    // unresolved makes the S_ISLNK check below return ELOOP, as Linux does.
    int normalize_flags = flags & O_NOFOLLOW_ ? N_SYMLINK_NOFOLLOW : N_SYMLINK_FOLLOW;
    struct fd *target = NULL;
    int err = fs ? path_normalize_in_fs(fs, at, path_raw, path, normalize_flags) :
        path_normalize_target(at, path_raw, path, normalize_flags, &target);
    if (err < 0)
        return ERR_PTR(err);
    if (target) {
        struct statbuf st;
        err = target->mount->fs->fstat(target, &st);
        if (!err && flags & O_CREAT_ && flags & O_EXCL_) err = _EEXIST;
        if (!err && flags & O_DIRECTORY_) err = _ENOTDIR;
        int access = execute ? AC_X : (flags & O_ACCMODE_) == O_RDWR_ ? AC_R | AC_W :
            (flags & O_ACCMODE_) == O_WRONLY_ ? AC_W : AC_R;
        if (!err && (flags & O_TRUNC_) && S_ISREG(st.mode)) access |= AC_W;
        if (!err) err = access_check(&st, access);
        struct fd *opened = err ? ERR_PTR(err) : target->ops->reopen ?
            target->ops->reopen(target, flags) : ERR_PTR(_ENXIO);
        fd_close(target);
        return opened;
    }
    // A create needs parent write permission only when the target is absent.
    // Existing O_CREAT opens must work in a read-only directory.
    bool creating = false;
    if (flags & O_CREAT_) {
        struct statbuf existing;
        // path already belongs to the explicit retained fs, if supplied.
        // Do not resolve it again through current->fs during context opens.
        char existing_path[MAX_PATH];
        strcpy(existing_path, path);
        struct mount *existing_mount = find_mount_and_trim_path(existing_path);
        int exists = existing_mount->fs->stat(existing_mount, existing_path, &existing);
        mount_release(existing_mount);
        if (exists == _ENOENT) {
            err = fs ? path_normalize_in_fs(fs, at, path_raw, path, normalize_flags | N_PARENT_DIR_WRITE) :
                path_normalize(at, path_raw, path, normalize_flags | N_PARENT_DIR_WRITE);
            if (err < 0) return ERR_PTR(err);
            creating = true;
        } else if (exists < 0) return ERR_PTR(exists);
    }
    struct mount *mount = find_mount_and_trim_path(path);
    // Never truncate before checking the opened object's Linux permissions.
    // Exclusive creation proves that the mode-created object is ours; a raced
    // existing object must still pass ordinary access checks.
    int open_flags = flags & ~O_TRUNC_;
    if (creating) open_flags |= O_EXCL_;
    struct fd *fd = mount->fs->open(mount, path, open_flags, mode);
    if (creating && PTR_ERR(fd) == _EEXIST && !(flags & O_EXCL_)) {
        creating = false;
        fd = mount->fs->open(mount, path, flags & ~(O_CREAT_ | O_TRUNC_), mode);
    }
    if (IS_ERR(fd)) {
        // if an error happens after this point, fd_close will release the
        // mount, but right now we need to do it manually
        mount_release(mount);
        return fd;
    }
    fd->mount = mount;

    struct statbuf stat;
    // Filesystem callbacks must not run under the global inode lock. Procfs
    // fstat takes pids_lock; a proc reader can hold that while waiting on mm,
    // whose unmap path holds mm while calling inode_release: a lock cycle.
    // Fakefs already pins its inode during open, before metadata can vanish.
    // Other filesystems retain their opened object through fd itself.
    err = fd->mount->fs->fstat(fd, &stat);
    if (err < 0)
        goto error;
    if (fd->inode == NULL) {
        fd->inode = inode_get(mount, stat.inode);
        if (fd->inode == NULL) {
            err = _ENOMEM;
            goto error;
        }
    }
    fd->type = stat.mode & S_IFMT;
    fd->flags = flags;

    if (!S_ISDIR(fd->type) && flags & O_DIRECTORY_) { err = _ENOTDIR; goto error; }

    int accmode;
    if (flags & O_RDWR_) accmode = AC_R | AC_W;
    else if (flags & O_WRONLY_) accmode = AC_W;
    else accmode = AC_R;
    if (execute) accmode = AC_X;
    if (flags & O_TRUNC_) accmode |= AC_W;
    if (flags & O_PATH_) accmode = 0;   // no permission needed on the target itself
    if (!creating && accmode) {
        err = mount_access_check(mount, path, &stat, accmode);
        if (err < 0) goto error;
    }

    // O_PATH|O_NOFOLLOW names the symlink itself (Linux 2.6.39; systemd's
    // chase/tmpfiles open the links they create this way). Any other open of
    // an unresolved symlink is ELOOP: O_NOFOLLOW proper, or path_normalize
    // having left one unresolved.
    if (S_ISLNK(fd->type) && !((flags & O_PATH_) && (flags & O_NOFOLLOW_))) {
        err = _ELOOP;
        goto error;
    }
    if ((S_ISBLK(fd->type) || S_ISCHR(fd->type)) && !(flags & O_PATH_)) {
        int type;
        if (S_ISBLK(fd->type))
            type = DEV_BLOCK;
        else
            type = DEV_CHAR;
        err = dev_open(dev_major(stat.rdev), dev_minor(stat.rdev), type, fd);
        if (err < 0)
            goto error;
    }
    err = _ENXIO;
    if (S_ISSOCK(fd->type) && !(flags & O_PATH_))
        goto error;
    err = _EISDIR;
    if (S_ISDIR(fd->type) && flags & (O_RDWR_ | O_WRONLY_))
        goto error;
    err = _ENOTDIR;
    if (!S_ISDIR(fd->type) && flags & O_DIRECTORY_)
        goto error;
    if ((flags & O_TRUNC_) && S_ISREG(fd->type) && !(flags & O_PATH_)) {
        if (!mount->fs->fsetattr) { err = _EINVAL; goto error; }
        err = mount->fs->fsetattr(fd, make_attr(size, 0));
        if (err < 0) goto error;
    }
    if (flags & O_PATH_) {
        // Devices keep their identity but no device ops (no I/O through O_PATH).
        fd->ops = &opath_fdops;
        fd->flags = flags & (O_PATH_ | O_DIRECTORY_ | O_NOFOLLOW_ | O_CLOEXEC_);
    }
    return fd;

error:
    fd_close(fd);
    return ERR_PTR(err);
}

// O_PATH (Linux 2.6.39): a descriptor naming a file without opening it for
// I/O. Path operations work (fstat, *at with it as dirfd, fchdir, readlink of
// /proc/self/fd), I/O fails with EBADF, and opening needs no read or write
// permission on the file itself. The underlying description keeps its mount
// and host descriptor for those path operations; mount close still runs.
static off_t_ opath_lseek(struct fd *UNUSED(fd), off_t_ UNUSED(off), int UNUSED(whence)) { return _EBADF; }
const struct fd_ops opath_fdops = { .lseek = opath_lseek };
bool fd_is_path_only(struct fd *fd) { return fd->ops == &opath_fdops; }

struct fd *generic_openat(struct fd *at, const char *path, int flags, int mode) {
    return generic_openat_impl(NULL, at, path, flags, mode, false);
}

// Execution reads file bytes using host descriptors but checks Linux execute
// permission, not guest read permission (execute-only binaries are valid).
struct fd *generic_open_execat(struct fd *at, const char *path, int flags) {
    return generic_openat_impl(NULL, at, path, flags, 0, true);
}

struct fd *generic_open_in_fs(struct fs_info *fs, const char *path, int flags, int mode) {
    return generic_openat_impl(fs, AT_PWD, path, flags, mode, false);
}

struct fd *generic_open(const char *path, int flags, int mode) {
    return generic_openat(AT_PWD, path, flags, mode);
}

int generic_getpath(struct fd *fd, char *buf) {
    if (fd_is_mapping_identity(fd))
        return fd_mapping_identity_path(fd, buf);
    int err = fd->mount->fs->getpath(fd, buf);
    if (err < 0)
        return err;
    if (strlen(buf) + strlen(fd->mount->point) >= MAX_PATH)
        return _ENAMETOOLONG;
    memmove(buf + strlen(fd->mount->point), buf, strlen(buf) + 1);
    memcpy(buf, fd->mount->point, strlen(fd->mount->point));
    if (buf[0] == '\0')
        strcpy(buf, "/");
    return 0;
}

int generic_accessat(struct fd *dirfd, const char *path_raw, int mode) {
    char path[MAX_PATH];
    // Like stat, resolve procfs magic links (fd/N, ns/*) to their target object.
    struct fd *target = NULL;
    int err = path_normalize_target(dirfd, path_raw, path, N_SYMLINK_FOLLOW | N_STAT_TARGET, &target);
    if (err < 0)
        return err;
    if (target) {
        struct statbuf stat = {};
        err = target->mount->fs->fstat(target, &stat);
        if (!err && mode) err = access_check(&stat, mode);
        fd_close(target);
        return err;
    }

    struct mount *mount = find_mount_and_trim_path(path);
    struct statbuf stat = {};
    err = mount->fs->stat(mount, path, &stat);
    if (!err) err = mount_access_check(mount, path, &stat, mode);
    mount_release(mount);
    return err;
}

int generic_linkat(struct fd *src_at, const char *src_raw, struct fd *dst_at, const char *dst_raw) {
    char src[MAX_PATH];
    int err = path_normalize(src_at, src_raw, src, N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    char dst[MAX_PATH];
    err = path_normalize(dst_at, dst_raw, dst, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(src);
    struct mount *dst_mount = find_mount_and_trim_path(dst);
    if (mount != dst_mount)
        err = _EXDEV;
    else if (mount->fs->link == NULL)
        err = _EPERM;
    else
        err = mount->fs->link(mount, src, dst);
    mount_release(mount);
    mount_release(dst_mount);
    return err;
}

int generic_unlinkat(struct fd *at, const char *path_raw) {
    char path[MAX_PATH];
    /* Include N_PARENT_DIR_WRITE so path_normalize checks write permission
     * on the parent directory. This makes read-only bind mounts (whose
     * top-level directory is mode 0555 in meta.db) reject `rm` with EACCES
     * without needing any special case in fakefs. */
    int err = path_normalize(at, path_raw, path, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    err = _EPERM;
    if (mount->fs->unlink)
        err = mount->fs->unlink(mount, path);
    mount_release(mount);
    return err;
}

int generic_renameat(struct fd *src_at, const char *src_raw, struct fd *dst_at, const char *dst_raw) {
    char src[MAX_PATH];
    /* Rename removes the src from its parent directory, so we also need
     * write permission on src's parent — not just dst's. */
    int err = path_normalize(src_at, src_raw, src, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    char dst[MAX_PATH];
    err = path_normalize(dst_at, dst_raw, dst, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    if (contains_mount_point(src))
        return _EBUSY;
    struct mount *mount = find_mount_and_trim_path(src);
    struct mount *dst_mount = find_mount_and_trim_path(dst);
    if (mount != dst_mount)
        err = _EXDEV;
    else if (mount->fs->rename == NULL)
        err = _EPERM;
    else
        err = mount->fs->rename(mount, src, dst);
    mount_release(mount);
    mount_release(dst_mount);
    return err;
}

int generic_symlinkat(const char *target, struct fd *at, const char *link_raw) {
    char link[MAX_PATH];
    int err = path_normalize(at, link_raw, link, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(link);
    err = _EPERM;
    if (mount->fs->symlink)
        err = mount->fs->symlink(mount, target, link);
    mount_release(mount);
    return err;
}

int generic_mknodat(struct fd *at, const char *path_raw, mode_t_ mode, dev_t_ dev) {
    if (S_ISDIR(mode) || S_ISLNK(mode))
        return _EINVAL;
    if (!superuser() && (S_ISBLK(mode) || S_ISCHR(mode)))
        return _EPERM;

    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    err = _EPERM;
    if (mount->fs->mknod)
        err = mount->fs->mknod(mount, path, mode, dev);
    mount_release(mount);
    return err;
}

int generic_setattrat(struct fd *at, const char *path_raw, struct attr attr, bool follow_links) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, follow_links ? N_SYMLINK_FOLLOW : N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    struct statbuf stat;
    err = mount->fs->stat(mount, path, &stat);
    if (err >= 0) err = setattr_check(&stat, &attr);
    if (err >= 0) err = mount->fs->setattr ? mount->fs->setattr(mount, path, attr) : _EPERM;
    mount_release(mount);
    return err;
}

int generic_utime(struct fd *at, const char *path_raw, struct timespec atime, struct timespec mtime, bool follow_links) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, follow_links ? N_SYMLINK_FOLLOW : N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    err = _EPERM;
    if (mount->fs->utime)
        err = mount->fs->utime(mount, path, atime, mtime);
    mount_release(mount);
    return err;
}

ssize_t generic_readlinkat(struct fd *at, const char *path_raw, char *buf, size_t bufsize) {
    char path[MAX_PATH];
    int err;
    if (path_raw[0] == '\0') {
        // readlinkat(fd, "") reads the link an O_PATH|O_NOFOLLOW fd names
        // (Linux 2.6.39); any other descriptor has no link here: ENOENT.
        if (at == AT_PWD || !S_ISLNK(at->type))
            return _ENOENT;
        err = generic_getpath(at, path);
    } else {
        err = path_normalize(at, path_raw, path, N_SYMLINK_NOFOLLOW);
    }
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    err = _EINVAL;
    if (mount->fs->readlink)
        err = mount->fs->readlink(mount, path, buf, bufsize);
    mount_release(mount);
    return err;
}

int generic_mkdirat(struct fd *at, const char *path_raw, mode_t_ mode) {
    char path[MAX_PATH];
    // mkdir does not follow the final component. Linux reports EEXIST before
    // checking whether the caller may create in its parent. BusyBox mkdir -p
    // probes existing ancestors (including /) as an ordinary user.
    int err = path_normalize(at, path_raw, path, N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    struct mount *existing = find_mount_and_trim_path(path);
    struct statbuf st;
    err = existing->fs->stat(existing, path, &st);
    mount_release(existing);
    if (err == 0)
        return _EEXIST;
    if (err != _ENOENT)
        return err;
    err = path_normalize(at, path_raw, path, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    err = _EPERM;
    if (mount->fs->mkdir)
        err = mount->fs->mkdir(mount, path, mode);
    mount_release(mount);
    return err;
}

int generic_rmdirat(struct fd *at, const char *path_raw) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, N_SYMLINK_FOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    if (contains_mount_point(path))
        return _EBUSY;
    struct mount *mount = find_mount_and_trim_path(path);
    err = _EPERM;
    if (mount->fs->rmdir)
        err = mount->fs->rmdir(mount, path);
    mount_release(mount);
    return err;
}

int generic_seek(struct fd *fd, off_t_ off, int whence, size_t size) {
    off_t_ new_off = fd->offset;
    if (whence == LSEEK_SET) {
        fd->offset = off;
    } else if (whence == LSEEK_CUR) {
        if (__builtin_add_overflow(new_off, off, &new_off) || new_off < 0)
            return _EINVAL;
        fd->offset = new_off;
    } else if (whence == LSEEK_END) {
        new_off = size + off;
        if (new_off < 0)
            return _EINVAL;
        fd->offset = new_off;
    } else {
        return _EINVAL;
    }
    return 0;
}
