#ifdef __APPLE__
#define __APPLE_USE_RFC_3542 1
#endif
#include <fcntl.h>
#include <ifaddrs.h>
#include <sys/ioctl.h>
#include <netinet/tcp.h>
#include <net/if.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include "kernel/calls.h"
#include "fs/fd.h"
#include "fs/inode.h"
#include "fs/path.h"
#include "fs/real.h"
#include "fs/sock.h"
#include "fs/poll.h"
#include "debug.h"

#define SOCKET_TYPE_MASK 0xf

const struct fd_ops socket_fdops;
extern volatile bool g_trace_syscalls;

// Opt-in, read-only route-netlink compatibility. One native readiness token
// represents a bounded reply queue. Enqueue whole snapshots atomically: a
// full native datagram queue must not leave half a dump after a failed send.
#include "fs/netlink.h"
#include "fs/uevent.h"

bool ish_netlink_stub_enabled(void) {
    const char *v = getenv("ISH_NETLINK_STUB");
    return v != NULL && strcmp(v, "1") == 0;
}

// ISH_GUEST_LISTEN=loopback: a guest bind to the IPv4/IPv6 wildcard address
// is made on the matching loopback address instead, so guest servers (for
// example a session's RFB server) are never reachable from the device's
// networks. Explicit addresses and other families are unchanged.
static bool guest_listen_loopback(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *v = getenv("ISH_GUEST_LISTEN");
        enabled = v != NULL && strcmp(v, "loopback") == 0;
    }
    return enabled;
}
static void guest_bind_wildcard_to_loopback(void *sockaddr) {
    if (!guest_listen_loopback())
        return;
    struct sockaddr *addr = sockaddr;
    if (addr->sa_family == PF_INET) {
        struct sockaddr_in *in = sockaddr;
        if (in->sin_addr.s_addr == htonl(INADDR_ANY))
            in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else if (addr->sa_family == PF_INET6) {
        struct sockaddr_in6 *in6 = sockaddr;
        if (IN6_IS_ADDR_UNSPECIFIED(&in6->sin6_addr))
            in6->sin6_addr = in6addr_loopback;
    }
}

static lock_t netlink_lock = LOCK_INITIALIZER;
static struct list netlink_sockets = LIST_INITIALIZER(netlink_sockets);
static uint32_t netlink_next_port = 0x80000000;
struct lr_netlink_replies {
    struct lr_netlink_replies *next;
    size_t size, offset;
    char data[];
};
#define NETLINK_REPLY_LIMIT (256 * 1024)
static int sock_wait_readable(int hostfd);
static ssize_t unix_credential_send(struct fd *, const void *, size_t, int, bool *);
static ssize_t unix_plain_recv(struct fd *, void *, size_t, int, void *, socklen_t *);
static ssize_t unix_stream_send_fitting(struct fd *, struct msghdr *, int, bool);
static bool unix_send_may_block(struct fd *, int);

static bool fd_is_netlink_stub(struct fd *fd) {
    return fd != NULL && fd->ops == &socket_fdops && fd->socket.domain == AF_NETLINK_ && fd->socket.protocol == NETLINK_ROUTE_;
}
static bool netlink_port_used(uint32_t port, struct fd *except) {
    struct fd *fd;
    list_for_each_entry(&netlink_sockets, fd, socket.netlink_link)
        if (fd != except && fd->socket.netlink_port == port) return true;
    return false;
}
static int netlink_name(struct fd *fd, addr_t addr, uint32_t capacity) {
    lock(&netlink_lock);
    struct sockaddr_nl_ nl = { .family = AF_NETLINK_,
        .pid = fd->socket.netlink_port, .groups = fd->socket.netlink_groups };
    unlock(&netlink_lock);
    if (capacity > sizeof(nl)) capacity = sizeof(nl);
    return capacity && user_write(addr, &nl, capacity) ? _EFAULT : 0;
}
static int netlink_bind(struct fd *fd, addr_t addr, uint32_t size) {
    struct sockaddr_nl_ nl;
    if (size < sizeof(nl)) return _EINVAL;
    if (user_get(addr, nl)) return _EFAULT;
    if (nl.family != AF_NETLINK_) return _EINVAL;
    lock(&netlink_lock);
    int error = 0;
    if (fd->socket.netlink_bound && nl.pid && nl.pid != fd->socket.netlink_port)
        error = _EINVAL;
    else if (nl.pid && netlink_port_used(nl.pid, fd)) error = _EADDRINUSE;
    else {
        if (nl.pid) fd->socket.netlink_port = nl.pid;
        fd->socket.netlink_bound = true;
        fd->socket.netlink_groups = nl.groups;
    }
    unlock(&netlink_lock);
    // Group subscriptions are recorded but no multicast events are generated.
    return error;
}
static int netlink_destination(addr_t addr, uint32_t size) {
    if (!addr) return 0; // Implicit kernel destination.
    struct sockaddr_nl_ nl;
    if (size < sizeof(nl)) return _EINVAL;
    if (user_get(addr, nl)) return _EFAULT;
    if (nl.family != AF_NETLINK_) return _EINVAL;
    return nl.pid || nl.groups ? _EPERM : 0;
}
static int netlink_stub_reply(struct fd *fd, const void *req, size_t size) {
    if (size > NETLINK_BUFFER_SIZE) return _EMSGSIZE;
    char *reply = malloc(NETLINK_SNAPSHOT_SIZE);
    if (!reply) return _ENOMEM;
    lock(&netlink_lock);
    fd->socket.netlink_bound = true;
    uint32_t port = fd->socket.netlink_port;
    unlock(&netlink_lock);
    int len = netlink_snapshot(req, size, port, reply, NETLINK_SNAPSHOT_SIZE);
    if (len < 0) { free(reply); return len; }
    struct lr_netlink_replies *entry = malloc(sizeof(*entry) + len);
    if (!entry) { free(reply); return _ENOMEM; }
    *entry = (struct lr_netlink_replies){.size = len};
    memcpy(entry->data, reply, len);
    free(reply);
    lock(&netlink_lock);
    int result = size;
    if ((size_t)len > NETLINK_REPLY_LIMIT - fd->socket.netlink_reply_bytes) result = _EAGAIN;
    else if (!fd->socket.netlink_replies &&
             send(fd->socket.netlink_peer_fd, "n", 1, MSG_DONTWAIT) != 1)
        result = errno == ENOBUFS ? _EAGAIN : errno_map();
    if (result >= 0) {
        if (fd->socket.netlink_replies_tail) fd->socket.netlink_replies_tail->next = entry;
        else fd->socket.netlink_replies = entry;
        fd->socket.netlink_replies_tail = entry;
        fd->socket.netlink_reply_bytes += len;
    } else free(entry);
    unlock(&netlink_lock);
    return result;
}
static int netlink_receive(struct fd *fd, void *buf, size_t size, int flags) {
    if (flags & ~(MSG_PEEK_ | MSG_DONTWAIT_ | MSG_TRUNC_)) return _EOPNOTSUPP;
    for (;;) {
        lock(&netlink_lock);
        struct lr_netlink_replies *entry = fd->socket.netlink_replies;
        if (entry) {
            size_t chunk = 0;
            // The encoder only produces validated, aligned complete messages.
            while (entry->offset + chunk < entry->size) {
                uint32_t message;
                memcpy(&message, entry->data + entry->offset + chunk, sizeof(message));
                size_t aligned = ((size_t)message + 3) & ~(size_t)3;
                if (chunk + aligned > NETLINK_BUFFER_SIZE) break;
                chunk += aligned;
            }
            size_t copied = size < chunk ? size : chunk;
            if (copied) memcpy(buf, entry->data + entry->offset, copied);
            if (!(flags & MSG_PEEK_)) {
                entry->offset += chunk;
                if (entry->offset == entry->size) {
                    fd->socket.netlink_replies = entry->next;
                    fd->socket.netlink_reply_bytes -= entry->size;
                    free(entry);
                    if (!fd->socket.netlink_replies) {
                        char token; (void)recv(fd->real_fd, &token, 1, MSG_DONTWAIT);
                        fd->socket.netlink_replies_tail = NULL;
                    }
                }
            }
            unlock(&netlink_lock);
            return (flags & MSG_TRUNC_) ? chunk : copied;
        }
        unlock(&netlink_lock);
        if ((flags & MSG_DONTWAIT_) || (fd_getflags(fd) & O_NONBLOCK_)) return _EAGAIN;
        int err = sock_wait_readable(fd->real_fd);
        if (err < 0) return err;
    }
}
struct netlink_iov { addr_t base; size_t len; };
static int netlink_iovs(addr_t addr, uint64_t count, struct netlink_iov *iov, size_t *size) {
    if (count > 1024) return _EMSGSIZE;
    *size = 0;
    for (size_t i = 0; i < count; i++) {
#ifdef GUEST_ARM64
        struct iovec64_ iv;
#else
        struct iovec_ iv;
#endif
        if (user_get(addr + i * sizeof(iv), iv)) return _EFAULT;
        if (iv.len > SIZE_MAX - *size) return _EMSGSIZE;
        iov[i] = (struct netlink_iov) {(addr_t) iv.base, iv.len};
        *size += iv.len;
    }
    return 0;
}
static int netlink_stub_sendmsg(struct fd *sock, addr_t addr, int flags) {
    if (flags & ~(MSG_DONTWAIT_ | MSG_NOSIGNAL_)) return _EOPNOTSUPP;
#ifdef GUEST_ARM64
    struct msghdr64_ m;
#else
    struct msghdr_ m;
#endif
    if (user_get(addr, m)) return _EFAULT;
    if (m.msg_controllen) return _EOPNOTSUPP;
    int err = netlink_destination(m.msg_name, m.msg_namelen);
    if (err < 0) return err;
    struct netlink_iov iov[1024];
    size_t total;
    err = netlink_iovs(m.msg_iov, m.msg_iovlen, iov, &total);
    if (err < 0) return err;
    if (total > NETLINK_BUFFER_SIZE) return _EMSGSIZE;
    char buf[NETLINK_BUFFER_SIZE];
    size_t off = 0;
    for (size_t i = 0; i < m.msg_iovlen; i++) {
        if (iov[i].len && user_read(iov[i].base, buf + off, iov[i].len)) return _EFAULT;
        off += iov[i].len;
    }
    return netlink_stub_reply(sock, buf, total);
}
static int netlink_stub_recvmsg(struct fd *sock, addr_t addr, int flags) {
#ifdef GUEST_ARM64
    struct msghdr64_ m;
#else
    struct msghdr_ m;
#endif
    if (user_get(addr, m)) return _EFAULT;
    struct netlink_iov iov[1024];
    size_t capacity;
    int err = netlink_iovs(m.msg_iov, m.msg_iovlen, iov, &capacity);
    if (err < 0) return err;
    char buf[NETLINK_BUFFER_SIZE];
    int n = netlink_receive(sock, buf, sizeof(buf), flags);
    if (n < 0) return n;
    size_t copied = 0;
    for (size_t i = 0; i < m.msg_iovlen && copied < (size_t) n; i++) {
        size_t len = iov[i].len;
        if (len > (size_t) n - copied) len = n - copied;
        if (len && user_write(iov[i].base, buf + copied, len)) return _EFAULT;
        copied += len;
    }
    if (m.msg_name) {
        struct sockaddr_nl_ sender = {.family = AF_NETLINK_};
        size_t len = m.msg_namelen < sizeof(sender) ? m.msg_namelen : sizeof(sender);
        if (len && user_write(m.msg_name, &sender, len)) return _EFAULT;
        m.msg_namelen = sizeof(sender);
    } else m.msg_namelen = 0;
    m.msg_controllen = 0;
    m.msg_flags = capacity < (size_t) n ? MSG_TRUNC_ : 0;
    if (user_put(addr, m)) return _EFAULT;
    return flags & MSG_TRUNC_ ? n : (int) copied;
}
static int netlink_recvfrom(struct fd *sock, addr_t buf_addr, size_t size, int flags,
                            addr_t name, addr_t len_addr) {
    uint32_t capacity = 0;
    if (name && (!len_addr || user_get(len_addr, capacity))) return _EFAULT;
    char buf[NETLINK_BUFFER_SIZE];
    int n = netlink_receive(sock, buf, sizeof(buf), flags);
    if (n < 0) return n;
    size_t copied = size < (size_t) n ? size : (size_t) n;
    if (copied && user_write(buf_addr, buf, copied)) return _EFAULT;
    if (name) {
        struct sockaddr_nl_ sender = {.family = AF_NETLINK_};
        size_t len = capacity < sizeof(sender) ? capacity : sizeof(sender);
        if (len && user_write(name, &sender, len)) return _EFAULT;
        capacity = sizeof(sender);
        if (user_put(len_addr, capacity)) return _EFAULT;
    }
    return flags & MSG_TRUNC_ ? n : (int) copied;
}
static fd_t netlink_stub_socket(dword_t type, dword_t protocol) {
    if (protocol != NETLINK_ROUTE_) return _EPROTONOSUPPORT;
    if ((type & SOCKET_TYPE_MASK) != SOCK_RAW_ && (type & SOCKET_TYPE_MASK) != SOCK_DGRAM_)
        return _ESOCKTNOSUPPORT;
    if (type & ~(SOCKET_TYPE_MASK | SOCK_NONBLOCK_ | SOCK_CLOEXEC_)) return _EINVAL;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) return errno_map();
    // Room for a whole multi-datagram dump in the receive queue.
    int datagram = NETLINK_BUFFER_SIZE * 2, queue = NETLINK_SNAPSHOT_SIZE * 4;
    setsockopt(sv[1], SOL_SOCKET, SO_SNDBUF, &datagram, sizeof(datagram));
    setsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &queue, sizeof(queue));
    struct fd *fd = adhoc_fd_create(&socket_fdops);
    if (fd == NULL) { close(sv[0]); close(sv[1]); return _ENOMEM; }
    fd->stat.mode = S_IFSOCK | 0666;
    fd->real_fd = sv[0];
    fd->socket.domain = AF_NETLINK_;
    fd->socket.type = type & SOCKET_TYPE_MASK;
    fd->socket.protocol = protocol;
    fd->socket.netlink_peer_fd = sv[1];
    lock(&netlink_lock);
    do { netlink_next_port++; } while (!netlink_next_port || netlink_port_used(netlink_next_port, NULL));
    fd->socket.netlink_port = netlink_next_port;
    list_add(&netlink_sockets, &fd->socket.netlink_link);
    unlock(&netlink_lock);
    // f_install closes fd (including both socketpair ends) on failure.
    return f_install(fd, type & ~SOCKET_TYPE_MASK);
}

static lock_t peer_lock = LOCK_INITIALIZER;

static fd_t sock_fd_create(int sock_fd, int domain, int type, int protocol, struct fd **retained) {
    if (retained) *retained = NULL;
    struct fd *fd = adhoc_fd_create(&socket_fdops);
    if (fd == NULL) {
        close(sock_fd);
        return _ENOMEM;
    }
    // Ownership of sock_fd is consumed on every path. f_install releases the
    // complete description (and its native socket) if the guest table is full.
    fd->stat.mode = S_IFSOCK | 0666;
    fd->real_fd = sock_fd;
    fd->socket.domain = domain;
    fd->socket.type = type & SOCKET_TYPE_MASK;
    fd->socket.protocol = protocol;
    fd->socket.netlink_peer_fd = -1;
    if (domain == AF_LOCAL_) {
        cond_init(&fd->socket.unix_got_peer);
        lock_init(&fd->socket.unix_send_lock);
        list_init(&fd->socket.unix_scm);
    }
    // Pin before publication: another CLONE_FILES thread can close/reuse the
    // installed guest slot as soon as f_install drops the table lock.
    if (retained) fd_retain(fd);
    fd_t installed = f_install(fd, type & ~SOCKET_TYPE_MASK);
    if (retained) {
        if (installed >= 0) *retained = fd;
        else fd_close(fd);
    }
    static _Thread_local unsigned trace_created;
    if (g_trace_syscalls && domain == AF_LOCAL_ && trace_created++ < 128)
        printk("UNIX_FD_CREATE tid=%d guest=%d native=%d kind=%d\n", current->pid, installed, sock_fd, type & SOCKET_TYPE_MASK);
    return installed;
}

int_t sys_socket(dword_t domain, dword_t type, dword_t protocol) {
    STRACE("socket(%d, %d, %d)", domain, type, protocol);
    // Guest device-event IPC is independent of the optional host-route view.
    if (domain == AF_NETLINK_ && protocol == NETLINK_KOBJECT_UEVENT_)
        return uevent_create(type);
    if (domain == AF_NETLINK_ && ish_netlink_stub_enabled())
        return netlink_stub_socket(type, protocol);
    int real_domain = sock_family_to_real(domain);
    if (real_domain < 0)
        return _EAFNOSUPPORT;
    int real_type = sock_type_to_real(type, protocol);
    if (real_type < 0)
        return _EINVAL;

    // this hack makes mtr work
    if (type == SOCK_RAW_ && protocol == IPPROTO_RAW)
        protocol = IPPROTO_ICMP;

    int sock = socket(real_domain, real_type, protocol);
    if (sock < 0)
        return errno_map();

#ifdef __APPLE__
    if (domain == AF_INET_ && type == SOCK_DGRAM_) {
        // in some cases, such as ICMP, datagram sockets on mac can default to
        // including the IP header like raw sockets
        int one = 1;
        setsockopt(sock, IPPROTO_IP, IP_STRIPHDR, &one, sizeof(one));
    }
#endif

    return sock_fd_create(sock, domain, type, protocol, NULL);
}

static void inode_release_if_exist(struct inode_data *inode) {
    if (inode != NULL)
        inode_release(inode);
}

static struct fd *sock_getfd(fd_t sock_fd) {
    struct fd *sock = f_get(sock_fd);
    if (sock == NULL || sock->ops != &socket_fdops)
        return NULL;
    return sock;
}
// Linux: a valid descriptor that is not a socket is ENOTSOCK, not EBADF.
// Callers such as PulseAudio's pa_write() try send() and fall back to write()
// only on ENOTSOCK.
static int sock_getfd_error(fd_t sock_fd) {
    return f_get(sock_fd) != NULL ? _ENOTSOCK : _EBADF;
}

// A syscall can sleep after another guest thread closes/reuses the slot.
// Hold the open description, not the descriptor-table slot, across that wait.
static struct fd *sock_getfd_retain(fd_t sock_fd, int *error) {
    *error = _EBADF;
    struct fd *sock = f_get_retain(sock_fd);
    if (sock != NULL && sock->ops != &socket_fdops) {
        fd_close(sock);
        sock = NULL;
        *error = _ENOTSOCK;
    }
    return sock;
}
// Pin an open description through all early returns and waits. In particular a
// sibling CLONE_FILES close must not free a blocked netlink receiver.
static void sock_release_scoped(struct fd **fd) { if (*fd) fd_close(*fd); }


static uint32_t unix_socket_next_id() {
    static uint32_t next_id = 0;
    static lock_t next_id_lock = LOCK_INITIALIZER;
    lock(&next_id_lock);
    uint32_t id = ++next_id;
    unlock(&next_id_lock);
    return id;
}

static int unix_socket_get(const char *path_raw, struct fd *bind_fd, uint32_t *socket_id) {
    char path[MAX_PATH];
    int err = path_normalize(AT_PWD, path_raw, path, N_SYMLINK_FOLLOW);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    struct statbuf stat;
    err = mount->fs->stat(mount, path, &stat);

    // If bind was called, there are some funny semantics.
    if (bind_fd != NULL) {
        // If the file exists, fail.
        if (err == 0) {
            err = _EADDRINUSE;
            goto out;
        }
        // If the file can't be found, try to create it as a socket.
        if (err < 0) {
            mode_t_ mode = 0777;
            struct fs_info *fs = current->fs;
            lock(&fs->lock);
            mode &= ~fs->umask;
            unlock(&fs->lock);
            err = mount->fs->mknod(mount, path, S_IFSOCK | mode, 0);
            if (err < 0)
                goto out;
            err = mount->fs->stat(mount, path, &stat);
            if (err < 0)
                goto out;
        }
    }

    // If something other than bind was called, just do the obvious thing and
    // fail if stat failed.
    if (bind_fd == NULL && err < 0)
        goto out;

    if (!S_ISSOCK(stat.mode)) {
        err = _ENOTSOCK;
        goto out;
    }

    // Look up the socket ID for the inode number.
    struct inode_data *inode = inode_get(mount, stat.inode);
    if (inode == NULL) {
        err = _ENOMEM;
        goto out;
    }
    lock(&inode->lock);
    if (inode->socket_id == 0)
        inode->socket_id = unix_socket_next_id();
    unlock(&inode->lock);
    *socket_id = inode->socket_id;

    mount_release(mount);
    if (bind_fd != NULL)
        bind_fd->socket.unix_name_inode = inode;
    else
        inode_release(inode);
    return 0;

out:
    mount_release(mount);
    return err;
}

// Dan Bernstein's simple and decently effective hash function
static uint32_t str_hash(const char *str) {
    uint32_t hash = 5381;
    for (int i = 0; str[i] != '\0'; i++) {
        hash = 33 * hash ^ str[i];
    }
    return hash;
}

// The abstract socket namespace is a lot simpler than it sounds: if the first
// byte of the path is a null byte, then it gets looked up in this hashtable
// instead of the filesystem.

struct unix_abstract {
    unsigned refcount;
    uint32_t hash;
    uint32_t socket_id;
    struct list links;
};
#define ABSTRACT_HASH_SIZE 1024
static struct list abstract_hash[ABSTRACT_HASH_SIZE];
static lock_t unix_abstract_lock = LOCK_INITIALIZER;

static int unix_abstract_get(const char *name, struct fd *bind_fd, uint32_t *socket_id) {
    uint32_t hash = str_hash(name);
    lock(&unix_abstract_lock);
    struct unix_abstract *sock_tmp;
    struct unix_abstract *sock = NULL;
    struct list *bucket = &abstract_hash[hash % ABSTRACT_HASH_SIZE];
    if (list_null(bucket))
        list_init(bucket);
    list_for_each_entry(bucket, sock_tmp, links) {
        if (sock_tmp->hash == hash) {
            sock = sock_tmp;
            break;
        }
    }

    if (bind_fd != NULL && sock != NULL) {
        unlock(&unix_abstract_lock);
        return _EEXIST;
    }
    if (bind_fd == NULL && sock == NULL) {
        unlock(&unix_abstract_lock);
        return _ENOENT;
    }

    if (sock == NULL) {
        sock = malloc(sizeof(struct unix_abstract));
        if (sock == NULL) {
            unlock(&unix_abstract_lock);
            return _ENOMEM;
        }
        sock->refcount = 0;
        sock->hash = hash;
        sock->socket_id = unix_socket_next_id();
        list_add(bucket, &sock->links);
    }

    sock->refcount++;
    unlock(&unix_abstract_lock);
    *socket_id = sock->socket_id;
    if (bind_fd != NULL)
        bind_fd->socket.unix_name_abstract = sock;
    return 0;
}

static void unix_abstract_release(struct unix_abstract *name) {
    lock(&unix_abstract_lock);
    if (--name->refcount == 0) {
        list_remove(&name->links);
        free(name);
    }
    unlock(&unix_abstract_lock);
}

const char *sock_tmp_prefix = "/tmp/ishsock";

static int sockaddr_read_bind(addr_t sockaddr_addr, void *sockaddr, uint_t *sockaddr_len, struct fd *bind_fd) {
    // Make sure we can read things without overflowing buffers
    if (*sockaddr_len < 2)
        return _EINVAL;
    if (*sockaddr_len > sizeof(struct sockaddr_max_))
        return _EINVAL;

    if (user_read(sockaddr_addr, sockaddr, *sockaddr_len))
        return _EFAULT;
    struct sockaddr *real_addr = sockaddr;
    struct sockaddr_ *fake_addr = sockaddr;
    real_addr->sa_family = sock_family_to_real(fake_addr->family);

    switch (real_addr->sa_family) {
        case PF_INET:
            if (*sockaddr_len < sizeof(struct sockaddr_in))
                return _EINVAL;
            break;
        case PF_INET6:
            if (*sockaddr_len < sizeof(struct sockaddr_in6))
                return _EINVAL;
            break;

        case PF_LOCAL: {
            // First pull out the path, being careful to not overflow anything.
            char path[SOCKADDR_DATA_MAX + 1];
            size_t path_size = *sockaddr_len - offsetof(struct sockaddr_, data);
            memcpy(path, fake_addr->data, path_size);
            path[path_size] = '\0';

            uint32_t socket_id;
            int err;
            if (path_size == 0) {
                return _ENOENT;
            } else if (path[0] != '\0') {
                STRACE(" unix socket %s", path);
                err = unix_socket_get(path, bind_fd, &socket_id);
            } else {
                STRACE(" unix abstract socket %s", path + 1);
                err = unix_abstract_get(path + 1, bind_fd, &socket_id);
            }
            if (err < 0)
                return err;
            if (bind_fd != NULL) {
                bind_fd->socket.unix_name_len = path_size;
                memcpy(bind_fd->socket.unix_name, path, path_size);
            }

            struct sockaddr_un *real_addr_un = sockaddr;
            int path_len = snprintf(real_addr_un->sun_path, sizeof(real_addr_un->sun_path),
                                    "%s%d.%u", sock_tmp_prefix, getpid(), socket_id);
            if (path_len < 0 || (size_t) path_len >= sizeof(real_addr_un->sun_path))
                return _ENAMETOOLONG;
            // The call to real bind will fail if the backing socket already
            // exists from a previous run or something. We already checked that
            // the fake file doesn't exist in unix_socket_get, so try a simple
            // solution.
            if (bind_fd != NULL)
                unlink(real_addr_un->sun_path);
            *sockaddr_len = offsetof(struct sockaddr_un, sun_path) + path_len;
            break;
        }
        default:
            return _EINVAL;
    }
    return 0;
}

static int sockaddr_read(addr_t sockaddr_addr, void *sockaddr, uint_t *sockaddr_len) {
    return sockaddr_read_bind(sockaddr_addr, sockaddr, sockaddr_len, NULL);
}

// Linux AF_INET6 datagram sockets accept IPv4 sockaddrs in connect, sendto
// and sendmsg (__ip6_datagram_connect, udpv6_sendmsg) and use the IPv4-mapped
// address ::ffff:a.b.c.d; glibc's getaddrinfo connects AF_INET6 datagram
// sockets to IPv4 candidates this way. XNU rejects a sockaddr_in on an
// AF_INET6 socket with EINVAL, so map it. Stream sockets and bind() keep
// Linux's EINVAL for the short sockaddr. IPV6_V6ONLY sockets refuse IPv4
// destinations: connect fails with EAFNOSUPPORT, sendto/sendmsg with
// ENETUNREACH (passed as v6only_err). Returns 0 or a negative errno.
static int sockaddr_map_v4_for_v6(struct fd *sock, void *sockaddr, uint_t *sockaddr_len, int v6only_err) {
    if (sock->socket.domain != AF_INET6_ || (sock->socket.type & 0xf) != SOCK_DGRAM_) return 0;
    struct sockaddr_in *in = sockaddr;
    if (in->sin_family != AF_INET) return 0;
    int v6only = 0; socklen_t optlen = sizeof(v6only);
    if (getsockopt(sock->real_fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, &optlen) == 0 && v6only)
        return v6only_err;
    struct sockaddr_in6 mapped = {.sin6_len = sizeof(struct sockaddr_in6), .sin6_family = AF_INET6, .sin6_port = in->sin_port};
    mapped.sin6_addr.s6_addr[10] = 0xff;
    mapped.sin6_addr.s6_addr[11] = 0xff;
    memcpy(&mapped.sin6_addr.s6_addr[12], &in->sin_addr, 4);
    memcpy(sockaddr, &mapped, sizeof(mapped));
    *sockaddr_len = sizeof(mapped);
    return 0;
}

static int sockaddr_write(addr_t sockaddr_addr, void *sockaddr, uint_t buffer_len, uint_t *sockaddr_len) {
    // No source address (e.g. recvmsg/recvfrom on a connected socketpair): the
    // host leaves the buffer untouched and Linux reports length 0. Reading the
    // untouched family here returned EINVAL (zbus/glycin, GitHub issue #5).
    if (*sockaddr_len == 0)
        return 0;
    struct sockaddr *real_addr = sockaddr;
    struct sockaddr_ *fake_addr = sockaddr;
    fake_addr->family = sock_family_from_real(real_addr->sa_family);
    switch (fake_addr->family) {
        case PF_LOCAL_: {
            // Most callers of sockaddr_write use it to return a peer name, and
            // since we don't know the peer name in this case, just return the
            // default peer name, which is the null address.
            static struct sockaddr_ unix_domain_null = {.family = PF_LOCAL_};
            sockaddr = &unix_domain_null;
            *sockaddr_len = sizeof(unix_domain_null);
            break;
        }
        case PF_INET_:
        case PF_INET6_:
            break;
        default:
            return _EINVAL;
    }

    if (buffer_len > *sockaddr_len)
        buffer_len = *sockaddr_len;
    // The address is supposed to be truncated if the specified length is too
    // short, instead of returning an error.
    if (user_write(sockaddr_addr, sockaddr, buffer_len))
        return _EFAULT;
    return 0;
}

int_t sys_bind(fd_t sock_fd, addr_t sockaddr_addr, uint_t sockaddr_len) {
    STRACE("bind(%d, 0x%x, %d)", sock_fd, sockaddr_addr, sockaddr_len);
    int lookup_error;
    struct fd *sock __attribute__((cleanup(sock_release_scoped))) = sock_getfd_retain(sock_fd, &lookup_error);
    if (sock == NULL) return lookup_error;
    if (uevent_socket(sock)) return uevent_bind(sock, sockaddr_addr, sockaddr_len);
    if (fd_is_netlink_stub(sock))
        return netlink_bind(sock, sockaddr_addr, sockaddr_len);
    struct sockaddr_max_ sockaddr;
    int err = sockaddr_read_bind(sockaddr_addr, &sockaddr, &sockaddr_len, sock);
    if (err < 0)
        return err;
    guest_bind_wildcard_to_loopback(&sockaddr);

    err = bind(sock->real_fd, (void *) &sockaddr, sockaddr_len);
    if (err < 0) {
        inode_release_if_exist(sock->socket.unix_name_inode);
        sock->socket.unix_name_inode = NULL;
        if (sock->socket.unix_name_abstract != NULL) {
            unix_abstract_release(sock->socket.unix_name_abstract);
            sock->socket.unix_name_abstract = NULL;
        }
        return errno_map();
    }
    return 0;
}

static void fill_cred(struct ucred_ *cred) {
    cred->pid = current->tgid;
    cred->uid = current->euid;
    cred->gid = current->egid;
}
// Credentials are stored with global ids; Linux reports them in the
// receiver's PID and user namespaces (unmapped: pid 0, uid/gid 65534).
static struct ucred_ cred_for_receiver(struct ucred_ cred) {
    if (current->pidns) { lock(&pids_lock); cred.pid = pid_view(cred.pid, current->pidns); unlock(&pids_lock); }
    cred.uid = userns_view_uid(current->userns, cred.uid);
    cred.gid = userns_view_gid(current->userns, cred.gid);
    return cred;
}

int_t sys_connect(fd_t sock_fd, addr_t sockaddr_addr, uint_t sockaddr_len) {
    STRACE("connect(%d, 0x%x, %d)", sock_fd, sockaddr_addr, sockaddr_len);
    int lookup_error;
    struct fd *sock __attribute__((cleanup(sock_release_scoped))) = sock_getfd_retain(sock_fd, &lookup_error);
    if (sock == NULL) return lookup_error;
    if (uevent_socket(sock)) return uevent_connect(sock, sockaddr_addr, sockaddr_len);
    // connect(AF_UNSPEC) dissolves a datagram socket's association and
    // returns 0 on Linux. glibc's getaddrinfo relies on it when sorting
    // candidate addresses (RFC 6724): it connects a UDP socket to each
    // candidate, reads the source address, then disconnects. XNU also
    // disconnects first but then reports EAFNOSUPPORT/EINVAL for the null
    // address, so only the result needs translating.
    if ((sock->socket.domain == AF_INET_ || sock->socket.domain == AF_INET6_) &&
            (sock->socket.type & 0xf) == SOCK_DGRAM_ && sockaddr_len >= sizeof(uint16_t)) {
        uint16_t family;
        if (user_get(sockaddr_addr, family))
            return _EFAULT;
        if (family == 0 /* AF_UNSPEC */) {
            struct sockaddr unspec = {.sa_len = sizeof(struct sockaddr), .sa_family = AF_UNSPEC};
            if (connect(sock->real_fd, &unspec, sizeof(unspec)) < 0 &&
                    errno != EAFNOSUPPORT && errno != EINVAL && errno != ENOTCONN)
                return errno_map();
            return 0;
        }
    }
    // Linux checks an AF_INET6 stream address's length before its family
    // (tcp_v6_pre_connect): a 16-byte sockaddr_in is EINVAL, where XNU
    // reports EAFNOSUPPORT.
    if (sock->socket.domain == AF_INET6_ && (sock->socket.type & 0xf) == SOCK_STREAM_ &&
            sockaddr_len < 24 /* SIN6_LEN_RFC2133 */)
        return _EINVAL;
    struct sockaddr_max_ sockaddr;
    int err = sockaddr_read(sockaddr_addr, &sockaddr, &sockaddr_len);
    if (err < 0)
        return err;
    err = sockaddr_map_v4_for_v6(sock, &sockaddr, &sockaddr_len, _EAFNOSUPPORT);
    if (err < 0)
        return err;

    err = connect(sock->real_fd, (void *) &sockaddr, sockaddr_len);
    if (err < 0) {
        int ce = errno;
        // Log connect failures except the routine nonblock in-progress case,
        // so VPN / routing issues are visible in user-shared logs.
        if (ce != EINPROGRESS && ce != EALREADY) {
            printk("NETDIAG connect(pid=%d comm=%s fd=%d) errno=%d\n",
                   current->pid, current->comm, sock_fd, ce);
        }
        errno = ce;
        return errno_map();
    }

    if (sock->socket.domain == AF_LOCAL_) {
        fill_cred(&sock->socket.unix_cred);
        assert(sock->socket.unix_peer == NULL);
        // Send a pointer to ourselves to the other end so they can set up the peer pointers.
        ssize_t res = write(sock->real_fd, &sock, sizeof(struct fd *));
        if (res == sizeof(struct fd *)) {
            // Wait for acknowledgement that it happened.
            lock(&peer_lock);
            while (sock->socket.unix_peer == NULL)
                wait_for_ignore_signals(&sock->socket.unix_got_peer, &peer_lock, NULL);
            unlock(&peer_lock);
        }
    }

    return err;
}

int_t sys_listen(fd_t sock_fd, int_t backlog) {
    STRACE("listen(%d, %d)", sock_fd, backlog);
    struct fd *sock = sock_getfd(sock_fd);
    if (sock == NULL)
        return sock_getfd_error(sock_fd);
    // listen() on an unbound IP socket binds an ephemeral port on the wildcard
    // address; under the loopback policy bind that port to loopback first.
    if (guest_listen_loopback() && (sock->socket.domain == AF_INET_ || sock->socket.domain == AF_INET6_)) {
        struct sockaddr_storage bound;
        socklen_t bound_len = sizeof(bound);
        if (getsockname(sock->real_fd, (void *) &bound, &bound_len) == 0 &&
                ((bound.ss_family == PF_INET && ((struct sockaddr_in *) &bound)->sin_port == 0) ||
                 (bound.ss_family == PF_INET6 && ((struct sockaddr_in6 *) &bound)->sin6_port == 0))) {
            guest_bind_wildcard_to_loopback(&bound);
            if (bind(sock->real_fd, (void *) &bound, bound_len) < 0)
                return errno_map();
        }
    }
    int err = listen(sock->real_fd, backlog);
    if (err < 0)
        return errno_map();
    sockrestart_begin_listen(sock);
    return err;
}

int_t sys_accept(fd_t sock_fd, addr_t sockaddr_addr, addr_t sockaddr_len_addr) {
    STRACE("accept(%d, 0x%x, 0x%x)", sock_fd, sockaddr_addr, sockaddr_len_addr);
    struct fd *sock = sock_getfd(sock_fd);
    if (sock == NULL)
        return sock_getfd_error(sock_fd);
    dword_t sockaddr_len = 0;
    dword_t sockaddr_buffer_len = 0;
    if (sockaddr_addr != 0) {
        if (sockaddr_len_addr == 0 || user_get(sockaddr_len_addr, sockaddr_len))
            return _EFAULT;
        if (sockaddr_len > sizeof(struct sockaddr_max_))
            sockaddr_len = sizeof(struct sockaddr_max_);
        sockaddr_buffer_len = sockaddr_len;
    }

    struct sockaddr_max_ sockaddr;
    int client;
    do {
        sockrestart_begin_listen_wait(sock);
        errno = 0;
        client = accept(sock->real_fd,
                sockaddr_addr != 0 ? (void *) &sockaddr : NULL,
                sockaddr_addr != 0 ? &sockaddr_len : NULL);
        sockrestart_end_listen_wait(sock);
    } while (sockrestart_should_restart_listen_wait() && errno == EINTR);
    if (client < 0)
        return errno_map();

    if (sockaddr_addr != 0) {
        int err = sockaddr_write(sockaddr_addr, &sockaddr, sockaddr_buffer_len, &sockaddr_len);
        if (err < 0) {
            close(client);
            return err;
        }
        if (user_put(sockaddr_len_addr, sockaddr_len)) {
            close(client);
            return _EFAULT;
        }
    }

    fd_t client_f = sock_fd_create(client,
            sock->socket.domain, sock->socket.type, sock->socket.protocol, NULL);
    if (client_f < 0)
        return client_f;

    if (sock->socket.domain == AF_LOCAL_) {
        lock(&peer_lock);
        struct fd *client_fd = f_get(client_f);
        fill_cred(&client_fd->socket.unix_cred);
        struct fd *peer;
        ssize_t res = read(client, &peer, sizeof(peer));
        if (res == sizeof(peer)) {
            client_fd->socket.unix_peer = peer;
            peer->socket.unix_peer = client_fd;
            client_fd->socket.unix_peer_tracked = peer->socket.unix_peer_tracked = true;
            unsigned shutdown = client_fd->socket.unix_shutdown;
            client_fd->socket.unix_shutdown |= ((peer->socket.unix_shutdown & 1) << 1) |
                ((peer->socket.unix_shutdown & 2) >> 1);
            peer->socket.unix_shutdown |= ((shutdown & 1) << 1) | ((shutdown & 2) >> 1);
            notify(&peer->socket.unix_got_peer);
        }
        unlock(&peer_lock);
    }

    return client_f;
}

int_t sys_accept4(fd_t sock_fd, addr_t sockaddr_addr, addr_t sockaddr_len_addr, int_t flags) {
    STRACE("accept4(%d, 0x%x, 0x%x, 0x%x)", sock_fd, sockaddr_addr, sockaddr_len_addr, flags);
    if (flags & ~(SOCK_NONBLOCK_ | SOCK_CLOEXEC_))
        return _EINVAL;
    int_t client_f = sys_accept(sock_fd, sockaddr_addr, sockaddr_len_addr);
    if (client_f < 0)
        return client_f;
    struct fd *client_fd = f_get(client_f);
    if (client_fd != NULL) {
        if (flags & SOCK_NONBLOCK_)
            fd_setflags(client_fd, O_NONBLOCK_);
        if (flags & SOCK_CLOEXEC_) {
            lock(&current->files->lock);
            bit_set(client_f, current->files->cloexec);
            unlock(&current->files->lock);
        }
    }
    return client_f;
}

static int copy_unix_name(struct sockaddr_max_ *sockaddr, dword_t buffer_len, dword_t *sockaddr_len, struct fd *sock) {
    struct sockaddr_ *fake_addr = (void *) sockaddr;
    size_t name_len_full = sock->socket.unix_name_len;
    if (name_len_full > sizeof(sockaddr->data))
        name_len_full = sizeof(sockaddr->data);
    size_t full_len = offsetof(struct sockaddr_, data) + name_len_full;
    *sockaddr_len = full_len;
    if (buffer_len > sizeof(*sockaddr))
        buffer_len = sizeof(*sockaddr);
    if (buffer_len < offsetof(struct sockaddr_, data))
        return 0;

    fake_addr->family = PF_LOCAL_;
    size_t data_len = buffer_len - offsetof(struct sockaddr_, data);
    size_t name_len = name_len_full;
    if (name_len > data_len)
        name_len = data_len;
    memset(fake_addr->data, 0, data_len);
    memcpy(fake_addr->data, sock->socket.unix_name, name_len);
    return 0;
}

int_t sys_getsockname(fd_t sock_fd, addr_t sockaddr_addr, addr_t sockaddr_len_addr) {
    STRACE("getsockname(%d, 0x%x, 0x%x)", sock_fd, sockaddr_addr, sockaddr_len_addr);
    int lookup_error;
    struct fd *sock __attribute__((cleanup(sock_release_scoped))) = sock_getfd_retain(sock_fd, &lookup_error);
    if (sock == NULL) return lookup_error;
    if (uevent_socket(sock)) return uevent_name(sock, sockaddr_addr, sockaddr_len_addr, false);
    if (fd_is_netlink_stub(sock)) {
        uint32_t len;
        if (user_get(sockaddr_len_addr, len)) return _EFAULT;
        int err = netlink_name(sock, sockaddr_addr, len);
        if (err < 0) return err;
        len = sizeof(struct sockaddr_nl_);
        return user_put(sockaddr_len_addr, len) ? _EFAULT : 0;
    }
    dword_t sockaddr_len;
    if (user_get(sockaddr_len_addr, sockaddr_len))
        return _EFAULT;
    dword_t sockaddr_buffer_len = sockaddr_len;
    if (sockaddr_len > sizeof(struct sockaddr_max_))
        sockaddr_len = sizeof(struct sockaddr_max_);
    struct sockaddr_max_ sockaddr;

    // if this is a unix socket, return the same string passed to bind
    if (sock->socket.domain == PF_LOCAL_) {
        int err = copy_unix_name(&sockaddr, sockaddr_buffer_len, &sockaddr_len, sock);
        if (err < 0)
            return err;
        size_t write_len = sockaddr_buffer_len < sockaddr_len ? sockaddr_buffer_len : sockaddr_len;
        if (write_len != 0 && user_write(sockaddr_addr, &sockaddr, write_len))
            return _EFAULT;
        if (user_put(sockaddr_len_addr, sockaddr_len))
            return _EFAULT;
        return 0;
    }

    int res = getsockname(sock->real_fd, (void *) &sockaddr, &sockaddr_len);
    if (res < 0)
        return errno_map();

    int err = sockaddr_write(sockaddr_addr, &sockaddr, sockaddr_buffer_len, &sockaddr_len);
    if (err < 0)
        return err;
    if (user_put(sockaddr_len_addr, sockaddr_len))
        return _EFAULT;
    return res;
}

int_t sys_getpeername(fd_t sock_fd, addr_t sockaddr_addr, addr_t sockaddr_len_addr) {
    STRACE("getpeername(%d, 0x%x, 0x%x)", sock_fd, sockaddr_addr, sockaddr_len_addr);
    int lookup_error;
    struct fd *sock __attribute__((cleanup(sock_release_scoped))) = sock_getfd_retain(sock_fd, &lookup_error);
    if (sock == NULL) return lookup_error;
    if (uevent_socket(sock)) return uevent_name(sock, sockaddr_addr, sockaddr_len_addr, true);
    dword_t sockaddr_len;
    if (user_get(sockaddr_len_addr, sockaddr_len))
        return _EFAULT;
    dword_t sockaddr_buffer_len = sockaddr_len;
    if (sockaddr_len > sizeof(struct sockaddr_max_))
        sockaddr_len = sizeof(struct sockaddr_max_);

    // TODO if this is a unix socket, return the same string the peer passed to
    // bind once the peer pointer is available

    struct sockaddr_max_ sockaddr;
    int res = getpeername(sock->real_fd, (void *) &sockaddr, &sockaddr_len);
    if (res < 0)
        return errno_map();

    int err = sockaddr_write(sockaddr_addr, &sockaddr, sockaddr_buffer_len, &sockaddr_len);
    if (err < 0)
        return err;
    if (user_put(sockaddr_len_addr, sockaddr_len))
        return _EFAULT;
    return res;
}

int_t sys_socketpair(dword_t domain, dword_t type, dword_t protocol, addr_t sockets_addr) {
    STRACE("socketpair(%d, %d, %d, 0x%x)", domain, type, protocol, sockets_addr);
    int real_domain = sock_family_to_real(domain);
    if (real_domain < 0)
        return _EAFNOSUPPORT;
    int real_type = sock_type_to_real(type, protocol);
    if (real_type < 0)
        return _EINVAL;

    int sockets[2];
    int err = socketpair(real_domain, real_type, protocol, sockets);
    if (err < 0)
        return errno_map();

    int fake_sockets[2] = {-1, -1};
    struct fd *sock1 = NULL, *sock2 = NULL;
    // Do not hold peer_lock over installation: failed f_install invokes the
    // socket destructor, which acquires peer_lock. Each helper consumes its
    // native handle even on allocation/table-limit failure.
    err = fake_sockets[0] = sock_fd_create(sockets[0], domain, type, protocol, &sock1);
    if (err < 0) { close(sockets[1]); return err; }
    err = fake_sockets[1] = sock_fd_create(sockets[1], domain, type, protocol, &sock2);
    if (err < 0) goto out;
    lock(&peer_lock);
    fill_cred(&sock1->socket.unix_cred);
    fill_cred(&sock2->socket.unix_cred);
    sock1->socket.unix_peer = sock2;
    sock2->socket.unix_peer = sock1;
    sock1->socket.unix_peer_tracked = sock2->socket.unix_peer_tracked = true;
    unlock(&peer_lock);
    err = user_put(sockets_addr, fake_sockets) ? _EFAULT : 0;
    if (!err) STRACE(" [%d, %d]", fake_sockets[0], fake_sockets[1]);
out:
    if (err < 0) {
        if (fake_sockets[0] >= 0) f_close_matching(fake_sockets[0], sock1);
        if (fake_sockets[1] >= 0) f_close_matching(fake_sockets[1], sock2);
    }
    if (sock1) fd_close(sock1);
    if (sock2) fd_close(sock2);
    return err;
}

static int_t sock_sendto(fd_t sock_fd, addr_t buffer_addr, dword_t len, dword_t flags, addr_t sockaddr_addr, dword_t sockaddr_len);
// MSG_NOSIGNAL: a broken stream returns EPIPE without raising SIGPIPE, on
// every path the send takes (errno_map raises it otherwise).
int_t sys_sendto(fd_t sock_fd, addr_t buffer_addr, dword_t len, dword_t flags, addr_t sockaddr_addr, dword_t sockaddr_len) {
    bool saved = current->send_nosignal;
    current->send_nosignal = saved || (flags & MSG_NOSIGNAL_);
    int_t result = sock_sendto(sock_fd, buffer_addr, len, flags, sockaddr_addr, sockaddr_len);
    current->send_nosignal = saved;
    return result;
}
static int_t sock_sendto(fd_t sock_fd, addr_t buffer_addr, dword_t len, dword_t flags, addr_t sockaddr_addr, dword_t sockaddr_len) {
    int lookup_error;
    struct fd *sock __attribute__((cleanup(sock_release_scoped))) = sock_getfd_retain(sock_fd, &lookup_error);
    if (sock == NULL) return lookup_error;
    if (uevent_socket(sock)) return uevent_sendto(sock, buffer_addr, len, flags, sockaddr_addr, sockaddr_len);
    if (fd_is_netlink_stub(sock)) {
        if (flags & ~(MSG_DONTWAIT_ | MSG_NOSIGNAL_)) return _EOPNOTSUPP;
        int err = netlink_destination(sockaddr_addr, sockaddr_len);
        if (err < 0) return err;
        if (len > NETLINK_BUFFER_SIZE) return _EMSGSIZE;
        char req[NETLINK_BUFFER_SIZE];
        if (len && user_read(buffer_addr, req, len)) return _EFAULT;
        return netlink_stub_reply(sock, req, len);
    }
    char *buffer = malloc((size_t) len + 1);
    if (buffer == NULL)
        return _ENOMEM;
    if (user_read(buffer_addr, buffer, len)) {
        free(buffer);
        return _EFAULT;
    }
    buffer[len] = '\0';
    STRACE("sendto(%d, \"%.100s\", %d, %d, 0x%x, %d)", sock_fd, buffer, len, flags, sockaddr_addr, sockaddr_len);
    int real_flags = sock_flags_to_real(flags);
    int err = _EINVAL;
    if (real_flags < 0)
        goto error;
    struct sockaddr_max_ sockaddr;
    if (sockaddr_addr) {
        err = sockaddr_read(sockaddr_addr, &sockaddr, &sockaddr_len);
        if (err < 0)
            goto error;
        err = sockaddr_map_v4_for_v6(sock, &sockaddr, &sockaddr_len, _ENETUNREACH);
        if (err < 0)
            goto error;
    }

    bool handled = false;
    ssize_t res = sockaddr_addr ? 0 : unix_credential_send(sock,buffer,len,real_flags,&handled);
    if (!handled) res = sendto(sock->real_fd, buffer, len, real_flags,
            sockaddr_addr ? (void *) &sockaddr : NULL, sockaddr_len);
    else if (res < 0) { free(buffer); return res; }
    free(buffer);
    if (res < 0)
        return errno_map();
    return res;

error:
    free(buffer);
    return err;
}

// Read a socket timeout option as milliseconds. Returns 0 if unset or error.
static long sock_get_timeout_ms(int real_fd, int optname) {
    struct timeval tv = {0, 0};
    socklen_t len = sizeof(tv);
    if (getsockopt(real_fd, SOL_SOCKET, optname, &tv, &len) < 0)
        return 0;
    return (long)tv.tv_sec * 1000L + (long)tv.tv_usec / 1000L;
}

static long monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + (long)ts.tv_nsec / 1000000L;
}

// Wait for a socket fd to become ready (readable or writable), with a
// bounded per-poll timeout so guest signals can interrupt even if the host
// thread is briefly suspended (iOS). Honors SO_RCVTIMEO / SO_SNDTIMEO as
// an overall deadline when set.
//
// Returns 0 when ready, _EINTR on guest signal, _EAGAIN on timeout expiry,
// or a negative errno on poll() error.
static int sock_wait_for(int real_fd, short events, int timeout_opt) {
    long timeout_ms = sock_get_timeout_ms(real_fd, timeout_opt);
    long start_ms = monotonic_ms();
    long deadline_ms = (timeout_ms > 0) ? (start_ms + timeout_ms) : 0;
    bool slow_logged = false;
    for (;;) {
        if (current->exiting || current->group->doing_group_exit)
            return _EINTR;
        if (current->sighand != NULL) {
            lock(&current->sighand->lock);
            bool pending = !!(current->pending & ~current->blocked);
            unlock(&current->sighand->lock);
            if (pending)
                return _EINTR;
        }

        // Keep this shorter than exit_group's safety-valve window so helper
        // threads blocked in recv/recvmsg can observe SIGKILL/exiting and
        // leave cleanly instead of being detached as leaked host threads.
        int poll_ms = 50;
        if (deadline_ms > 0) {
            long remaining = deadline_ms - monotonic_ms();
            if (remaining <= 0) {
                printk("NETDIAG sock_wait(pid=%d comm=%s fd=%d ev=%#x) deadline-expired after %ldms\n",
                       current->pid, current->comm, real_fd, events, timeout_ms);
                return _EAGAIN;
            }
            if (remaining < poll_ms)
                poll_ms = (int)remaining;
        }

        struct pollfd pfd = { .fd = real_fd, .events = events };
        current->blocking = true;
        int pr = poll(&pfd, 1, poll_ms);
        int saved_errno = errno;
        current->blocking = false;
        if (current->exiting || current->group->doing_group_exit)
            return _EINTR;

        if (pr > 0)
            return 0;
        if (pr == 0) {
            // Log once if we're stuck > 3 seconds without any ready event
            if (!slow_logged && (monotonic_ms() - start_ms) >= 3000) {
                printk("NETDIAG sock_wait(pid=%d comm=%s fd=%d ev=%#x) no-event for 3s (timeout_opt_ms=%ld)\n",
                       current->pid, current->comm, real_fd, events, timeout_ms);
                slow_logged = true;
            }
            continue;
        }
        if (saved_errno == EINTR)
            continue;
        errno = saved_errno;
        printk("NETDIAG sock_wait(pid=%d comm=%s fd=%d ev=%#x) poll err=%d\n",
               current->pid, current->comm, real_fd, events, saved_errno);
        return errno_map();
    }
}

static int sock_wait_readable(int real_fd) {
    return sock_wait_for(real_fd, POLLIN, SO_RCVTIMEO);
}

int_t sys_recvfrom(fd_t sock_fd, addr_t buffer_addr, dword_t len, dword_t flags, addr_t sockaddr_addr, addr_t sockaddr_len_addr) {
    STRACE("recvfrom(%d, 0x%x, %d, %d, 0x%x, 0x%x)", sock_fd, buffer_addr, len, flags, sockaddr_addr, sockaddr_len_addr);
    int lookup_error;
    struct fd *sock __attribute__((cleanup(sock_release_scoped))) = sock_getfd_retain(sock_fd, &lookup_error);
    if (sock == NULL) return lookup_error;
    if (uevent_socket(sock)) return uevent_recvfrom(sock, buffer_addr, len, flags, sockaddr_addr, sockaddr_len_addr);
    if (fd_is_netlink_stub(sock))
        return netlink_recvfrom(sock, buffer_addr, len, flags, sockaddr_addr, sockaddr_len_addr);
    int real_flags = sock_flags_to_real(flags);
    if (real_flags < 0)
        return _EINVAL;
    uint_t sockaddr_len = 0;
    uint_t sockaddr_buffer_len = 0;
    if (sockaddr_len_addr != 0) {
        if (user_get(sockaddr_len_addr, sockaddr_len))
            return _EFAULT;
        sockaddr_buffer_len = sockaddr_len;
        if (sockaddr_len > sizeof(struct sockaddr_max_))
            sockaddr_len = sizeof(struct sockaddr_max_);
    }

    char *buffer = malloc(len);
    if (len != 0 && buffer == NULL)
        return _ENOMEM;
    struct sockaddr_max_ sockaddr;

    // Determine whether this call should wait for data:
    //   * guest flags MSG_DONTWAIT → never wait (explicit)
    //   * fd has O_NONBLOCK set   → never wait (fd-level nonblock)
    //
    // The second case is critical: musl's res_msend creates UDP sockets
    // with SOCK_NONBLOCK and then calls recv(fd, buf, len, 0). It relies
    // on the kernel returning EAGAIN immediately on empty buffer to
    // terminate its "drain all pending responses" loop. If we wait when
    // the fd itself is nonblock, musl loops forever and DNS resolution
    // appears to hang. We check both the iSH-level fd flags and the host
    // fd's real flags, in case they diverge (e.g. set via fcntl only).
    int host_flags = fcntl(sock->real_fd, F_GETFL, 0);
    bool host_nonblock = (host_flags >= 0) && (host_flags & O_NONBLOCK);
    bool guest_nonblock = (sock->flags & O_NONBLOCK_) != 0;
    bool should_wait = !(real_flags & MSG_DONTWAIT) && !guest_nonblock && !host_nonblock;

    // Try a nonblock recvfrom first (fast path — no host syscall overhead
    // when data is already buffered). Only if it returns EAGAIN and we
    // should wait do we fall back to the bounded-poll wait.
    ssize_t res;
    if (should_wait) {
        for (;;) {
            res = sock->socket.domain == AF_LOCAL_ && sock->socket.unix_passcred
                    ? unix_plain_recv(sock,buffer,len,real_flags | MSG_DONTWAIT, sockaddr_addr ? (void *)&sockaddr : NULL, sockaddr_len_addr ? &sockaddr_len : NULL)
                    : recvfrom(sock->real_fd, buffer, len, real_flags | MSG_DONTWAIT,
                    sockaddr_addr != 0 ? (void *) &sockaddr : NULL,
                    sockaddr_len_addr != 0 ? &sockaddr_len : NULL);
            if (res >= 0)
                break;
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                free(buffer);
                return errno_map();
            }
            // EAGAIN: no data yet. Wait (bounded) for readability or a
            // guest signal, then retry.
            int wr = sock_wait_readable(sock->real_fd);
            if (wr < 0) {
                free(buffer);
                return wr;
            }
        }
    } else {
        res = sock->socket.domain == AF_LOCAL_ && sock->socket.unix_passcred
                ? unix_plain_recv(sock,buffer,len,real_flags,sockaddr_addr ? (void *)&sockaddr : NULL,sockaddr_len_addr ? &sockaddr_len : NULL)
                : recvfrom(sock->real_fd, buffer, len, real_flags,
                sockaddr_addr != 0 ? (void *) &sockaddr : NULL,
                sockaddr_len_addr != 0 ? &sockaddr_len : NULL);
        if (res < 0) {
            free(buffer);
            return errno_map();
        }
    }

    if (res > 0 && user_write(buffer_addr, buffer, (size_t) res)) {
        free(buffer);
        return _EFAULT;
    }
    free(buffer);
    if (sockaddr_addr != 0) {
        int err = sockaddr_write(sockaddr_addr, &sockaddr, sockaddr_buffer_len, &sockaddr_len);
        if (err < 0)
            return err;
    }
    if (sockaddr_len_addr != 0)
        if (user_put(sockaddr_len_addr, sockaddr_len))
            return _EFAULT;
    return res;
}

int_t sys_send(fd_t sock_fd, addr_t buf, dword_t len, int_t flags) {
    return sys_sendto(sock_fd, buf, len, flags, 0, 0);
}

int_t sys_recv(fd_t sock_fd, addr_t buf, dword_t len, int_t flags) {
    return sys_recvfrom(sock_fd, buf, len, flags, 0, 0);
}

int_t sys_shutdown(fd_t sock_fd, dword_t how) {
    STRACE("shutdown(%d, %d)", sock_fd, how);
    int error;
    struct fd *sock = sock_getfd_retain(sock_fd, &error);
    if (sock == NULL) return error;
    if (how > 2) { fd_close(sock); return _EINVAL; }
    if (uevent_socket(sock)) { fd_close(sock); return _EOPNOTSUPP; }
    bool unix_connection = sock->socket.domain == AF_LOCAL_ &&
        (sock->socket.type == SOCK_STREAM_ || sock->socket.type == SOCK_SEQPACKET_);
    if (unix_connection) lock(&peer_lock);
    int err = shutdown(sock->real_fd, how);
    error = err < 0 ? errno_map() : 0;
    if (!error && unix_connection) {
        unsigned mode = how + 1;
        sock->socket.unix_shutdown |= mode;
        struct fd *peer = sock->socket.unix_peer;
        if (peer) peer->socket.unix_shutdown |= ((mode & 1) << 1) | ((mode & 2) >> 1);
    }
    if (unix_connection) unlock(&peer_lock);
    fd_close(sock);
    return error;
}

#define DEFAULT_TCP_CONGESTION "cubic"

int_t sys_setsockopt(fd_t sock_fd, dword_t level, dword_t option, addr_t value_addr, dword_t value_len) {
    STRACE("setsockopt(%d, %d, %d, 0x%x, %d)", sock_fd, level, option, value_addr, value_len);
    int lookup_error;
    struct fd *sock = sock_getfd_retain(sock_fd,&lookup_error);
    if (sock == NULL)
        return lookup_error;
    if (uevent_socket(sock)) { int result; if (uevent_setsockopt(sock,level,option,value_addr,value_len,&result)) { fd_close(sock); return result; } }

    char *value = NULL;
    if (value_len != 0) {
        value = malloc(value_len);
        if (value == NULL)
            { fd_close(sock); return _ENOMEM; }
        if (user_read(value_addr, value, value_len)) {
            free(value); fd_close(sock);
            return _EFAULT;
        }
    }

    int ret = 0;

    if (level == SOL_SOCKET_ && option == SO_PASSCRED_) {
        if (value_len < sizeof(int)) { ret = _EINVAL; goto out; }
        int enabled; memcpy(&enabled, value, sizeof enabled);
        lock(&peer_lock);
        // Only registered, connected guest Unix peers are implemented. Do not
        // promise credentials for unconnected/host-owned datagrams or listeners.
        if (sock->socket.domain != AF_LOCAL_ || !sock->socket.unix_peer) ret = _EOPNOTSUPP;
        else sock->socket.unix_passcred = enabled != 0;
        unlock(&peer_lock);
        goto out;
    }

    // Tailscale's Linux netns path requests SO_BINDTODEVICE. Keep this
    // compatibility opt-in with netlink; honour the binding rather than silently
    // ignoring it. Darwin takes an interface index instead of a Linux name.
    if (level == SOL_SOCKET_ && option == 25 && ish_netlink_stub_enabled()) {
        if (value_len > IF_NAMESIZE) { ret=_EINVAL; goto out; }
        char name[IF_NAMESIZE + 1] = {0};
        memcpy(name, value, value_len);
#ifdef __APPLE__
        unsigned index = name[0] ? if_nametoindex(name) : 0;
        if (name[0] && !index) { ret=_ENODEV; goto out; }
        int result;
        if (sock->socket.domain == AF_INET_)
            result = setsockopt(sock->real_fd, IPPROTO_IP, IP_BOUND_IF, &index, sizeof(index));
        else if (sock->socket.domain == AF_INET6_)
            result = setsockopt(sock->real_fd, IPPROTO_IPV6, IPV6_BOUND_IF, &index, sizeof(index));
        else { ret=_EOPNOTSUPP; goto out; }
#else
        int result = setsockopt(sock->real_fd, SOL_SOCKET, SO_BINDTODEVICE, name, strlen(name) + 1);
#endif
        ret=result < 0 ? errno_map() : 0; goto out;
    }

    // ICMP6_FILTER can only be set on real SOCK_RAW
    if (level == IPPROTO_ICMPV6 && option == ICMP6_FILTER_)
        goto out;
    // IP_MTU_DISCOVER has no equivalent on Darwin
    if (level == IPPROTO_IP && option == IP_MTU_DISCOVER_) {
#ifdef __APPLE__
        ret = _ENOPROTOOPT; goto out;
#endif
    }
    // TCP_CONGESTION also has no equivalent on Darwin
#if defined(__APPLE__)
    if (level == IPPROTO_TCP && option == TCP_CONGESTION_) {
        size_t default_len = strlen(DEFAULT_TCP_CONGESTION);
        bool has_nul = value_len == default_len + 1 && value[default_len] == '\0';
        bool same_len = value_len == default_len || has_nul;
        if (same_len && memcmp(value, DEFAULT_TCP_CONGESTION, default_len) == 0)
            goto out;
        ret = _ENOENT;
        goto out;
    }
#endif

    int real_opt = sock_opt_to_real(option, level);
    if (real_opt < 0) {
        ret = _ENOPROTOOPT;
        goto out;
    }
    int real_level = sock_level_to_real(level);
    if (real_level < 0) {
        ret = _EINVAL;
        goto out;
    }

    // 0 means the option is not implemented, but things rely on it, so we
    // should just ignore attempts to set it.
    if (real_opt == 0)
        goto out;

    int err = setsockopt(sock->real_fd, real_level, real_opt, value, value_len);
    if (err < 0)
        ret = errno_map();

out:
    free(value);
    fd_close(sock);
    return ret;
}

int_t sys_getsockopt(fd_t sock_fd, dword_t level, dword_t option, addr_t value_addr, addr_t len_addr) {
    STRACE("getsockopt(%d, %d, %d, %#x, %#x)", sock_fd, level, option, value_addr, len_addr);
    int lookup_error;
    struct fd *sock = sock_getfd_retain(sock_fd,&lookup_error);
    if (sock == NULL)
        return lookup_error;
    if (uevent_socket(sock)) { int result; if (uevent_getsockopt(sock,level,option,value_addr,len_addr,&result)) { fd_close(sock); return result; } }
    dword_t value_len;
    if (user_get(len_addr, value_len)) { fd_close(sock); return _EFAULT; }
    dword_t buffer_len = value_len;

    char *value = NULL;
    if (buffer_len != 0) {
        value = malloc(buffer_len);
        if (value == NULL)
            { fd_close(sock); return _ENOMEM; }
    }

    int ret = 0;

    if (level == SOL_SOCKET_ && option == SO_PASSCRED_) {
        int enabled;
        lock(&peer_lock); enabled = sock->socket.unix_passcred; unlock(&peer_lock);
        value_len = buffer_len < sizeof enabled ? buffer_len : sizeof enabled;
        if (value_len) memcpy(value, &enabled, value_len);
    } else if (level == SOL_SOCKET_ && (option == SO_DOMAIN_ || option == SO_TYPE_ || option == SO_PROTOCOL_)) {
        dword_t *value_p = (dword_t *) value;
        if (buffer_len != sizeof(*value_p)) {
            ret = _EINVAL;
            goto out;
        }
        if (option == SO_DOMAIN_)
            *value_p = sock->socket.domain;
        else if (option == SO_TYPE_)
            *value_p = sock->socket.type;
        else if (option == SO_PROTOCOL_)
            *value_p = sock->socket.protocol;
        value_len = sizeof(*value_p);
    } else if (level == SOL_SOCKET_ && option == SO_PEERCRED_) {
        struct ucred_ *cred = (struct ucred_ *) value;
        if (buffer_len != sizeof(*cred)) {
            ret = _EINVAL;
            goto out;
        }
        lock(&peer_lock);
        if (sock->socket.domain != AF_LOCAL_ || sock->socket.unix_peer == NULL) {
            cred->pid = 0;
            cred->uid = cred->gid = -1;
        } else {
            *cred = sock->socket.unix_peer->socket.unix_cred;
        }
        unlock(&peer_lock);
        if (cred->pid != 0) *cred = cred_for_receiver(*cred);
        value_len = sizeof(*cred);
    } else if (level == SOL_SOCKET_ && option == SO_PEERPIDFD_) {
        // Linux 6.5: a pidfd for the process at the other end of a connected
        // AF_UNIX socket (the SO_PEERCRED process). zbus (GLib/glycin image
        // loaders, GitHub issue #5) treats ENOPROTOOPT as "older kernel" but any
        // other error as fatal.
        if (buffer_len < sizeof(int_t)) { ret = _EINVAL; goto out; }
        pid_t_ peer = 0;
        lock(&peer_lock);
        if (sock->socket.domain == AF_LOCAL_ && sock->socket.unix_peer != NULL)
            peer = sock->socket.unix_peer->socket.unix_cred.pid;
        unlock(&peer_lock);
        if (peer == 0) { ret = _ENODATA; goto out; }
        lock(&pids_lock);
        struct task *peer_task = pid_get_task(peer);
        bool alive = peer_task != NULL && !peer_task->zombie;
        unlock(&pids_lock);
        if (!alive) { ret = _ESRCH; goto out; }
        int_t pidfd = pidfd_install(peer);
        if (pidfd < 0) { ret = pidfd; goto out; }
        memcpy(value, &pidfd, sizeof pidfd);
        value_len = sizeof pidfd;
    } else if (level == SOL_SOCKET_ && option == SO_ERROR_) {
        if (buffer_len != sizeof(dword_t)) {
            ret = _EINVAL;
            goto out;
        }
        int real_error;
        socklen_t real_error_len = sizeof(real_error);
        int err = getsockopt(sock->real_fd, SOL_SOCKET, SO_ERROR, &real_error, &real_error_len);
        if (err < 0) {
            ret = errno_map();
            goto out;
        }
        *(dword_t *) value = real_error == 0 ? 0 : -err_map(real_error);
        value_len = sizeof(dword_t);
    } else if (level == SOL_SOCKET_ && (option == SO_OOBINLINE_ || option == SO_REUSEADDR_ ||
                option == SO_REUSEPORT_ || option == SO_KEEPALIVE_ || option == SO_BROADCAST_ || option == SO_TIMESTAMP_)) {
        // BSD may return a socket flag bit (e.g. SO_OOBINLINE == 0x100).
        // Linux returns a boolean integer, including with a short user buffer.
        int enabled = 0; socklen_t len = sizeof(enabled);
        if (getsockopt(sock->real_fd, SOL_SOCKET, sock_opt_to_real(option, level), &enabled, &len) < 0) {
            ret = errno_map(); goto out;
        }
        enabled = !!enabled;
        value_len = buffer_len < sizeof(enabled) ? buffer_len : sizeof(enabled);
        if (value_len) memcpy(value, &enabled, value_len);
    } else if (level == IPPROTO_TCP && option == TCP_CONGESTION_) {
        value_len = strlen(DEFAULT_TCP_CONGESTION);
        size_t copy_len = buffer_len < value_len ? buffer_len : value_len;
        if (copy_len != 0)
            memcpy(value, DEFAULT_TCP_CONGESTION, copy_len);
#if defined(__APPLE__)
    } else if (level == IPPROTO_TCP && option == TCP_INFO_) {
        // This one's fun. On Linux, the struct is not ABI dependent, so no
        // special handling is needed. On Darwin, the struct is completely
        // different and has a different sockopt name.
        struct tcp_connection_info conn_info;
        socklen_t conn_info_size = sizeof(conn_info);
        int err = getsockopt(sock->real_fd, IPPROTO_TCP, TCP_CONNECTION_INFO, &conn_info, &conn_info_size);
        if (err < 0) {
            ret = errno_map();
            goto out;
        }

        // The possible keys for this table are in netinet/tcp_fsm.h, but that
        // header isn't available on iOS, only macOS.
        static const uint8_t tcp_state_table[] = {
            7, // TCPS_CLOSED
            10, // TCPS_LISTEN
            2, // TCPS_SYN_SENT
            3, // TCPS_SYN_RECEIVED
            1, // TCPS_ESTABLISHED
            8, // TCPS_CLOSE_WAIT
            4, // TCPS_FIN_WAIT_1
            11, // TCPS_CLOSING
            9, // TCPS_LAST_ACK
            5, // TCPS_FIN_WAIT_2
            6, // TCPS_TIME_WAIT
        };
        struct tcp_info_ info = {
            .state = tcp_state_table[conn_info.tcpi_state],
            .options = conn_info.tcpi_options,
            .snd_wscale = conn_info.tcpi_snd_wscale,
            .rcv_wscale = conn_info.tcpi_rcv_wscale,

            .rto = conn_info.tcpi_rto * 1000,
            .snd_mss = conn_info.tcpi_maxseg,

            .rtt = conn_info.tcpi_srtt * 1000,
            .rttvar = conn_info.tcpi_rttvar * 1000,
            .snd_ssthresh = conn_info.tcpi_snd_ssthresh,
            .snd_cwnd = conn_info.tcpi_snd_cwnd / conn_info.tcpi_maxseg,

            // https://lkml.org/lkml/2017/4/24/923
            .total_retrans = conn_info.tcpi_txretransmitpackets,
        };
        value_len = sizeof(struct tcp_info_);
        size_t copy_len = buffer_len < value_len ? buffer_len : value_len;
        if (copy_len != 0)
            memcpy(value, &info, copy_len);
#endif
    } else {
        int real_opt = sock_opt_to_real(option, level);
        if (real_opt < 0) {
            ret = _ENOPROTOOPT;   // Linux: unknown option; was EINVAL
            goto out;
        }
        int real_level = sock_level_to_real(level);
        if (real_level < 0) {
            ret = _EINVAL;
            goto out;
        }

        socklen_t real_value_len = value_len;
        int err = getsockopt(sock->real_fd, real_level, real_opt, value, &real_value_len);
        if (err < 0) {
            ret = errno_map();
            goto out;
        }
        value_len = real_value_len;
    }

    if (user_put(len_addr, value_len)) {
        ret = _EFAULT;
        goto out;
    }
    size_t write_len = buffer_len < value_len ? buffer_len : value_len;
    if (write_len != 0 && user_write(value_addr, value, write_len))
        ret = _EFAULT;

out:
    free(value);
    fd_close(sock);
    return ret;
}

#ifdef GUEST_ARM64
#define cmsghdr_guest cmsghdr64_
#define CMSG_NXTHDR_GUEST CMSG_NXTHDR64_
#else
#define cmsghdr_guest cmsghdr_
#define CMSG_NXTHDR_GUEST CMSG_NXTHDR_
#endif

static void scm_free(struct scm *scm) {
    for (unsigned i = 0; i < scm->num_fds; i++)
        if (scm->fds[i]) fd_close(scm->fds[i]);
    free(scm);
}

// Attach the actual sending guest process credentials to ordinary sends/writes
// too. Darwin's host PID is shared by all guest processes and cannot be used.
static ssize_t unix_credential_send(struct fd *sock, const void *buf, size_t len, int flags, bool *handled) {
    *handled = false;
    if (sock->socket.domain != AF_LOCAL_) return 0;
    lock(&sock->socket.unix_send_lock);
    lock(&peer_lock);
    struct fd *peer = sock->socket.unix_peer;
    if (!peer || !peer->socket.unix_passcred) {
        unlock(&peer_lock); unlock(&sock->socket.unix_send_lock); return 0;
    }
    peer = fd_retain(peer);
    unlock(&peer_lock);
    *handled = true;
    if (len == 0 && sock->socket.type == SOCK_STREAM_) {
        // Darwin can send an ancillary-only stream record; Linux cannot.
        fd_close(peer); unlock(&sock->socket.unix_send_lock); return 0;
    }
    struct scm *scm = calloc(1, sizeof(*scm));
    if (!scm) { fd_close(peer); unlock(&sock->socket.unix_send_lock); return _ENOMEM; }
    list_init(&scm->queue); scm->has_credentials = true; fill_cred(&scm->credentials);
    // A native ancillary marker preserves the association with stream bytes.
    // Sending our socket descriptor is only a host marker; it is closed on recv.
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = {(void *)buf,len};
    struct msghdr msg = {.msg_iov=&iov,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof control};
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);c->cmsg_len=CMSG_LEN(sizeof(int));c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;
    memcpy(CMSG_DATA(c), &sock->real_fd, sizeof(int));
    lock(&peer->lock); list_add_tail(&peer->socket.unix_scm,&scm->queue); unlock(&peer->lock);
    ssize_t result = sendmsg(sock->real_fd,&msg,flags);
    if (result < 0 && errno == EMSGSIZE && sock->socket.type == SOCK_STREAM_)
        result = unix_stream_send_fitting(sock, &msg, flags, unix_send_may_block(sock, flags));
    else if (result < 0)
        result = errno_map();
    if (result < 0 || (result == 0 && sock->socket.type == SOCK_STREAM_)) {
        lock(&peer->lock); list_remove_safe(&scm->queue); unlock(&peer->lock); scm_free(scm);
    }
    fd_close(peer); unlock(&sock->socket.unix_send_lock); return result;
}

static void unix_discard_marker(struct fd *sock, struct msghdr *msg, int flags) {
    struct cmsghdr *c = CMSG_FIRSTHDR(msg);
    if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) return;
    int marker;memcpy(&marker,CMSG_DATA(c),sizeof marker);close(marker);
    if (flags & MSG_PEEK) return;
    lock(&sock->lock);
    struct scm *scm = list_empty(&sock->socket.unix_scm) ? NULL : list_first_entry(&sock->socket.unix_scm,struct scm,queue);
    if (scm) list_remove(&scm->queue);
    unlock(&sock->lock);
    if (scm) {
        if (scm->has_credentials) { sock->socket.unix_last_cred=scm->credentials;sock->socket.unix_last_cred_valid=true; }
        scm_free(scm);
    }
}

static ssize_t unix_plain_recv(struct fd *sock, void *buf, size_t len, int flags, void *name, socklen_t *namelen) {
    char control[CMSG_SPACE(sizeof(int))] = {0};struct iovec iov={buf,len};
    struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof control,.msg_name=name,.msg_namelen=namelen?*namelen:0};
    ssize_t result=recvmsg(sock->real_fd,&msg,flags);
    if(result>=0){unix_discard_marker(sock,&msg,flags);if(namelen)*namelen=msg.msg_namelen;}
    return result;
}

// Linux AArch64 cmsghdr is 16-byte/8-aligned; Darwin uses 12-byte/4.
// Packet-info payloads have identical fixed-width layouts on these platforms.
static int ip_control_type(int level, int type, bool receiving) {
    if(level==IPPROTO_IP) {
        if(type==(receiving?IP_PKTINFO:IP_PKTINFO_))return receiving?IP_PKTINFO_:IP_PKTINFO;
        if(type==(receiving?IP_RECVTOS:IP_TOS_))return receiving?IP_TOS_:IP_TOS;
        if(type==(receiving?IP_RECVTTL:IP_TTL_))return receiving?IP_TTL_:IP_TTL;
    } else if(level==IPPROTO_IPV6) {
        if(type==(receiving?IPV6_PKTINFO:IPV6_PKTINFO_))return receiving?IPV6_PKTINFO_:IPV6_PKTINFO;
        if(type==(receiving?IPV6_TCLASS:IPV6_TCLASS_))return receiving?IPV6_TCLASS_:IPV6_TCLASS;
    }
    return -1;
}
static size_t ip_control_payload(int level, int type) {
    if(level==IPPROTO_IP && type==IP_PKTINFO_)return 12;
    if(level==IPPROTO_IPV6 && type==IPV6_PKTINFO_)return 20;
    return sizeof(int);
}
static int ip_send_control(const uint8_t *guest, size_t length, struct msghdr *msg, uint8_t *native, size_t capacity) {
    size_t used=0, offset=0;
    while(length-offset>=sizeof(struct cmsghdr_guest)) {
        struct cmsghdr_guest header;memcpy(&header,guest+offset,sizeof header);
        if(header.len<sizeof header || header.len>length-offset)return _EINVAL;
        int type=ip_control_type(header.level,header.type,false);
        if(type<0)return _ENOPROTOOPT;
        size_t payload=header.len-sizeof header, expected=ip_control_payload(header.level,header.type);
        if(payload!=expected && !(header.level==IPPROTO_IP && header.type==IP_TOS_ && payload==1))return _EINVAL;
        size_t space=CMSG_SPACE(payload);if(space>capacity-used)return _ENOBUFS;
        struct cmsghdr *c=(void *)(native+used);c->cmsg_len=CMSG_LEN(payload);c->cmsg_level=header.level;c->cmsg_type=type;
        memcpy(CMSG_DATA(c),guest+offset+sizeof header,payload);used+=space;
        size_t step=(header.len+sizeof(header.len)-1)&~(sizeof(header.len)-1);
        if(step>length-offset)break;
        offset+=step;
    }
    msg->msg_control=native;msg->msg_controllen=used;return 0;
}
static int ip_recv_control(const struct msghdr *msg, addr_t address, size_t capacity, uint_t *length, bool *truncated) {
    uint8_t guest[1024]={0};size_t used=0;
    for(struct cmsghdr *c=CMSG_FIRSTHDR((struct msghdr *)msg);c;c=CMSG_NXTHDR((struct msghdr *)msg,c)) {
        if(c->cmsg_len<CMSG_LEN(0))return _EIO;
        int type=ip_control_type(c->cmsg_level,c->cmsg_type,true);if(type<0)continue;
        size_t payload=c->cmsg_len-CMSG_LEN(0), expected=ip_control_payload(c->cmsg_level,type);
        if(payload!=expected && !(c->cmsg_level==IPPROTO_IP && type==IP_TOS_ && payload==1))continue;
        struct cmsghdr_guest header={.len=sizeof header+payload,.level=c->cmsg_level,.type=type};
        if(!address || capacity-used<sizeof header || sizeof header+payload>sizeof guest-used){*truncated=true;break;}
        size_t copied=sizeof header+payload;if(copied>capacity-used){copied=capacity-used;*truncated=true;}
        header.len=copied;memcpy(guest+used,&header,sizeof header);memcpy(guest+used+sizeof header,CMSG_DATA(c),copied-sizeof header);
        size_t aligned=(copied+sizeof(header.len)-1)&~(sizeof(header.len)-1);
        used+=aligned<=capacity-used?aligned:copied;
        if(*truncated)break;
    }
    if(used && user_write(address,guest,used))return _EFAULT;
    *length=used;return 0;
}

// Guest O_NONBLOCK on sockets is applied to the host descriptor (realfs_setflags).
static bool unix_send_may_block(struct fd *sock, int real_flags) {
    if ((real_flags & MSG_DONTWAIT) || (sock->flags & O_NONBLOCK_))
        return false;
    int host_flags = fcntl(sock->real_fd, F_GETFL, 0);
    return !(host_flags >= 0 && (host_flags & O_NONBLOCK));
}

// XNU rejects an AF_UNIX stream record carrying SCM_RIGHTS that does not fit
// the remaining send buffer with EMSGSIZE (Chromium's Mojo tolerates that only
// in Apple builds). Linux never reports EMSGSIZE here: it writes a prefix or
// reports EAGAIN/blocks. Send the largest halved prefix that fits; the host
// marker, and so the queued guest descriptors, stay with its first byte.
// Returns bytes sent or a Linux error.
static ssize_t unix_stream_send_fitting(struct fd *sock, struct msghdr *msg, int real_flags, bool may_wait) {
    size_t total = 0;
    for (size_t i = 0; i < (size_t) msg->msg_iovlen; i++)
        total += msg->msg_iov[i].iov_len;
    // The caller's full-length attempt already failed; after a wait, retry it.
    size_t first = total / 2;
    for (;;) {
        for (size_t limit = first; limit > 0; limit /= 2) {
            struct iovec part[msg->msg_iovlen];
            size_t parts = 0, left = limit;
            for (size_t i = 0; i < (size_t) msg->msg_iovlen && left > 0; i++) {
                part[parts] = msg->msg_iov[i];
                if (part[parts].iov_len > left)
                    part[parts].iov_len = left;
                left -= part[parts].iov_len;
                parts++;
            }
            struct msghdr prefix = *msg;
            prefix.msg_iov = part;
            prefix.msg_iovlen = (int) parts;
            ssize_t sent = sendmsg(sock->real_fd, &prefix, real_flags);
            if (sent < 0 && errno != EMSGSIZE)
                return errno_map();
            if (sent < 0)
                continue;
            // A blocking Linux stream send completes the whole buffer; the
            // remainder carries no ancillary data.
            while (may_wait && (size_t) sent < total) {
                struct iovec rest[msg->msg_iovlen];
                size_t rests = 0, skip = (size_t) sent;
                for (size_t i = 0; i < (size_t) msg->msg_iovlen; i++) {
                    if (skip >= msg->msg_iov[i].iov_len) { skip -= msg->msg_iov[i].iov_len; continue; }
                    rest[rests].iov_base = (char *) msg->msg_iov[i].iov_base + skip;
                    rest[rests].iov_len = msg->msg_iov[i].iov_len - skip;
                    skip = 0; rests++;
                }
                struct msghdr tail = {.msg_iov = rest, .msg_iovlen = (int) rests};
                ssize_t more = sendmsg(sock->real_fd, &tail, real_flags);
                if (more > 0) { sent += more; continue; }
                if (more < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                    break; // Linux reports the bytes already queued.
                if (sock_wait_for(sock->real_fd, POLLOUT, SO_SNDTIMEO) < 0)
                    break;
            }
            return sent;
        }
        if (!may_wait)
            return _EAGAIN;
        int waited = sock_wait_for(sock->real_fd, POLLOUT, SO_SNDTIMEO);
        if (waited < 0)
            return waited;
        first = total;
    }
}

static int_t sock_sendmsg(struct fd *sock, addr_t msghdr_addr, int_t flags) {
    int err;
    bool send_locked = false;
    struct fd *queued_peer = NULL;
    if (uevent_socket(sock)) return uevent_sendmsg(sock, msghdr_addr, flags);
    if (fd_is_netlink_stub(sock))
        return netlink_stub_sendmsg(sock, msghdr_addr, flags);

    // Read the guest msghdr struct into our internal 32-bit representation
    struct msghdr msg = {0};
    struct msghdr_ msg_fake;
#ifdef GUEST_ARM64
    struct msghdr64_ msg_fake64;
    if (user_get(msghdr_addr, msg_fake64))
        return _EFAULT;
    if (msg_fake64.msg_iovlen > 1024)
        return _EMSGSIZE;
    if (msg_fake64.msg_controllen > UINT32_MAX)
        return _ENOBUFS;
    msg_fake.msg_name = (addr_t)msg_fake64.msg_name;
    msg_fake.msg_namelen = msg_fake64.msg_namelen;
    msg_fake.msg_iov = (addr_t)msg_fake64.msg_iov;
    msg_fake.msg_iovlen = (uint_t)msg_fake64.msg_iovlen;
    msg_fake.msg_control = (addr_t)msg_fake64.msg_control;
    msg_fake.msg_controllen = (uint_t)msg_fake64.msg_controllen;
    msg_fake.msg_flags = msg_fake64.msg_flags;
#else
    if (user_get(msghdr_addr, msg_fake))
        return _EFAULT;
#endif
    if (msg_fake.msg_iovlen > 1024)
        return _EMSGSIZE;
    if (msg_fake.msg_iovlen != 0 && msg_fake.msg_iov == 0)
        return _EFAULT;

    // msg_name
    struct sockaddr_max_ msg_name;
    if (msg_fake.msg_name != 0) {
        int err = sockaddr_read(msg_fake.msg_name, &msg_name, &msg_fake.msg_namelen);
        if (err < 0)
            return err;
        err = sockaddr_map_v4_for_v6(sock, &msg_name, &msg_fake.msg_namelen, _ENETUNREACH);
        if (err < 0)
            return err;
        msg.msg_name = &msg_name;
        msg.msg_namelen = msg_fake.msg_namelen;
    } else {
        msg.msg_name = NULL;
    }

    // msg_iovec
#ifdef GUEST_ARM64
    struct iovec64_ msg_iov_fake64[msg_fake.msg_iovlen];
    if (user_read(msg_fake.msg_iov, msg_iov_fake64, sizeof(msg_iov_fake64)))
        return _EFAULT;
    struct iovec msg_iov[msg_fake.msg_iovlen];
    memset(msg_iov, 0, sizeof(msg_iov));
    msg.msg_iov = msg_iov;
    msg.msg_iovlen = msg_fake.msg_iovlen;
    for (size_t i = 0; i < (size_t) msg.msg_iovlen; i++) {
        msg_iov[i].iov_len = (size_t)msg_iov_fake64[i].len;
        msg_iov[i].iov_base = malloc(msg_iov[i].iov_len);
        if (msg_iov[i].iov_len != 0 && msg_iov[i].iov_base == NULL) {
            err = _ENOMEM;
            goto out_free_iov;
        }
        err = _EFAULT;
        if (user_read((addr_t)msg_iov_fake64[i].base, msg_iov[i].iov_base, msg_iov[i].iov_len))
            goto out_free_iov;
    }
#else
    struct iovec_ msg_iov_fake[msg_fake.msg_iovlen];
    if (user_get(msg_fake.msg_iov, msg_iov_fake))
        return _EFAULT;
    struct iovec msg_iov[msg_fake.msg_iovlen];
    memset(msg_iov, 0, sizeof(msg_iov));
    msg.msg_iov = msg_iov;
    msg.msg_iovlen = sizeof(msg_iov) / sizeof(msg_iov[0]);
    for (size_t i = 0; i < (size_t) msg.msg_iovlen; i++) {
        msg_iov[i].iov_len = msg_iov_fake[i].len;
        msg_iov[i].iov_base = malloc(msg_iov_fake[i].len);
        if (msg_iov[i].iov_len != 0 && msg_iov[i].iov_base == NULL) {
            err = _ENOMEM;
            goto out_free_iov;
        }
        err = _EFAULT;
        if (user_read(msg_iov_fake[i].base, msg_iov[i].iov_base, msg_iov_fake[i].len))
            goto out_free_iov;
    }
#endif

    // Serialize ancillary metadata with the corresponding host byte boundary.
    if (sock->socket.domain == AF_LOCAL_) {
        lock(&sock->socket.unix_send_lock);
        send_locked = true;
    }

    // msg_control
    uint8_t msg_control_buf[2048];
    uint8_t *msg_control = NULL;
    if (msg_fake.msg_control != 0) {
        if (msg_fake.msg_controllen > sizeof(msg_control_buf)) {
            err = _EINVAL;
            goto out_free_iov;
        }
        msg_control = msg_control_buf;
        err = _EFAULT;
        if (user_read(msg_fake.msg_control, msg_control, msg_fake.msg_controllen))
            goto out_free_iov;
    }
    msg.msg_control = NULL;
    msg.msg_controllen = 0;

    struct scm *scm = NULL;
    bool passcred = false;
    if (sock->socket.domain == AF_LOCAL_) {
        lock(&peer_lock);
        if (sock->socket.unix_peer) passcred = sock->socket.unix_peer->socket.unix_passcred;
        unlock(&peer_lock);
    }
    union { struct cmsghdr align; uint8_t bytes[2048]; } ip_control = {0};
    if (sock->socket.domain != AF_LOCAL_ && msg_control) {
        err=ip_send_control(msg_control,msg_fake.msg_controllen,&msg,ip_control.bytes,sizeof ip_control.bytes);
        if(err<0)goto out_free_iov;
    }
    char real_msg_control[CMSG_SPACE(sizeof(int))]; // only used if actually sending an fd
    if (sock->socket.domain == AF_LOCAL_ && (passcred || (msg_control != NULL && msg_fake.msg_controllen >= sizeof(struct cmsghdr_guest)))) {
        // figure out how many file descriptors we're sending
        uint8_t *mhdr_end = msg_control ? msg_control + msg_fake.msg_controllen : NULL;
        unsigned num_fds = 0;
        bool explicit_credentials = false;
        struct ucred_ credentials = {0};
        struct cmsghdr_guest *cmsg;
        for (cmsg = (void *) msg_control; cmsg != NULL; cmsg = CMSG_NXTHDR_GUEST(cmsg, mhdr_end)) {
            size_t remaining = (size_t)(mhdr_end - (uint8_t *)cmsg);
            if (cmsg->len < sizeof(struct cmsghdr_guest) || cmsg->len > remaining) {
                err = _EINVAL;
                goto out_free_iov;
            }
            if (cmsg->level != SOL_SOCKET_)
                continue;
            if (cmsg->type == SCM_CREDENTIALS_) {
                if (cmsg->len != sizeof(struct cmsghdr_guest) + sizeof(credentials)) {
                    err = _EINVAL;
                    goto out_free_iov;
                }
                memcpy(&credentials, cmsg->data, sizeof(credentials));
                // Validate guest identity (in the caller's namespaces), then store
                // global ids; never forward guest credentials to XNU.
                // Capability-authorized identity overrides are not implemented.
                struct ucred_ own = cred_for_receiver((struct ucred_) {current->tgid, current->euid, current->egid});
                uid_t_ vu = userns_view_uid(current->userns, current->uid), vsu = userns_view_uid(current->userns, current->suid);
                uid_t_ vg = userns_view_gid(current->userns, current->gid), vsg = userns_view_gid(current->userns, current->sgid);
                if (credentials.pid != own.pid ||
                        (credentials.uid != vu && credentials.uid != own.uid && credentials.uid != vsu) ||
                        (credentials.gid != vg && credentials.gid != own.gid && credentials.gid != vsg)) {
                    err = _EPERM;
                    goto out_free_iov;
                }
                credentials.pid = current->tgid;
                credentials.uid = credentials.uid == vu ? current->uid : credentials.uid == own.uid ? current->euid : current->suid;
                credentials.gid = credentials.gid == vg ? current->gid : credentials.gid == own.gid ? current->egid : current->sgid;
                explicit_credentials = true;
                continue;
            }
            if (cmsg->type != SCM_RIGHTS_ ||
                    (cmsg->len - sizeof(struct cmsghdr_guest)) % sizeof(fd_t) != 0) {
                err = _EINVAL;
                goto out_free_iov;
            }
            num_fds += (cmsg->len - sizeof(struct cmsghdr_guest)) / sizeof(fd_t);
        }
        if (num_fds > 253) { // *magic*
            err = _EINVAL;
            goto out_free_iov;
        }

        if (num_fds > 0 || passcred) {
            // send one (1) real fd and put the rest in a struct scm
            int real_fd = sock->real_fd;
            msg.msg_control = real_msg_control;
            msg.msg_controllen = sizeof(real_msg_control);
            struct cmsghdr *real_cmsg = CMSG_FIRSTHDR(&msg);
            real_cmsg->cmsg_level = SOL_SOCKET;
            real_cmsg->cmsg_type = SCM_RIGHTS;
            real_cmsg->cmsg_len = CMSG_LEN(sizeof(real_fd));
            memcpy(CMSG_DATA(real_cmsg), &real_fd, sizeof(real_fd));

            scm = malloc(sizeof(struct scm) + num_fds * sizeof(struct fd *));
            if (scm == NULL) {
                err = _ENOMEM;
                goto out_free_iov;
            }
            list_init(&scm->queue);
            scm->num_fds = 0;
            scm->has_credentials = passcred;
            fill_cred(&scm->credentials);
            if (explicit_credentials) scm->credentials = credentials;
            unsigned fd_i = 0;
            for (cmsg = (void *) msg_control; cmsg != NULL; cmsg = CMSG_NXTHDR_GUEST(cmsg, mhdr_end)) {
                size_t remaining = (size_t)(mhdr_end - (uint8_t *)cmsg);
                if (cmsg->len < sizeof(struct cmsghdr_guest) || cmsg->len > remaining) {
                    err = _EINVAL;
                    goto out_free_scm;
                }
                if (cmsg->level != SOL_SOCKET_ || cmsg->type != SCM_RIGHTS_)
                    continue;
                fd_t *fds = (void *) cmsg->data;
                for (unsigned i = 0; i < (cmsg->len - sizeof(struct cmsghdr_guest)) / sizeof(fd_t); i++) {
                    STRACE(" sending fd %d", fds[i]);
                    struct fd *send_fd = f_get_retain(fds[i]);
                    if (send_fd == NULL) {
                        err = _EBADF;
                        goto out_free_scm;
                    }
                    scm->fds[fd_i++] = send_fd;
                    scm->num_fds = fd_i;
                }
            }
            lock(&peer_lock);
            struct fd *peer = sock->socket.unix_peer;
            if (peer == NULL) {
                unlock(&peer_lock);
                err = _EPIPE;
                goto out_free_scm;
            }
            queued_peer = fd_retain(peer);
            lock(&peer->lock);
            list_add_tail(&peer->socket.unix_scm, &scm->queue);
            unlock(&peer->lock);
            unlock(&peer_lock);
        }
    }

    // Linux ignores input msghdr.msg_flags for sendmsg; syscall flags govern it.
    msg.msg_flags = 0;
    err = _EINVAL;
    if (msg.msg_flags < 0)
        goto out_free_scm;
    int real_flags = sock_flags_to_real(flags);
    if (real_flags < 0)
        goto out_free_scm;

    bool has_payload = false;
    for (size_t i = 0; i < msg.msg_iovlen; i++)
        if (msg_iov[i].iov_len != 0) { has_payload = true; break; }
    if (sock->socket.domain == AF_LOCAL_ && sock->socket.type == SOCK_STREAM_ && !has_payload) {
        err = 0;
        goto out_free_scm;
    }

    err = sendmsg(sock->real_fd, &msg, real_flags);
    if (err < 0 && errno == EMSGSIZE && sock->socket.domain == AF_LOCAL_ &&
            sock->socket.type == SOCK_STREAM_ && msg.msg_control != NULL) {
        err = (int) unix_stream_send_fitting(sock, &msg, real_flags, unix_send_may_block(sock, real_flags));
        if (err < 0)
            goto out_free_scm;
    } else if (err < 0) {
        err = errno_map();
        goto out_free_scm;
    }
    // Stream ancillary data needs an actual byte boundary. A zero-byte send
    // must not leave metadata for the next unrelated native ancillary marker.
    if (err == 0 && sock->socket.type == SOCK_STREAM_)
        goto out_free_scm;
    goto out_free_iov;

out_free_scm:
    if (scm != NULL) {
        if (queued_peer != NULL) {
            lock(&queued_peer->lock);
            list_remove_safe(&scm->queue);
            unlock(&queued_peer->lock);
        }
        scm_free(scm);
    }
out_free_iov:
    if (queued_peer != NULL) fd_close(queued_peer);
    if (send_locked) unlock(&sock->socket.unix_send_lock);
    for (size_t i = 0; i < (size_t) msg.msg_iovlen; i++)
        free(msg_iov[i].iov_base);
    return err;
}

int_t sys_sendmsg(fd_t sock_fd, addr_t msghdr_addr, int_t flags) {
    STRACE("sendmsg(%d, %#x, %d)", sock_fd, msghdr_addr, flags);
    int error;
    struct fd *sock = sock_getfd_retain(sock_fd, &error);
    if (sock == NULL) return error;
    bool saved = current->send_nosignal;
    current->send_nosignal = saved || (flags & MSG_NOSIGNAL_);
    int_t result = sock_sendmsg(sock, msghdr_addr, flags);
    current->send_nosignal = saved;
    fd_close(sock);
    return result;
}

static int_t sock_recvmsg(struct fd *sock, addr_t msghdr_addr, int_t flags) {
    if (uevent_socket(sock)) return uevent_recvmsg(sock, msghdr_addr, flags);
    if (fd_is_netlink_stub(sock))
        return netlink_stub_recvmsg(sock, msghdr_addr, flags);

    // Read the guest msghdr struct into our internal 32-bit representation
    struct msghdr_ msg_fake;
#ifdef GUEST_ARM64
    struct msghdr64_ msg_fake64;
    if (user_get(msghdr_addr, msg_fake64))
        return _EFAULT;
    if (msg_fake64.msg_iovlen > 1024)
        return _EMSGSIZE;
    if (msg_fake64.msg_controllen > UINT32_MAX)
        return _ENOBUFS;
    msg_fake.msg_name = (addr_t)msg_fake64.msg_name;
    msg_fake.msg_namelen = msg_fake64.msg_namelen;
    msg_fake.msg_iov = (addr_t)msg_fake64.msg_iov;
    msg_fake.msg_iovlen = (uint_t)msg_fake64.msg_iovlen;
    msg_fake.msg_control = (addr_t)msg_fake64.msg_control;
    msg_fake.msg_controllen = (uint_t)msg_fake64.msg_controllen;
    msg_fake.msg_flags = msg_fake64.msg_flags;
#else
    if (user_get(msghdr_addr, msg_fake))
        return _EFAULT;
#endif
    if (msg_fake.msg_iovlen > 1024)
        return _EMSGSIZE;
    if (msg_fake.msg_iovlen != 0 && msg_fake.msg_iov == 0)
        return _EFAULT;

    struct msghdr msg = {0};

    // msg_name
    struct sockaddr_max_ msg_name;
    uint_t msg_name_buffer_len = msg_fake.msg_namelen;
    if (msg_fake.msg_name != 0) {
        msg.msg_name = &msg_name;
        msg.msg_namelen = msg_name_buffer_len;
        if (msg.msg_namelen > sizeof(msg_name))
            msg.msg_namelen = sizeof(msg_name);
    } else {
        msg.msg_name = NULL;
        msg.msg_namelen = 0;
    }

    union { struct cmsghdr align; char bytes[1024]; } control_storage = {0};
    char *real_msg_control = control_storage.bytes;
    if (msg_fake.msg_controllen != 0 || sock->socket.domain == AF_LOCAL_) {
        // msg_control, include room for one (1) fd
        msg.msg_control = real_msg_control;
        msg.msg_controllen = sizeof(control_storage.bytes);
    } else {
        msg.msg_control = NULL;
        msg.msg_controllen = 0;
    }

    int real_flags = sock_flags_to_real(flags & ~0x40000000);
    if (real_flags < 0)
        return _EINVAL;

    // msg_iovec (no initial content)
#ifdef GUEST_ARM64
    struct iovec64_ msg_iov_fake64[msg_fake.msg_iovlen];
    if (user_read(msg_fake.msg_iov, msg_iov_fake64, sizeof(msg_iov_fake64)))
        return _EFAULT;
    struct iovec msg_iov[msg_fake.msg_iovlen];
    msg.msg_iov = msg_iov;
    msg.msg_iovlen = msg_fake.msg_iovlen;
    addr_t msg_iov_bases[msg_fake.msg_iovlen]; // save guest base addrs for writeback
    for (size_t i = 0; i < (size_t) msg.msg_iovlen; i++) {
        msg_iov_bases[i] = (addr_t)msg_iov_fake64[i].base;
        msg_iov[i].iov_len = (size_t)msg_iov_fake64[i].len;
        msg_iov[i].iov_base = malloc(msg_iov[i].iov_len);
        if (msg_iov[i].iov_len != 0 && msg_iov[i].iov_base == NULL) {
            for (size_t j = 0; j < i; j++)
                free(msg_iov[j].iov_base);
            return _ENOMEM;
        }
    }
#else
    struct iovec_ msg_iov_fake[msg_fake.msg_iovlen];
    if (user_get(msg_fake.msg_iov, msg_iov_fake))
        return _EFAULT;
    struct iovec msg_iov[msg_fake.msg_iovlen];
    msg.msg_iov = msg_iov;
    msg.msg_iovlen = sizeof(msg_iov) / sizeof(msg_iov[0]);
    for (size_t i = 0; i < (size_t) msg.msg_iovlen; i++) {
        msg_iov[i].iov_len = msg_iov_fake[i].len;
        msg_iov[i].iov_base = malloc(msg_iov_fake[i].len);
        if (msg_iov[i].iov_len != 0 && msg_iov[i].iov_base == NULL) {
            for (size_t j = 0; j < i; j++)
                free(msg_iov[j].iov_base);
            return _ENOMEM;
        }
    }
#endif

    // Same iOS-safe wait pattern as sys_recvfrom: avoid unbounded blocking
    // in host recvmsg() so guest signals can always be delivered. But only
    // wait when the caller actually wants blocking behavior — if the fd is
    // nonblock (SOCK_NONBLOCK set by guest or via fcntl), surface EAGAIN
    // immediately instead of waiting. This is critical for musl's
    // res_msend, which creates nonblock sockets and calls
    // recvmsg(fd, &mh, 0) — it relies on EAGAIN on empty buffer to end its
    // "drain pending responses" loop. Same bug that used to affect
    // sys_recvfrom before 76907b86.
    int host_flags_rm = fcntl(sock->real_fd, F_GETFL, 0);
    bool host_nonblock_rm = (host_flags_rm >= 0) && (host_flags_rm & O_NONBLOCK);
    bool guest_nonblock_rm = (sock->flags & O_NONBLOCK_) != 0;
    bool should_wait_rm = !(real_flags & MSG_DONTWAIT) && !guest_nonblock_rm && !host_nonblock_rm;

    ssize_t res = -1;
    int err = 0;
    if (should_wait_rm) {
        for (;;) {
            res = recvmsg(sock->real_fd, &msg, real_flags | MSG_DONTWAIT);
            if (res >= 0)
                break;
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                err = errno_map();
                break;
            }
            int wr = sock_wait_readable(sock->real_fd);
            if (wr < 0) {
                err = wr;
                break;
            }
        }
    } else {
        res = recvmsg(sock->real_fd, &msg, real_flags);
        if (res < 0)
            err = errno_map();
    }
    // don't return err quite yet, there are outstanding mallocs

    // msg_iovec (changed)
    // copy as many bytes as were received, according to the return value
    size_t n = res;
    if (res < 0)
        n = 0;
    for (size_t i = 0; i < (size_t) msg.msg_iovlen; i++) {
        size_t chunk_size = msg_iov[i].iov_len;
        if (chunk_size > n)
            chunk_size = n;
        if (chunk_size > 0) {
#ifdef GUEST_ARM64
            if (user_write(msg_iov_bases[i], msg_iov[i].iov_base, chunk_size))
#else
            if (user_write(msg_iov_fake[i].base, msg_iov[i].iov_base, chunk_size))
#endif
            {
                err = _EFAULT;
                n = chunk_size;
            }
        }
        n -= chunk_size;
        free(msg_iov[i].iov_base);
    }
    if (err < 0 && res >= 0) {
        // The native receive already consumed its byte/ancillary boundary.
        // Dispose both its host marker and guest metadata even on copy faults;
        // otherwise the next message can receive the previous message's FD.
        if (sock->socket.domain == AF_LOCAL_)
            unix_discard_marker(sock, &msg, real_flags);
        return err;
    }
    if (res < 0)
        return err;

    // msg_control (changed)
    uint_t msg_control_buffer_len = msg_fake.msg_controllen;
    msg_fake.msg_controllen = 0;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (sock->socket.domain == AF_LOCAL_ && cmsg != NULL &&
            cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        int dummy_fd = ((int *) CMSG_DATA(cmsg))[0];
        close(dummy_fd);

        lock(&sock->lock);
        if (list_empty(&sock->socket.unix_scm)) {
            unlock(&sock->lock);
            return _EINVAL;
        }
        struct scm *scm = list_first_entry(&sock->socket.unix_scm, struct scm, queue);
        if (flags & MSG_PEEK_) {
            struct scm *copy = malloc(sizeof(*copy)+scm->num_fds*sizeof(struct fd *));
            if (!copy) { unlock(&sock->lock); return _ENOMEM; }
            memcpy(copy,scm,sizeof(*copy)+scm->num_fds*sizeof(struct fd *));
            for(unsigned i=0;i<copy->num_fds;i++)fd_retain(copy->fds[i]);
            scm=copy;
        } else list_remove(&scm->queue);
        unlock(&sock->lock);

        if (res < 0) {
            scm_free(scm);
            return err;
        }

        if (scm->has_credentials) { sock->socket.unix_last_cred=scm->credentials;sock->socket.unix_last_cred_valid=true; }
        uint8_t control[2048] = {0};
        size_t used = 0;
        int extra_flags = 0;
        if (scm->has_credentials && sock->socket.unix_passcred) {
            size_t length = sizeof(struct cmsghdr_guest) + sizeof(struct ucred_);
            size_t aligned = (length + sizeof(addr_t)-1) & ~(sizeof(addr_t)-1);
            if (msg_fake.msg_control && msg_control_buffer_len >= length) {
                struct cmsghdr_guest *out = (void *)control;
                out->len = length; out->level = SOL_SOCKET_; out->type = SCM_CREDENTIALS_;
                struct ucred_ seen = cred_for_receiver(scm->credentials);
                memcpy(out->data, &seen, sizeof seen);
                used = aligned <= msg_control_buffer_len ? aligned : length;
            } else extra_flags |= MSG_CTRUNC_;
        }
        unsigned room = msg_control_buffer_len > used + sizeof(struct cmsghdr_guest)
            ? (msg_control_buffer_len-used-sizeof(struct cmsghdr_guest))/sizeof(fd_t) : 0;
        if (!msg_fake.msg_control) room = 0;
        unsigned count = scm->num_fds < room ? scm->num_fds : room;
        if (count < scm->num_fds) extra_flags |= MSG_CTRUNC_;
        fd_t installed[253]; unsigned installed_count = 0;
        if (count) {
            struct cmsghdr_guest *out = (void *)(control + used);
            out->len = sizeof(*out) + count*sizeof(fd_t); out->level = SOL_SOCKET_; out->type = SCM_RIGHTS_;
            fd_t *fds = (void *)out->data;
            for (unsigned i = 0; i < count; i++) {
                fd_t f = f_install(scm->fds[i], (flags & 0x40000000) ? O_CLOEXEC_ : 0);
                scm->fds[i] = NULL;
                if (f < 0) {
                    // Linux delivers the payload and as many rights as fit,
                    // discarding excess rights with MSG_CTRUNC. Do not turn
                    // NOFILE exhaustion into an error after consuming data.
                    extra_flags |= MSG_CTRUNC_;
                    break;
                }
                fds[installed_count] = f;
                installed[installed_count++] = f;
            }
            if (installed_count) {
                out->len = sizeof(*out) + installed_count*sizeof(fd_t);
                size_t space = (out->len + sizeof(addr_t)-1) & ~(sizeof(addr_t)-1);
                size_t available = msg_control_buffer_len - used;
                used += space <= available ? space : out->len;
            }
        }
        scm_free(scm);
        if (used && user_write(msg_fake.msg_control, control, used)) {
            for (unsigned i = 0; i < installed_count; i++) sys_close(installed[i]);
            return _EFAULT;
        }
        msg_fake.msg_controllen = used;
        if (extra_flags) msg.msg_flags |= MSG_CTRUNC;
    }

    if (res > 0 && sock->socket.domain == AF_LOCAL_ && sock->socket.unix_passcred && !msg_fake.msg_controllen && cmsg == NULL) {
        struct { struct cmsghdr_guest header; struct ucred_ cred; } control = {0};
        control.header.len=sizeof(struct cmsghdr_guest)+sizeof(struct ucred_);control.header.level=SOL_SOCKET_;control.header.type=SCM_CREDENTIALS_;
        control.cred = sock->socket.unix_last_cred_valid ? cred_for_receiver(sock->socket.unix_last_cred) : (struct ucred_){.pid=0,.uid=65534,.gid=65534};
        if(msg_fake.msg_control && msg_control_buffer_len>=control.header.len) {
            if(user_write(msg_fake.msg_control,&control,control.header.len))return _EFAULT;
            msg_fake.msg_controllen=control.header.len;
        } else msg.msg_flags |= MSG_CTRUNC;
    }
    if (sock->socket.domain == AF_INET_ || sock->socket.domain == AF_INET6_) {
        bool truncated=false;
        err=ip_recv_control(&msg,msg_fake.msg_control,msg_control_buffer_len,&msg_fake.msg_controllen,&truncated);
        if(err<0)return err;
        if(truncated)msg.msg_flags|=MSG_CTRUNC;
    }
    // by now the iovecs and scm have been freed so we can return
    if (res < 0)
        return err;

    // msg_name (changed)
    if (msg.msg_name != 0) {
        int err = sockaddr_write(msg_fake.msg_name, msg.msg_name, msg_name_buffer_len, &msg.msg_namelen);
        if (err < 0)
            return err;
    }
    msg_fake.msg_namelen = msg.msg_namelen;

    // msg_flags (changed)
    msg_fake.msg_flags = sock_flags_from_real(msg.msg_flags);

    // Write back the updated msghdr to guest memory
#ifdef GUEST_ARM64
    msg_fake64.msg_namelen = msg_fake.msg_namelen;
    msg_fake64.msg_controllen = msg_fake.msg_controllen;
    msg_fake64.msg_flags = msg_fake.msg_flags;
    if (user_put(msghdr_addr, msg_fake64))
        return _EFAULT;
#else
    if (user_put(msghdr_addr, msg_fake))
        return _EFAULT;
#endif
    return res;
}

int_t sys_recvmsg(fd_t sock_fd, addr_t msghdr_addr, int_t flags) {
    STRACE("recvmsg(%d, %#x, %d)", sock_fd, msghdr_addr, flags);
    int error;
    struct fd *sock = sock_getfd_retain(sock_fd, &error);
    if (sock == NULL) return error;
    int_t result = sock_recvmsg(sock, msghdr_addr, flags);
    fd_close(sock);
    return result;
}

struct mmsghdr_ {
    struct msghdr_ hdr;
    uint_t len;
};

#ifdef GUEST_ARM64
struct mmsghdr64_ {
    struct msghdr64_ hdr;
    uint32_t len;
    uint32_t _pad;
};
#endif

int_t sys_recvmmsg(fd_t sock_fd, addr_t msg_vec, uint_t vec_len, int_t flags, addr_t timeout_addr) {
    STRACE("recvmmsg(%d, %#x, %d, %d)", sock_fd, msg_vec, vec_len, flags);
    (void) timeout_addr;
    int num_recv = 0;
#ifdef GUEST_ARM64
    size_t mmsghdr_size = sizeof(struct mmsghdr64_);
    size_t len_offset = offsetof(struct mmsghdr64_, len);
#else
    size_t mmsghdr_size = sizeof(struct mmsghdr_);
    size_t len_offset = offsetof(struct mmsghdr_, len);
#endif
    for (unsigned i = 0; i < vec_len; i++) {
        addr_t msghdr = msg_vec + i * mmsghdr_size;
        int_t res = sys_recvmsg(sock_fd, msghdr, flags);
        if (res >= 0) {
            addr_t msg_len_addr = msghdr + len_offset;
            if (user_put(msg_len_addr, res))
                res = _EFAULT;
        }
        if (res < 0) {
            if (num_recv > 0)
                break;
            return res;
        }
        num_recv++;
        if (res == 0)
            break;
    }
    return num_recv;
}

int_t sys_sendmmsg(fd_t sock_fd, addr_t msg_vec, uint_t vec_len, int_t flags) {
    int num_sent = 0;
#ifdef GUEST_ARM64
    size_t mmsghdr_size = sizeof(struct mmsghdr64_);
    size_t len_offset = offsetof(struct mmsghdr64_, len);
#else
    size_t mmsghdr_size = sizeof(struct mmsghdr_);
    size_t len_offset = offsetof(struct mmsghdr_, len);
#endif
    for (unsigned i = 0; i < vec_len; i++) {
        addr_t msghdr = msg_vec + i * mmsghdr_size;
        int_t res = sys_sendmsg(sock_fd, msghdr, flags);
        if (res >= 0) {
            addr_t msg_len_addr = msghdr + len_offset;
            if (user_put(msg_len_addr, res))
                res = _EFAULT;
        }
        if (res < 0) {
            // From the man page:
            // If an error occurs after at least one message has been sent, the
            // call succeeds, and returns the number of messages sent.  The
            // error code is lost.
            if (num_sent > 0)
                break;
            return res;
        }
        num_sent++;
        if (res == 0) {
            // This means the socket is non-blocking and can't be written to anymore.
            break;
        }
    }
    return num_sent;
}

static void sock_translate_err(struct fd *fd, int *err) {
    // on ios, when the device goes to sleep, all connected sockets are killed.
    // reads/writes return ENOTCONN, which I'm pretty sure is a violation of
    // posix. so instead, detect this and return ECONNRESET.
    if (*err == _ENOTCONN) {
        struct sockaddr addr;
        socklen_t len = sizeof(addr);
        if (getpeername(fd->real_fd, &addr, &len) < 0 && errno == EINVAL) {
            *err = _ECONNRESET;
        }
    }
}

// sock_read / sock_write: behave exactly like realfs_read / realfs_write
// (a single host read/write call, with no poll and no flag change), except
// that EINTR is handled specially: if there is a guest signal pending, we
// surface EINTR back to the guest instead of blindly retrying.
//
// The original realfs_read/realfs_write does `while (errno == EINTR) retry`,
// which swallows the SIGUSR1 wake iSH uses to deliver guest signals into
// blocked host syscalls. Combined with iOS occasionally deferring signal
// delivery (thread suspension on backgrounding), this causes indefinite
// hangs on any blocking socket read/write (git HTTPS, nc, telnet, etc.).
//
// We intentionally do NOT poll, do NOT mess with nonblock flags, and do NOT
// switch to recv/send. This minimizes regression risk — the host I/O path
// is byte-for-byte identical to the original implementation for every case
// except "host syscall got EINTR AND guest has a pending signal", which is
// precisely the bug we need to fix.
static ssize_t sock_read(struct fd *fd, void *buf, size_t size) {
    if (uevent_socket(fd)) return uevent_read(fd, buf, size);
    if (fd_is_netlink_stub(fd)) return netlink_receive(fd, buf, size, 0);
    int err;
    int eintr_count = 0;
    for (;;) {
        ssize_t res = fd->socket.domain == AF_LOCAL_ && fd->socket.unix_passcred
            ? unix_plain_recv(fd,buf,size,0,NULL,NULL) : read(fd->real_fd, buf, size);
        if (res >= 0) { err = (int)res; break; }
        if (errno != EINTR) { err = errno_map(); break; }
        eintr_count++;
        // EINTR: only surface to guest if there is actually a pending signal
        // that the guest wants to handle. Otherwise retry (matches the
        // original realfs_read behavior for spurious EINTR).
        if (current->sighand != NULL) {
            lock(&current->sighand->lock);
            bool pending = !!(current->pending & ~current->blocked);
            unlock(&current->sighand->lock);
            if (pending) { err = _EINTR; break; }
        }
        // no guest signal — retry the host read
    }
    if (eintr_count >= 3) {
        printk("NETDIAG sock_read-eintr(pid=%d comm=%s fd=%d) count=%d err=%d\n",
               current->pid, current->comm, fd->real_fd, eintr_count, err);
    }
    sock_translate_err(fd, &err);
    return err;
}

static ssize_t sock_write(struct fd *fd, const void *buf, size_t size) {
    if (uevent_socket(fd)) return uevent_write(fd, buf, size);
    if (fd_is_netlink_stub(fd)) return netlink_stub_reply(fd, buf, size);
    int err;
    int eintr_count = 0;
    for (;;) {
        bool handled = false;
        ssize_t res = unix_credential_send(fd,buf,size,0,&handled);
        if (handled) { err = (int)res; break; }
        res = write(fd->real_fd, buf, size);
        if (res >= 0) { err = (int)res; break; }
        if (errno != EINTR) { err = errno_map(); break; }
        eintr_count++;
        if (current->sighand != NULL) {
            lock(&current->sighand->lock);
            bool pending = !!(current->pending & ~current->blocked);
            unlock(&current->sighand->lock);
            if (pending) { err = _EINTR; break; }
        }
    }
    if (eintr_count >= 3) {
        printk("NETDIAG sock_write-eintr(pid=%d comm=%s fd=%d) count=%d err=%d\n",
               current->pid, current->comm, fd->real_fd, eintr_count, err);
    }
    sock_translate_err(fd, &err);
    return err;
}

static int sock_close(struct fd *fd) {
    if (uevent_socket(fd)) uevent_close(fd);
    static _Thread_local unsigned trace_closed;
    if (g_trace_syscalls && fd->socket.domain == AF_LOCAL_ && trace_closed++ < 128)
        printk("UNIX_FD_CLOSE tid=%d native=%d kind=%d\n", current->pid, fd->real_fd, fd->socket.type);
    if (fd_is_netlink_stub(fd)) {
        lock(&netlink_lock);
        list_remove(&fd->socket.netlink_link);
        while (fd->socket.netlink_replies) {
            struct lr_netlink_replies *entry = fd->socket.netlink_replies;
            fd->socket.netlink_replies = entry->next;
            free(entry);
        }
        unlock(&netlink_lock);
        close(fd->socket.netlink_peer_fd);
    }

    sockrestart_end_listen(fd);
    // FIXME next 3 lines should go in a function like release_unix_names
    inode_release_if_exist(fd->socket.unix_name_inode);
    if (fd->socket.unix_name_abstract != NULL)
        unix_abstract_release(fd->socket.unix_name_abstract);
    lock(&peer_lock);
    struct fd *peer = fd->socket.unix_peer;
    if (peer != NULL) {
        peer->socket.unix_peer = NULL;
        if (fd->socket.type == SOCK_STREAM_ || fd->socket.type == SOCK_SEQPACKET_)
            peer->socket.unix_shutdown = 3;
    }
    unlock(&peer_lock);
    if (fd->socket.domain == AF_LOCAL_) {
        lock(&fd->lock);
        struct scm *scm, *tmp;
        list_for_each_entry_safe(&fd->socket.unix_scm, scm, tmp, queue) {
            list_remove(&scm->queue);
            scm_free(scm);
        }
        unlock(&fd->lock);
    }
    return realfs_close(fd);
}

// Read-only Linux interface ABI view; never pass Linux ioctl numbers to XNU.
// Gated with the existing route-netlink option. Host interface names/indices
// match the existing netlink snapshots, rather than inventing guest hardware.
struct lr_ifreq { char name[16]; unsigned char value[24]; };
#ifdef GUEST_ARM64
struct lr_ifconf { int32_t len, pad; uint64_t buffer; };
#define LR_IFREQ_SIZE 40
#else
struct lr_ifconf { int32_t len; uint32_t buffer; };
#define LR_IFREQ_SIZE 32
#endif
static bool socket_interface_command(int cmd) {
    switch(cmd){case 0x8910:case 0x8912:case 0x8913:case 0x8915:case 0x8917:
        case 0x8919:case 0x891b:case 0x8921:case 0x8933:case 0x8946:return true;default:return false;}
}
static uint16_t interface_linux_flags(unsigned flags) {
    uint16_t out=0;
    if(flags&IFF_UP)out|=1;if(flags&IFF_BROADCAST)out|=2;
    if(flags&IFF_LOOPBACK)out|=8;if(flags&IFF_POINTOPOINT)out|=16;
    if(flags&IFF_RUNNING)out|=64;if(flags&IFF_NOARP)out|=128;
    if(flags&IFF_PROMISC)out|=256;if(flags&IFF_MULTICAST)out|=4096;
    return out;
}
static void interface_ipv4(struct lr_ifreq *out,const struct sockaddr *address) {
    uint16_t family=AF_INET_;memcpy(out->value,&family,2);
    const struct sockaddr_in *in=(const void*)address;
    memcpy(out->value+2,&in->sin_port,2);memcpy(out->value+4,&in->sin_addr,4);
}
static ssize_t socket_ioctl_size(int cmd) {
    if(ish_netlink_stub_enabled()&&socket_interface_command(cmd))
        return cmd==0x8912?sizeof(struct lr_ifconf):LR_IFREQ_SIZE;
    return realfs_ioctl_size(cmd);
}
static int socket_ioctl(struct fd *fd,int cmd,void *arg) {
    if(uevent_socket(fd)&&cmd==FIONREAD_){*(int *)arg=uevent_available(fd);return 0;}
    if(!ish_netlink_stub_enabled()||!socket_interface_command(cmd))return realfs_ioctl(fd,cmd,arg);
    if(cmd==0x8910){ // SIOCGIFNAME: index -> name.
        struct lr_ifreq *req=arg;uint32_t index;memcpy(&index,req->value,4);
        char name[IF_NAMESIZE];if(!if_indextoname(index,name))return _ENODEV;
        memset(req->name,0,16);strncpy(req->name,name,15);return 0;
    }
    struct ifaddrs *all;if(getifaddrs(&all)<0)return errno_map();
    int error=0;
    if(cmd==0x8912){ // SIOCGIFCONF: bounded array of guest ifreq, IPv4 only.
        struct lr_ifconf *conf=arg;
        if(conf->len<0){freeifaddrs(all);return _EINVAL;}
        size_t used=0,capacity=conf->buffer?(size_t)conf->len:SIZE_MAX;
        for(struct ifaddrs *p=all;p;p=p->ifa_next){
            if(!p->ifa_name||!p->ifa_addr||p->ifa_addr->sa_family!=AF_INET)continue;
            if(LR_IFREQ_SIZE>capacity-used)break;
            struct lr_ifreq req={0};strncpy(req.name,p->ifa_name,15);interface_ipv4(&req,p->ifa_addr);
            if(conf->buffer&&user_write((addr_t)conf->buffer+used,&req,LR_IFREQ_SIZE)){error=_EFAULT;break;}
            used+=LR_IFREQ_SIZE;
        }
        if(!error)conf->len=used;
    }else{
        struct lr_ifreq *req=arg;char name[16];memcpy(name,req->name,16);name[15]=0;
        struct ifaddrs *found=NULL;
        for(struct ifaddrs *p=all;p;p=p->ifa_next)if(p->ifa_name&&!strcmp(p->ifa_name,name)){
            if(!found)found=p;if(p->ifa_addr&&p->ifa_addr->sa_family==AF_INET){found=p;break;}}
        if(!found)error=_ENODEV;
        else if(cmd==0x8933){uint32_t index=if_nametoindex(name);if(!index)error=_ENODEV;else memcpy(req->value,&index,4);}
        else if(cmd==0x8913){uint16_t flags=interface_linux_flags(found->ifa_flags);memcpy(req->value,&flags,2);}
        else if(cmd==0x8946){ // SIOCETHTOOL: ifr_data points at a struct starting with u32 cmd.
            // Host interfaces have no Linux ethtool driver. Link state (GLINK) is
            // the host's real carrier flag; everything else is EOPNOTSUPP, as
            // Linux reports for a device without ethtool operations (libpcap
            // and ethtool treat that as "not available").
            uint64_t data=0;memcpy(&data,req->value,sizeof(addr_t));uint32_t ecmd;
            if(!data||user_get((addr_t)data,ecmd))error=_EFAULT;
            else if(ecmd==0x0000000a){struct{uint32_t cmd,data;}value={ecmd,(found->ifa_flags&IFF_RUNNING)?1:0};
                if(user_put((addr_t)data,value))error=_EFAULT;}
            else error=_EOPNOTSUPP;}
        else if(cmd==0x8921){struct ifreq host={0};strncpy(host.ifr_name,name,sizeof(host.ifr_name)-1);
            int ctl=socket(AF_INET,SOCK_DGRAM,0);if(ctl<0)error=errno_map();else{if(ioctl(ctl,SIOCGIFMTU,&host)<0)error=errno_map();else{int32_t mtu=host.ifr_mtu;memcpy(req->value,&mtu,4);}close(ctl);}}
        else{const struct sockaddr *sa=NULL;
            if(cmd==0x8915)sa=found->ifa_addr;else if(cmd==0x891b)sa=found->ifa_netmask;
            else if(cmd==0x8917&&(found->ifa_flags&IFF_POINTOPOINT))sa=found->ifa_dstaddr;
            else if(cmd==0x8919&&(found->ifa_flags&IFF_BROADCAST))sa=found->ifa_broadaddr;
            if(!sa||sa->sa_family!=AF_INET)error=_EADDRNOTAVAIL;else{memset(req->value,0,16);interface_ipv4(req,sa);}}
    }
    freeifaddrs(all);return error;
}

static int sock_poll(struct fd *fd) {
    int events = realfs_poll(fd);
    if (uevent_socket(fd)) return events | uevent_poll_error(fd);
    if (fd->socket.domain != AF_LOCAL_ ||
        (fd->socket.type != SOCK_STREAM_ && fd->socket.type != SOCK_SEQPACKET_)) return events;
    // Synchronize with native shutdown + state publication and peer teardown.
    lock(&peer_lock);
    if (fd->socket.unix_peer_tracked) {
        unsigned state = fd->socket.unix_shutdown;
        // Darwin reports HUP after only local SHUT_WR. Linux reserves it
        // for full shutdown/peer close; preserve unexpected native errors.
        if (state == 1 || state == 2) events &= ~POLL_HUP;
#ifdef __APPLE__
        if (state) events &= ~POLL_PRI; // Darwin EOF is not urgent data.
#endif
        if (state == 3) events |= POLL_HUP;
        if (state & 1) events |= POLL_READ | POLL_RDHUP;
        if (state & 2) events |= POLL_WRITE; // A write will return EPIPE, not wait.
    }
    unlock(&peer_lock);
    return events;
}

const struct fd_ops socket_fdops = {
    .read = sock_read,
    .write = sock_write,
    .close = sock_close,
    .poll = sock_poll,
    .getflags = realfs_getflags,
    .setflags = realfs_setflags,
    .ioctl_size = socket_ioctl_size,
    .ioctl = socket_ioctl,
};

#if defined(__GNUC__) && __GNUC__ >= 8
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
#if defined(__clang__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
static struct socket_call {
    syscall_t func;
    int args;
} socket_calls[] = {
    {NULL},
    {(syscall_t) sys_socket, 3},
    {(syscall_t) sys_bind, 3},
    {(syscall_t) sys_connect, 3},
    {(syscall_t) sys_listen, 2},
    {(syscall_t) sys_accept, 3},
    {(syscall_t) sys_getsockname, 3},
    {(syscall_t) sys_getpeername, 3},
    {(syscall_t) sys_socketpair, 4},
    {(syscall_t) sys_send, 4}, // send
    {(syscall_t) sys_recv, 4}, // recv
    {(syscall_t) sys_sendto, 6},
    {(syscall_t) sys_recvfrom, 6},
    {(syscall_t) sys_shutdown, 2},
    {(syscall_t) sys_setsockopt, 5},
    {(syscall_t) sys_getsockopt, 5},
    {(syscall_t) sys_sendmsg, 3},
    {(syscall_t) sys_recvmsg, 3},
    {NULL}, // accept4
    {NULL}, // recvmmsg
    {(syscall_t) sys_sendmmsg, 4},
};

int_t sys_socketcall(dword_t call_num, addr_t args_addr) {
    STRACE("%d ", call_num);
    if (call_num < 1 || call_num >= sizeof(socket_calls)/sizeof(socket_calls[0]))
        return _EINVAL;
    struct socket_call call = socket_calls[call_num];
    if (call.func == NULL) {
        FIXME("socketcall %d", call_num);
        return _ENOSYS;
    }

    dword_t args[6];
    if (user_read(args_addr, args, sizeof(dword_t) * call.args))
        return _EFAULT;
    return call.func(args[0], args[1], args[2], args[3], args[4], args[5]);
}
