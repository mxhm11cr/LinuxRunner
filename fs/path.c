#include <string.h>
#include <sys/stat.h>
#include <stdbool.h>
#include "kernel/calls.h"
#include "fs/path.h"
#include "fs/fd.h"
#include "misc.h"

static int directory_getpath(struct fd *fd, char *path) {
    // Relative *at walks require a directory, including for ".". Anonymous
    // descriptors expose diagnostic names (pipe:[...], unknown:[...]), not
    // filesystem paths. Reject them before normalizing or asserting on a name.
    // Guest-opened directories cache their immutable type, avoiding extra VFS
    // reads on the normal path; imported descriptors may need an actual fstat.
    if (fd->type != S_IFDIR) {
        struct statbuf stat;
        int err = fd->mount->fs->fstat(fd, &stat);
        if (err < 0) return err;
        if (!S_ISDIR(stat.mode)) return _ENOTDIR;
    }
    return generic_getpath(fd, path);
}

static int __path_normalize(const char *at_path, const char *path, char *out,
        int flags, int levels, const char *root_path, struct fd **target) {
    // you must choose one
    if (flags & N_SYMLINK_FOLLOW)
        assert(!(flags & N_SYMLINK_NOFOLLOW));
    else
        assert(flags & N_SYMLINK_NOFOLLOW);

    const char *p = path;
    char *o = out;
    *o = '\0';
    int n = MAX_PATH - 1;

    if (strcmp(path, "") == 0)
        return _ENOENT;

    if (at_path != NULL && strcmp(at_path, "/") != 0) {
        size_t at_len = strlen(at_path);
        if (at_len >= MAX_PATH)
            return _ENAMETOOLONG;
        memcpy(o, at_path, at_len + 1);
        n -= at_len;
        o += at_len;
    }

    while (*p == '/')
        p++;

    while (*p != '\0') {
        if (p[0] == '.') {
            if (p[1] == '\0' || p[1] == '/') {
                // single dot path component, ignore
                p++;
                while (*p == '/')
                    p++;
                continue;
            } else if (p[1] == '.' && (p[2] == '\0' || p[2] == '/')) {
                // double dot path component, delete the last component
                size_t floor = root_path ? strlen(root_path) : 0;
                if ((size_t)(o - out) > floor) {
                    do {
                        o--;
                        n++;
                    } while (*o != '/');
                }
                p += 2;
                while (*p == '/')
                    p++;
                continue;
            }
        }

        // Reserve the slash and terminating NUL (OpenMinis a4b5d7e3).
        if (n <= 1)
            return _ENAMETOOLONG;
        *o++ = '/'; n--;
        char *c = o;
        // copy up to a slash or null
        while (*p != '/' && *p != '\0' && --n > 0)
            *o++ = *p++;
        // eat any slashes
        while (*p == '/')
            p++;

        if (n <= 0)
            return _ENAMETOOLONG;

        if ((flags & N_SYMLINK_FOLLOW) || *p != '\0') {
            // this buffer is used to store the path that we're readlinking, then
            // if it turns out to point to a symlink it's reused as the buffer
            // passed to the next path_normalize call
            char possible_symlink[MAX_PATH];
            *o = '\0';
            strcpy(possible_symlink, out);
            struct mount *mount = find_mount_and_trim_path(possible_symlink);
            assert(path_is_normalized(possible_symlink));
            if (target && *p == '\0' && mount->fs->follow_link) {
                struct fd *fd = mount->fs->follow_link(mount, possible_symlink, flags & N_STAT_TARGET);
                if (fd) {
                    mount_release(mount);
                    if (IS_ERR(fd)) return PTR_ERR(fd);
                    if (*(p - 1) == '/' && fd->type != S_IFDIR) { fd_close(fd); return _ENOTDIR; }
                    *target = fd;
                    return 0;
                }
            }
            int res = _EINVAL;
            if (mount->fs->readlink)
                res = mount->fs->readlink(mount, possible_symlink, c, MAX_PATH - (c - out));
            if (res >= 0) {
                mount_release(mount);
                if (levels >= 5)
                    return _ELOOP;
                // readlink does not null terminate. A full-buffer result leaves
                // no room for the terminator in out/possible_symlink.
                size_t link_room = MAX_PATH - (size_t)(c - out);
                if ((size_t)res >= link_room)
                    return _ENAMETOOLONG;
                c[res] = '\0';
                // if we should restart from the root, copy down
                if (*c == '/') {
                    if (root_path && *root_path) {
                        size_t root_len = strlen(root_path), link_len = strlen(c);
                        if (root_len + link_len >= MAX_PATH) return _ENAMETOOLONG;
                        // c points inside out; move the link before the prefix.
                        memmove(out + root_len, c, link_len + 1);
                        memcpy(out, root_path, root_len);
                    } else {
                        memmove(out, c, strlen(c) + 1);
                    }
                }
                size_t out_len = strlen(out);
                size_t rest_len = strlen(p);
                bool have_rest = rest_len != 0;
                if (out_len + (have_rest ? 1 + rest_len : 0) >= MAX_PATH)
                    return _ENAMETOOLONG;
                char *expanded_path = possible_symlink;
                memcpy(expanded_path, out, out_len + 1);
                if (have_rest) {
                    expanded_path[out_len] = '/';
                    memcpy(expanded_path + out_len + 1, p, rest_len + 1);
                }
                return __path_normalize(NULL, expanded_path, out, flags, levels + 1, root_path, target);
            }

            // if there's a slash after this component, ensure that if it
            // exists, it's a directory and that we have execute perms on it
            if (*(p - 1) == '/') {
                struct statbuf stat;
                int err = mount->fs->stat(mount, possible_symlink, &stat);
                if (err >= 0) {
                    if (!S_ISDIR(stat.mode)) err = _ENOTDIR;
                    else err = mount_access_check(mount, possible_symlink, &stat, AC_X);
                    mount_release(mount);
                    if (err < 0) return err;
                } else mount_release(mount);
            } else {
                mount_release(mount);
            }
        }
    }

    *o = '\0';
    assert(path_is_normalized(out));
    if (flags & N_PARENT_DIR_WRITE) {
        char parent[MAX_PATH];
        strcpy(parent, out);
        char *slash = strrchr(parent, '/');
        if (slash) *slash = '\0';
        struct mount *mount = find_mount_and_trim_path(parent);
        struct statbuf stat;
        int err = mount->fs->stat(mount, parent, &stat);
        if (!err) {
            if (!S_ISDIR(stat.mode)) err = _ENOTDIR;
            else err = mount_access_check(mount, parent, &stat, AC_W | AC_X);
        }
        mount_release(mount);
        if (err < 0) return err;
    }
    return 0;
}

int path_normalize_target(struct fd *at, const char *path, char *out, int flags, struct fd **target) {
    if (target) *target = NULL;
    if (strcmp(path, "") == 0)
        return _ENOENT;

    // start with root or cwd, depending on whether it starts with a slash
    lock(&current->fs->lock);
    if (path[0] == '/')
        at = current->fs->root;
    else if (at == AT_PWD)
        at = current->fs->pwd;
    unlock(&current->fs->lock);
    if (at == NULL && path[0] != '/')
        return _EBADF;
    if (at != NULL && IS_ERR(at))
        return PTR_ERR(at);

    char at_path[MAX_PATH];
    if (at != NULL) {
        int err = directory_getpath(at, at_path);
        if (err < 0)
            return err;
        assert(path_is_normalized(at_path));
    }

    return __path_normalize(at != NULL ? at_path : NULL, path, out, flags, 0, NULL, target);
}

int path_normalize(struct fd *at, const char *path, char *out, int flags) {
    return path_normalize_target(at, path, out, flags, NULL);
}

int path_normalize_in_fs(struct fs_info *fs, struct fd *at,
        const char *path, char *out, int flags) {
    if (!fs || !path || !*path) return _ENOENT;
    // Context callers hold a private retained snapshot; never swap current->fs.
    struct fd *root = fs->root;
    if (path[0] == '/') at = root;
    else if (at == AT_PWD) at = fs->pwd;
    if (!root || !at || IS_ERR(root) || IS_ERR(at)) return _EBADF;
    char root_path[MAX_PATH], at_path[MAX_PATH];
    int err = directory_getpath(root, root_path);
    if (err < 0) return err;
    err = directory_getpath(at, at_path);
    if (err < 0) return err;
    if (!strcmp(root_path, "/")) root_path[0] = '\0';
    size_t n = strlen(root_path);
    if (strncmp(at_path, root_path, n) || (at_path[n] && at_path[n] != '/'))
        return _EXDEV;
    return __path_normalize(at_path, path, out, flags, 0, root_path, NULL);
}


bool path_is_normalized(const char *path) {
    while (*path != '\0') {
        if (*path != '/')
            return false;
        path++;
        if (*path == '/')
            return false;
        while (*path != '/' && *path != '\0')
            path++;
    }
    return true;
}

bool path_next_component(const char **path, char *component, int *err) {
    const char *p = *path;
    if (*p == '\0')
        return false;

    assert(*p == '/');
    p++;
    char *c = component;
    while (*p != '/' && *p != '\0') {
        *c++ = *p++;
        if (c - component >= MAX_NAME) {
            *err = _ENAMETOOLONG;
            return false;
        }
    }
    *c = '\0';
    *path = p;
    return true;
}
