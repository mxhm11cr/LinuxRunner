// Linux IP_RECVERR / MSG_ERRQUEUE emulation for IPv4 datagram sockets.
// See fs/errqueue.h for what is (and is not) reconstructed.
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include "fs/errqueue.h"
#include "fs/fd.h"
#include "kernel/errno.h"

#define QUEUE_SLOTS 16

struct ip_errqueue {
    pthread_mutex_t lock;
    int icmp_fd;
    unsigned head, count;
    struct ip_queued_error slots[QUEUE_SLOTS];
};

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

// Linux icmp_err_convert[] (net/ipv4/icmp.c) for destination unreachable.
static uint32_t unreach_errno(uint8_t code) {
    switch (code) {
        case 0: case 6: case 9: case 11: return -_ENETUNREACH;
        case 2: return -_ENOPROTOOPT;
        case 3: return -_ECONNREFUSED;
        case 4: return -_EMSGSIZE;
        case 5: return -_EOPNOTSUPP;
        case 7: return -_EHOSTDOWN;
        case 8: return -_ENONET;
        case 13: return -_EACCES; // administratively prohibited (Linux: EHOSTUNREACH unless filtered)
        default: return -_EHOSTUNREACH;
    }
}

static void push(struct ip_errqueue *q, const struct ip_queued_error *e) {
    // Linux bounds the queue by receive buffer; keep the newest errors.
    if (q->count == QUEUE_SLOTS) { q->head = (q->head + 1) % QUEUE_SLOTS; q->count--; }
    q->slots[(q->head + q->count) % QUEUE_SLOTS] = *e;
    q->count++;
}

// Parse one packet from the companion ICMP socket (outer IPv4 header first,
// as Darwin delivers it) and queue it if its quoted UDP datagram is ours.
static void consider(struct ip_errqueue *q, const uint8_t *b, size_t n,
                     const struct sockaddr_in *local, const struct sockaddr_in *peer) {
    if (n < 20 || (b[0] >> 4) != 4) return;
    size_t ihl = (size_t)(b[0] & 0xf) * 4;
    if (ihl < 20 || n < ihl + 8) return;
    const uint8_t *icmp = b + ihl;
    uint8_t type = icmp[0], code = icmp[1];
    if (type != 3 && type != 11 && type != 12) return;
    const uint8_t *inner = icmp + 8;
    size_t inner_n = n - ihl - 8;
    if (inner_n < 20 || (inner[0] >> 4) != 4) return;
    size_t ihl2 = (size_t)(inner[0] & 0xf) * 4;
    if (ihl2 < 20 || inner_n < ihl2 + 4 || inner[9] != IPPROTO_UDP) return;
    const uint8_t *udp = inner + ihl2;
    if (memcmp(udp, &local->sin_port, 2) != 0) return; // source port is ours
    if (local->sin_addr.s_addr != INADDR_ANY && memcmp(inner + 12, &local->sin_addr, 4) != 0) return;
    if (peer && (memcmp(inner + 16, &peer->sin_addr, 4) != 0 || memcmp(udp + 2, &peer->sin_port, 2) != 0)) return;
    struct ip_queued_error e = {.origin = 2, .type = type, .code = code, .has_offender = true, .ttl = b[8]};
    memcpy(&e.offender, b + 12, 4);
    memcpy(&e.dst, inner + 16, 4);
    memcpy(&e.dst_port, udp + 2, 2);
    if (type == 3) {
        e.ee_errno = unreach_errno(code);
        if (code == 4) e.info = get16(icmp + 6); // next-hop MTU (RFC 1191)
    } else if (type == 11) {
        e.ee_errno = -_EHOSTUNREACH;
    } else {
        e.ee_errno = -_EPROTO;
        e.info = icmp[4]; // parameter-problem pointer
    }
    if (inner_n > ihl2 + 8) {
        e.payload_len = inner_n - ihl2 - 8;
        if (e.payload_len > sizeof(e.payload)) e.payload_len = sizeof(e.payload);
        memcpy(e.payload, udp + 8, e.payload_len);
    }
    push(q, &e);
}

static void drain(struct fd *sock, struct ip_errqueue *q) {
    if (q->icmp_fd < 0) return;
    struct sockaddr_in local = {0}, peer = {0};
    socklen_t len = sizeof(local);
    if (getsockname(sock->real_fd, (struct sockaddr *) &local, &len) < 0 || local.sin_family != AF_INET) return;
    len = sizeof(peer);
    bool connected = getpeername(sock->real_fd, (struct sockaddr *) &peer, &len) == 0 && peer.sin_family == AF_INET;
    if (local.sin_port == 0) return; // nothing sent yet; ICMP cannot refer to us
    uint8_t buffer[1600];
    for (;;) {
        ssize_t n = recv(q->icmp_fd, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (n < 0) { if (errno == EINTR) continue; return; }
        consider(q, buffer, (size_t) n, &local, connected ? &peer : NULL);
    }
}

int ip_errqueue_set(struct fd *sock, bool enable) {
    if (!enable) {
        // Linux discards queued errors when IP_RECVERR is turned off.
        ip_errqueue_free(sock);
        return 0;
    }
    if (sock->socket.errqueue) return 0;
    struct ip_errqueue *q = calloc(1, sizeof(*q));
    if (!q) return _ENOMEM;
    q->icmp_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (q->icmp_fd < 0) {
        int err = errno_map();
        free(q);
        return err;
    }
    fcntl(q->icmp_fd, F_SETFD, FD_CLOEXEC);
    fcntl(q->icmp_fd, F_SETFL, O_NONBLOCK);
    pthread_mutex_init(&q->lock, NULL);
    struct ip_errqueue *expected = NULL;
    if (!__atomic_compare_exchange_n(&sock->socket.errqueue, &expected, q, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        close(q->icmp_fd);
        pthread_mutex_destroy(&q->lock);
        free(q);
    }
    return 0;
}

extern const struct fd_ops socket_fdops;
int ip_errqueue_host_fd(struct fd *sock) {
    if (sock->ops != &socket_fdops) return -1;
    struct ip_errqueue *q = __atomic_load_n(&sock->socket.errqueue, __ATOMIC_ACQUIRE);
    return q ? q->icmp_fd : -1;
}

bool ip_errqueue_enabled(struct fd *sock) {
    return __atomic_load_n(&sock->socket.errqueue, __ATOMIC_ACQUIRE) != NULL;
}

void ip_errqueue_free(struct fd *sock) {
    struct ip_errqueue *q = __atomic_exchange_n(&sock->socket.errqueue, NULL, __ATOMIC_ACQ_REL);
    if (!q) return;
    if (q->icmp_fd >= 0) close(q->icmp_fd);
    pthread_mutex_destroy(&q->lock);
    free(q);
}

bool ip_errqueue_pending(struct fd *sock) {
    struct ip_errqueue *q = __atomic_load_n(&sock->socket.errqueue, __ATOMIC_ACQUIRE);
    if (!q) return false;
    pthread_mutex_lock(&q->lock);
    drain(sock, q);
    bool pending = q->count > 0;
    pthread_mutex_unlock(&q->lock);
    return pending;
}

int ip_errqueue_pop(struct fd *sock, struct ip_queued_error *out) {
    struct ip_errqueue *q = __atomic_load_n(&sock->socket.errqueue, __ATOMIC_ACQUIRE);
    if (!q) return _EAGAIN;
    pthread_mutex_lock(&q->lock);
    drain(sock, q);
    int result = _EAGAIN;
    if (q->count) {
        *out = q->slots[q->head];
        q->head = (q->head + 1) % QUEUE_SLOTS;
        q->count--;
        result = 0;
    }
    pthread_mutex_unlock(&q->lock);
    return result;
}

void ip_errqueue_local(struct fd *sock, uint32_t linux_errno, uint32_t info,
                       const struct sockaddr_in *dst, const void *payload, size_t len) {
    struct ip_errqueue *q = __atomic_load_n(&sock->socket.errqueue, __ATOMIC_ACQUIRE);
    if (!q) return;
    struct ip_queued_error e = {.ee_errno = linux_errno, .origin = 1, .info = info};
    if (dst) { e.dst = dst->sin_addr; e.dst_port = dst->sin_port; }
    e.payload_len = len < sizeof(e.payload) ? len : sizeof(e.payload);
    if (payload && e.payload_len) memcpy(e.payload, payload, e.payload_len);
    pthread_mutex_lock(&q->lock);
    push(q, &e);
    pthread_mutex_unlock(&q->lock);
}

uint32_t ip_errqueue_route_mtu(const struct sockaddr_in *dst) {
    // The connected-UDP route lookup sends no packet.
    int probe = socket(AF_INET, SOCK_DGRAM, 0);
    if (probe < 0) return 0;
    struct sockaddr_in target = *dst, source = {0};
    if (target.sin_port == 0) target.sin_port = htons(9);
    socklen_t len = sizeof(source);
    uint32_t mtu = 0;
    if (connect(probe, (struct sockaddr *) &target, sizeof(target)) == 0 &&
            getsockname(probe, (struct sockaddr *) &source, &len) == 0) {
        struct ifaddrs *all;
        if (getifaddrs(&all) == 0) {
            for (struct ifaddrs *ifa = all; ifa; ifa = ifa->ifa_next) {
                if (!ifa->ifa_name || !ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
                if (((struct sockaddr_in *) ifa->ifa_addr)->sin_addr.s_addr != source.sin_addr.s_addr) continue;
                struct ifreq request = {0};
                strncpy(request.ifr_name, ifa->ifa_name, sizeof(request.ifr_name) - 1);
                if (ioctl(probe, SIOCGIFMTU, &request) == 0 && request.ifr_mtu > 0) mtu = (uint32_t) request.ifr_mtu;
                break;
            }
            freeifaddrs(all);
        }
    }
    close(probe);
    return mtu;
}
