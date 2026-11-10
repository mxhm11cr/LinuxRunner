// Linux file capabilities are guest metadata and guest credentials only. Host
// xattrs use a private name: never install host file capabilities or entitlements.
#include <string.h>
#include <errno.h>
#include "platform/guest_file_caps.h"
#include "kernel/calls.h"
#include "kernel/task.h"
#include "kernel/filecaps.h"
#include "fs/fd.h"
#include "fs/path.h"

#define CAPS_NAME "security.capability"
#define CAPS_MAX 24

static int host_error(void) {
#ifdef ENOATTR
    if (errno == ENOATTR) return _ENODATA;
#endif
    return errno_map();
}
ssize_t file_caps_host_get(int fd, void *value, size_t size) {
    ssize_t n = lr_guest_caps_get(fd, value, size);
    return n < 0 ? host_error() : n;
}
int file_caps_host_set(int fd, const void *value, size_t size, int flags) {
    int native = (flags & 1 ? XATTR_CREATE : 0) | (flags & 2 ? XATTR_REPLACE : 0);
    return lr_guest_caps_set(fd, value, size, native) < 0 ? host_error() : 0;
}
int file_caps_host_remove(int fd) {
    return lr_guest_caps_remove(fd) < 0 ? host_error() : 0;
}
int file_caps_strip(struct fd *fd) {
    if (!fd->mount || !fd->mount->fs->remove_file_caps || !S_ISREG(fd->type)) return 0;
    int err = fd->mount->fs->remove_file_caps(fd);
    return err == _ENODATA || err == _EOPNOTSUPP ? 0 : err;
}
// Linux VFS_CAP_REVISION_1/2, and revision 3 with initial-namespace rootid 0.
// Namespaced root IDs require a separate ownership translation implementation.
int file_caps_decode(const void *value, size_t size, struct file_caps *caps) {
    uint32_t w[6] = {0}; *caps = (struct file_caps){0};
    int validation = lr_guest_caps_validate(value, size);
    if (validation) return validation == -2 ? _EOPNOTSUPP : _EINVAL;
    memcpy(w, value, size);
    unsigned rev = w[0] >> 24;
    caps->permitted = w[1] | (uint64_t)(rev >= 2 ? w[3] : 0) << 32;
    caps->inheritable = w[2] | (uint64_t)(rev >= 2 ? w[4] : 0) << 32;
    if ((caps->permitted | caps->inheritable) & ~CAP_FULL_SET_) return _EINVAL;
    caps->effective = w[0] & 1; caps->present = true;
    return 0;
}
int file_caps_read(struct fd *fd, struct file_caps *caps) {
    *caps = (struct file_caps){0};
    if (!fd->mount->fs->get_file_caps || !S_ISREG(fd->type)) return 0;
    uint8_t value[CAPS_MAX];
    ssize_t n = fd->mount->fs->get_file_caps(fd, value, sizeof value);
    if (n == _ENODATA || n == _EOPNOTSUPP) return 0;
    if (n < 0) return n;
    return file_caps_decode(value, n, caps);
}
static int cap_name(addr_t address) {
    char name[256];
    if (user_read_string(address, name, sizeof name)) return _EFAULT;
    if (!name[0]) return _ERANGE;
    return !strcmp(name, CAPS_NAME) ? 0 : _EOPNOTSUPP;
}
enum xop { XGET, XSET, XLIST, XREMOVE };
static dword_t caps_op(struct fd *fd, enum xop op, addr_t name,
        addr_t value, dword_t size, dword_t flags) {
    if (op != XLIST) { int err = cap_name(name); if (err) return err; }
    if (flags & ~3u || flags == 3) return _EINVAL;
    if (!fd->mount->fs->get_file_caps) return _EOPNOTSUPP;
    if (!S_ISREG(fd->type)) return op == XSET ? _EINVAL : (op == XLIST ? 0 : _ENODATA);
    bool modifying = op == XSET || op == XREMOVE;
    if (modifying) {
        if (fd->mount->flags & MS_READONLY_) return _EROFS;
        // File caps apply only to the initial namespace for now. Never let a
        // nested userns root label an executable with initial-namespace grants.
        if (current->userns || !ns_capable(31)) return _EPERM; // CAP_SETFCAP
    }
    uint8_t bytes[CAPS_MAX];
    if (op == XSET) {
        if (size > sizeof bytes) return _EINVAL;
        if (user_read(value, bytes, size)) return _EFAULT;
        struct file_caps decoded;
        int err = file_caps_decode(bytes, size, &decoded); if (err) return err;
        if (size == 24) { // initial-namespace rootid 0 is represented as v2
            uint32_t magic; memcpy(&magic, bytes, 4);
            magic = 0x02000000u | (magic & 1); memcpy(bytes, &magic, 4); size = 20;
        }
        return fd->mount->fs->set_file_caps ? fd->mount->fs->set_file_caps(fd, bytes, size, flags) : _EOPNOTSUPP;
    }
    if (op == XREMOVE) return fd->mount->fs->remove_file_caps ? fd->mount->fs->remove_file_caps(fd) : _EOPNOTSUPP;
    ssize_t n = fd->mount->fs->get_file_caps(fd, bytes, sizeof bytes);
    if (op == XLIST) {
        if (n == _ENODATA) return 0;
        if (n < 0) return n;
        if (size && size < sizeof CAPS_NAME) return _ERANGE;
        return size && user_write(value, CAPS_NAME, sizeof CAPS_NAME) ? _EFAULT : sizeof CAPS_NAME;
    }
    if (n < 0) return n;
    if (size && size < (size_t)n) return _ERANGE;
    return size && user_write(value, bytes, n) ? _EFAULT : n;
}
static dword_t path_op(addr_t path_addr, bool follow, enum xop op,
        addr_t name, addr_t value, dword_t size, dword_t flags) {
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof path)) return _EFAULT;
    struct fd *fd = generic_open(path, O_PATH_ | (follow ? 0 : O_NOFOLLOW_), 0);
    if (IS_ERR(fd)) return PTR_ERR(fd);
    dword_t r = caps_op(fd, op, name, value, size, flags); fd_close(fd); return r;
}
static dword_t fd_op(fd_t number, enum xop op, addr_t name, addr_t value, dword_t size, dword_t flags) {
    struct fd *fd = f_get_retain(number); if (!fd) return _EBADF;
    dword_t r = fd_is_path_only(fd) ? _EBADF : caps_op(fd, op, name, value, size, flags);
    fd_close(fd); return r;
}
dword_t sys_setxattr(addr_t p,addr_t n,addr_t v,dword_t s,dword_t f){return path_op(p,true,XSET,n,v,s,f);}
dword_t sys_lsetxattr(addr_t p,addr_t n,addr_t v,dword_t s,dword_t f){return path_op(p,false,XSET,n,v,s,f);}
dword_t sys_fsetxattr(fd_t p,addr_t n,addr_t v,dword_t s,dword_t f){return fd_op(p,XSET,n,v,s,f);}
dword_t sys_getxattr(addr_t p,addr_t n,addr_t v,dword_t s){return path_op(p,true,XGET,n,v,s,0);}
dword_t sys_lgetxattr(addr_t p,addr_t n,addr_t v,dword_t s){return path_op(p,false,XGET,n,v,s,0);}
dword_t sys_fgetxattr(fd_t p,addr_t n,addr_t v,dword_t s){return fd_op(p,XGET,n,v,s,0);}
dword_t sys_listxattr(addr_t p,addr_t v,dword_t s){return path_op(p,true,XLIST,0,v,s,0);}
dword_t sys_llistxattr(addr_t p,addr_t v,dword_t s){return path_op(p,false,XLIST,0,v,s,0);}
dword_t sys_flistxattr(fd_t p,addr_t v,dword_t s){return fd_op(p,XLIST,0,v,s,0);}
dword_t sys_removexattr(addr_t p,addr_t n){return path_op(p,true,XREMOVE,n,0,0,0);}
dword_t sys_lremovexattr(addr_t p,addr_t n){return path_op(p,false,XREMOVE,n,0,0,0);}
dword_t sys_fremovexattr(fd_t p,addr_t n){return fd_op(p,XREMOVE,n,0,0,0);}
