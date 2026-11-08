// Read-only Linux route-netlink snapshots backed by host getifaddrs().
// This deliberately does not forward guest route changes or synthesize events.
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#include <net/if_dl.h>
#else
#include <netpacket/packet.h>
#endif
#include <stdbool.h>
#include <stddef.h>
#include "fs/netlink.h"
#include "kernel/errno.h"

struct nl_header {
    uint32_t len;
    uint16_t type, flags;
    uint32_t seq, pid;
};
struct nl_link {
    uint8_t family, pad;
    uint16_t type;
    int32_t index;
    uint32_t flags, change;
};
struct nl_addr {
    uint8_t family, prefix, flags, scope;
    uint32_t index;
};
struct nl_attr { uint16_t len, type; };
#define ALIGN4(n) (((n) + 3u) & ~3u)
#define NL_ERROR 2
#define NL_DONE 3
#define NL_REQUEST 1
#define NL_MULTI 2
#define NL_DUMP 0x300
#define RTM_GETLINK_ 18
#define RTM_GETADDR_ 22
#define RTM_GETROUTE_ 26
#define RTM_NEWROUTE_ 24

struct builder {
    char *data;
    size_t used, capacity, start;
    uint32_t seq, port;
    int error;
    bool single; // reply to a non-dump request: no NLM_F_MULTI, no DONE
};
static void append(struct builder *b, const void *data, size_t size) {
    if (b->error) return;
    size_t aligned = ALIGN4(size);
    if (aligned > b->capacity - b->used) {
        b->error = _ENOBUFS;
        return;
    }
    memcpy(b->data + b->used, data, size);
    memset(b->data + b->used + size, 0, aligned - size);
    b->used += aligned;
}
static void begin(struct builder *b, uint16_t type) {
    b->start = b->used;
    struct nl_header h = {sizeof(h), type, b->single ? 0 : NL_MULTI, b->seq, b->port};
    append(b, &h, sizeof(h));
}
static void end(struct builder *b) {
    if (!b->error) {
        uint32_t size = b->used - b->start;
        memcpy(b->data + b->start, &size, sizeof(size));
    }
}
static void attr(struct builder *b, uint16_t type, const void *data, size_t size) {
    struct nl_attr a = {sizeof(a) + size, type};
    append(b, &a, sizeof(a));
    append(b, data, size);
}
static uint32_t linux_flags(unsigned flags) {
    uint32_t out = 0;
    if (flags & IFF_UP) out |= 1;
    if (flags & IFF_BROADCAST) out |= 2;
    if (flags & IFF_LOOPBACK) out |= 8;
    if (flags & IFF_POINTOPOINT) out |= 16;
    if (flags & IFF_RUNNING) out |= 64;
    if (flags & IFF_MULTICAST) out |= 4096;
    return out;
}
static void link_message(struct builder *b, struct ifaddrs *ifa, struct ifaddrs *all, int ctl) {
    unsigned index = if_nametoindex(ifa->ifa_name);
    if (!index) return; // Interface disappeared during the snapshot.
    struct nl_link link = {
        .type = (ifa->ifa_flags & IFF_LOOPBACK) ? 772 : 1,
        .index = index, .flags = linux_flags(ifa->ifa_flags),
    };
    if (ifa->ifa_flags & IFF_POINTOPOINT) link.type = 65534; // ARPHRD_NONE
    const void *hardware = NULL;
    size_t hardware_size = 0;
    for (struct ifaddrs *p = all; p; p = p->ifa_next) {
        if (!p->ifa_addr || !p->ifa_name || strcmp(p->ifa_name, ifa->ifa_name)) continue;
#ifdef __APPLE__
        if (p->ifa_addr->sa_family == AF_LINK) {
            const struct sockaddr_dl *dl = (const void *) p->ifa_addr;
            hardware = LLADDR(dl);
            hardware_size = dl->sdl_alen;
            if ((const char *) hardware + hardware_size > (const char *) dl + dl->sdl_len)
                hardware_size = 0;
        }
#else
        if (p->ifa_addr->sa_family == AF_PACKET) {
            const struct sockaddr_ll *ll = (const void *) p->ifa_addr;
            link.type = ll->sll_hatype;
            hardware = ll->sll_addr;
            if (ll->sll_halen <= sizeof(ll->sll_addr)) hardware_size = ll->sll_halen;
        }
#endif
    }
    begin(b, RTM_GETLINK_ - 2);
    append(b, &link, sizeof(link));
    attr(b, 3, ifa->ifa_name, strlen(ifa->ifa_name) + 1); // IFLA_IFNAME
    if (hardware_size) attr(b, 1, hardware, hardware_size); // IFLA_ADDRESS
    struct ifreq req = {0};
    strncpy(req.ifr_name, ifa->ifa_name, sizeof(req.ifr_name) - 1);
    if (ctl >= 0 && ioctl(ctl, SIOCGIFMTU, &req) == 0) {
        uint32_t mtu = req.ifr_mtu;
        attr(b, 4, &mtu, sizeof(mtu));
    }
    // IFLA_OPERSTATE from the host flags: loopback UNKNOWN (as Linux reports),
    // running UP, otherwise DOWN.
    uint8_t operstate = (ifa->ifa_flags & IFF_LOOPBACK) ? 0 : (ifa->ifa_flags & IFF_RUNNING) ? 6 : 2;
    attr(b, 16, &operstate, sizeof(operstate));
    // IFLA_TXQLEN 0: the guest has no Linux queueing discipline over host
    // interfaces (Darwin queues internally); 0 is "no queue length".
    uint32_t txqlen = 0;
    attr(b, 13, &txqlen, sizeof(txqlen));
    end(b);
}
static const void *ip_bytes(const struct sockaddr *sa) {
    if (!sa) return NULL;
    if (sa->sa_family == AF_INET) return &((const struct sockaddr_in *) sa)->sin_addr;
    if (sa->sa_family == AF_INET6) return &((const struct sockaddr_in6 *) sa)->sin6_addr;
    return NULL;
}
static unsigned prefix_length(const struct sockaddr *mask, size_t size) {
    const uint8_t *bytes = ip_bytes(mask);
    if (!bytes) return 0;
    unsigned prefix = 0;
    for (size_t i = 0; i < size; i++) {
        for (unsigned bit = 128; bit; bit >>= 1) {
            if (!(bytes[i] & bit)) return prefix;
            prefix++;
        }
    }
    return prefix;
}
static void addr_message(struct builder *b, struct ifaddrs *ifa, uint8_t family) {
    if (!ifa->ifa_addr) return;
    int af = ifa->ifa_addr->sa_family;
    if (af != AF_INET && af != AF_INET6) return;
    uint8_t guest_af = af == AF_INET ? 2 : 10;
    if (family && family != guest_af) return;
    unsigned index = if_nametoindex(ifa->ifa_name);
    if (!index) return;
    size_t size = af == AF_INET ? 4 : 16;
    union { struct in6_addr aligned; uint8_t bytes[16]; } storage;
    uint8_t *local = storage.bytes;
    memcpy(local, ip_bytes(ifa->ifa_addr), size);
#ifdef __APPLE__
    // Darwin embeds the interface scope in link-local IPv6 bytes 2..3.
    if (af == AF_INET6 && local[0] == 0xfe && (local[1] & 0xc0) == 0x80)
        local[2] = local[3] = 0;
#endif
    struct nl_addr addr = {
        .family = guest_af, .prefix = prefix_length(ifa->ifa_netmask, size),
        .flags = 0x80, .index = index, // IFA_F_PERMANENT
    };
    if ((af == AF_INET && local[0] == 127) ||
            (af == AF_INET6 && IN6_IS_ADDR_LOOPBACK((const struct in6_addr *) local)))
        addr.scope = 254; // RT_SCOPE_HOST
    else if ((af == AF_INET && local[0] == 169 && local[1] == 254) ||
            (af == AF_INET6 && local[0] == 0xfe && (local[1] & 0xc0) == 0x80))
        addr.scope = 253; // RT_SCOPE_LINK
    begin(b, RTM_GETADDR_ - 2);
    append(b, &addr, sizeof(addr));
    const void *peer = NULL;
    if (af == AF_INET && (ifa->ifa_flags & IFF_POINTOPOINT) && ifa->ifa_dstaddr &&
            ifa->ifa_dstaddr->sa_family == af)
        peer = ip_bytes(ifa->ifa_dstaddr);
    attr(b, 1, peer ? peer : local, size); // IFA_ADDRESS
    if (af == AF_INET) attr(b, 2, local, size); // IFA_LOCAL
    attr(b, 3, ifa->ifa_name, strlen(ifa->ifa_name) + 1); // IFA_LABEL
    end(b);
}

// Host routing table (Darwin sysctl NET_RT_DUMP, a public interface; iOS omits
// <net/route.h>, so the stable routing-message layout is declared here). Only
// the primary table is reported: ARP/ND cache entries, cloned host routes,
// local/broadcast/multicast entries and interface-scoped duplicate tables are
// not part of Linux's main table. Read-only; guests cannot change host routes.
#ifdef __APPLE__
struct lr_rt_metrics { uint32_t locks, mtu, hopcount; int32_t expire; uint32_t recvpipe, sendpipe, ssthresh, rtt, rttvar, pksent, filler[4]; };
struct lr_rt_msghdr {
    unsigned short msglen; unsigned char version, type; unsigned short index;
    int flags, addrs; pid_t pid; int seq, err, use; uint32_t inits;
    struct lr_rt_metrics rmx;
};
#define LR_RTF_UP 0x1
#define LR_RTF_GATEWAY 0x2
#define LR_RTF_HOST 0x4
#define LR_RTF_REJECT 0x8
#define LR_RTF_CLONING 0x100
#define LR_RTF_LLINFO 0x400
#define LR_RTF_STATIC 0x800
#define LR_RTF_BLACKHOLE 0x1000
#define LR_RTF_WASCLONED 0x20000
#define LR_RTF_LOCAL 0x200000
#define LR_RTF_BROADCAST 0x400000
#define LR_RTF_MULTICAST 0x800000
#define LR_RTF_IFSCOPE 0x1000000
#define LR_SA_ROUNDUP(n) ((n) > 0 ? (1 + (((n) - 1) | (sizeof(uint32_t) - 1))) : sizeof(uint32_t))
static unsigned mask_bits(const uint8_t *bytes, size_t size) {
    unsigned prefix = 0;
    for (size_t i = 0; i < size; i++)
        for (unsigned bit = 128; bit; bit >>= 1) { if (!(bytes[i] & bit)) return prefix; prefix++; }
    return prefix;
}
// Copy an address of the given family from a (possibly truncated) sockaddr.
static bool route_addr(const struct sockaddr *sa, int family, uint8_t out[16]) {
    memset(out, 0, 16);
    if (!sa) return false;
    size_t offset = family == AF_INET ? offsetof(struct sockaddr_in, sin_addr) : offsetof(struct sockaddr_in6, sin6_addr);
    size_t size = family == AF_INET ? 4 : 16;
    if (sa->sa_len > offset) memcpy(out, (const char *) sa + offset, sa->sa_len - offset < size ? sa->sa_len - offset : size);
    if (family == AF_INET6 && out[0] == 0xfe && (out[1] & 0xc0) == 0x80) out[2] = out[3] = 0; // KAME scope
    return true;
}
static int route_parse(const char *table, size_t size, int family,
                       int (*callback)(const struct lr_route *, void *), void *context) {
    int result = 0;
    for (size_t at = 0; at < size && !result;) {
        if (size - at < sizeof(struct lr_rt_msghdr)) return _EIO;
        // A corrupt or future host message must not trigger unaligned loads or
        // turn a partial table into a successful empty routing snapshot.
        struct lr_rt_msghdr header;
        memcpy(&header, table + at, sizeof(header));
        const struct lr_rt_msghdr *m = &header;
        if (m->msglen < sizeof(*m) || m->msglen > size - at || m->version != 5)
            return _EIO;
        const char *cursor = table + at + sizeof(*m), *limit = table + at + m->msglen;
        at += m->msglen;
        int f = m->flags;
        if (!(f & LR_RTF_UP) || (f & (LR_RTF_LLINFO | LR_RTF_WASCLONED | LR_RTF_LOCAL | LR_RTF_BROADCAST |
                LR_RTF_MULTICAST | LR_RTF_IFSCOPE))) continue;
        const struct sockaddr *addrs[8] = {0};
        for (int i = 0; i < 8; i++) {
            if (!(m->addrs & (1 << i))) continue;
            const struct sockaddr *sa = (const void *) cursor;
            if (limit - cursor < 2 || LR_SA_ROUNDUP(sa->sa_len) > (size_t)(limit - cursor))
                return _EIO;
            addrs[i] = sa;
            cursor += LR_SA_ROUNDUP(sa->sa_len);
        }
        const struct sockaddr *dst = addrs[0], *gateway = addrs[1], *mask = addrs[2];
        if (!dst || dst->sa_family != family) continue;
        size_t address_end = family == AF_INET ? offsetof(struct sockaddr_in, sin_addr) + 4
                                               : offsetof(struct sockaddr_in6, sin6_addr) + 16;
        if (dst->sa_len < address_end) return _EIO;
        struct lr_route r = {.family = family, .ifindex = m->index};
        route_addr(dst, family, r.dst);
        size_t bytes = family == AF_INET ? 4 : 16;
        if (f & LR_RTF_HOST) r.dst_len = bytes * 8;
        else if (mask) { uint8_t m8[16]; route_addr(mask, family, m8); r.dst_len = mask_bits(m8, bytes); }
        else r.dst_len = 0;
        r.has_gateway = (f & LR_RTF_GATEWAY) && gateway && gateway->sa_family == family;
        // Darwin's per-address /32 (/128) cloning stubs (netstat "UCS" link#N for the
        // gateway, own and broadcast addresses) have no Linux main-table equivalent.
        if (!r.has_gateway && (f & LR_RTF_CLONING) && r.dst_len == bytes * 8) continue;
        if (r.has_gateway) {
            if (gateway->sa_len < address_end) return _EIO;
            route_addr(gateway, family, r.gateway);
        }
        r.type = (f & LR_RTF_BLACKHOLE) ? 6 : (f & LR_RTF_REJECT) ? 7 : 1; // RTN_*
        r.scope = r.has_gateway ? 0 : 253; // RT_SCOPE_UNIVERSE / LINK
        // Darwin marks DHCP and interface routes static as well, so origin is not
        // recoverable: interface routes are reported as kernel, others as boot.
        r.protocol = r.has_gateway ? 3 : 2; // RTPROT_BOOT / RTPROT_KERNEL
        r.mtu = m->rmx.mtu;
        result = callback(&r, context);
    }
    return result;
}
int lr_route_snapshot(int family, int (*callback)(const struct lr_route *, void *), void *context) {
    if (family != AF_INET && family != AF_INET6) return _EAFNOSUPPORT;
    int mib[6] = {CTL_NET, PF_ROUTE, 0, family, NET_RT_DUMP, 0};
    // A Wi-Fi/VPN transition can grow the table between size and data reads.
    // Retry that race, bound allocation, and propagate genuine permission errors.
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        size_t size = 0;
        if (sysctl(mib, 6, NULL, &size, NULL, 0) < 0) return errno_map();
        if (size > 8 * 1024 * 1024) return _ENOBUFS;
        size += size / 4 + 4096;
        char *table = malloc(size);
        if (!table) return _ENOMEM;
        if (sysctl(mib, 6, table, &size, NULL, 0) < 0) {
            int error = errno_map(), native = errno;
            free(table);
            if (native == ENOMEM && attempt < 2) continue;
            return error;
        }
        int result = route_parse(table, size, family, callback, context);
        free(table);
        return result;
    }
    return _EAGAIN;
}
#else
int lr_route_snapshot(int family, int (*callback)(const struct lr_route *, void *), void *context) {
    (void) family; (void) callback; (void) context;
    return _EOPNOTSUPP;
}
#endif

static int route_message(const struct lr_route *r, void *context) {
    struct builder *b = context;
    struct { uint8_t family, dst_len, src_len, tos, table, protocol, scope, type; uint32_t flags; } rt = {
        .family = r->family == AF_INET ? 2 : 10, .dst_len = r->dst_len, .table = 254, // RT_TABLE_MAIN
        .protocol = r->protocol, .scope = r->scope, .type = r->type,
    };
    size_t size = r->family == AF_INET ? 4 : 16;
    begin(b, RTM_NEWROUTE_);
    append(b, &rt, sizeof(rt));
    uint32_t table = 254;
    attr(b, 15, &table, sizeof(table)); // RTA_TABLE
    if (r->dst_len) attr(b, 1, r->dst, size); // RTA_DST
    if (r->has_gateway) attr(b, 5, r->gateway, size); // RTA_GATEWAY
    if (r->ifindex) { uint32_t oif = r->ifindex; attr(b, 4, &oif, sizeof(oif)); } // RTA_OIF
    end(b);
    return b->error;
}

static int error_message(void *out, size_t capacity, struct nl_header req,
                         uint32_t port, int error) {
    // NLM_F_CAPPED: original request header included, payload omitted.
    struct { struct nl_header h; int32_t error; struct nl_header request; } reply = {
        .h = {sizeof(reply), NL_ERROR, 0x100, req.seq, port}, .error = error, .request = req,
    };
    if (capacity < sizeof(reply)) return _ENOBUFS;
    memcpy(out, &reply, sizeof(reply));
    return sizeof(reply);
}

// Non-dump RTM_GETLINK (e.g. "ip addr show dev en0"): one interface selected by
// ifi_index or IFLA_IFNAME; ENODEV when absent; NLM_F_ACK adds the ACK.
static int link_get(const void *request, struct nl_header req, uint32_t port, void *response, size_t capacity) {
    const uint8_t *body = (const uint8_t *) request + sizeof(req);
    size_t body_size = req.len - sizeof(req);
    int32_t index = 0;
    char name[IF_NAMESIZE] = "";
    if (body_size >= sizeof(struct nl_link)) {
        memcpy(&index, body + offsetof(struct nl_link, index), sizeof(index));
        for (size_t at = sizeof(struct nl_link); at + sizeof(struct nl_attr) <= body_size;) {
            struct nl_attr a;
            memcpy(&a, body + at, sizeof(a));
            if (a.len < sizeof(a) || a.len > body_size - at)
                return error_message(response, capacity, req, port, _EINVAL);
            if ((a.type & 0x3fff) == 3) { // IFLA_IFNAME (NLA type flags excluded)
                size_t len = a.len - sizeof(a);
                const char *value = (const void *)(body + at + sizeof(a));
                if (!len || len > sizeof(name) || !memchr(value, 0, len))
                    return error_message(response, capacity, req, port, _EINVAL);
                memcpy(name, value, len);
            }
            at += ALIGN4(a.len);
        }
    }
    if (!index && !name[0]) return error_message(response, capacity, req, port, _EINVAL);
    struct builder b = {response, 0, capacity, 0, req.seq, port, 0, true};
    struct ifaddrs *all;
    if (getifaddrs(&all) < 0) return error_message(response, capacity, req, port, errno_map());
    int ctl = socket(AF_INET, SOCK_DGRAM, 0);
    bool found = false;
    for (struct ifaddrs *ifa = all; ifa && !found; ifa = ifa->ifa_next) {
        if (!ifa->ifa_name) continue;
        if (name[0] ? strcmp(name, ifa->ifa_name) != 0 : (int32_t) if_nametoindex(ifa->ifa_name) != index) continue;
        link_message(&b, ifa, all, ctl);
        found = true;
    }
    if (ctl >= 0) close(ctl);
    freeifaddrs(all);
    if (!found) return error_message(response, capacity, req, port, _ENODEV);
    if (b.error) return error_message(response, capacity, req, port, b.error);
    if (req.flags & 4) { // NLM_F_ACK
        int ack = error_message(b.data + b.used, capacity - b.used, req, port, 0);
        if (ack < 0) return error_message(response, capacity, req, port, ack);
        b.used += ack;
    }
    return b.used;
}

#define RTM_NEWNEIGH_ 28
#define RTM_GETNEIGH_ 30

static bool prefix_matches(const uint8_t *a, const uint8_t *b, unsigned bits) {
    unsigned whole = bits / 8, rest = bits % 8;
    if (memcmp(a, b, whole)) return false;
    if (!rest) return true;
    uint8_t mask = (uint8_t)(0xff << (8 - rest));
    return (a[whole] & mask) == (b[whole] & mask);
}
struct route_lookup { uint8_t dst[16]; unsigned ifindex; struct lr_route best; bool found; };
static int route_lookup_cb(const struct lr_route *r, void *context) {
    struct route_lookup *l = context;
    if (!prefix_matches(r->dst, l->dst, r->dst_len)) return 0;
    // Prefer routes on the interface the host stack actually selected.
    if (l->ifindex && r->ifindex && r->ifindex != l->ifindex) return 0;
    if (!l->found || r->dst_len > l->best.dst_len) { l->best = *r; l->found = true; }
    return 0;
}

// Non-dump RTM_GETROUTE ("ip route get ADDR"). The host chooses the route: a
// connected UDP socket performs the kernel route lookup without sending any
// packet, getsockname() gives the preferred source, its interface is the
// output device, and the gateway comes from the longest matching host route.
static int route_get(const void *request, struct nl_header req, uint32_t port, void *response, size_t capacity) {
    const uint8_t *body = (const uint8_t *) request + sizeof(req);
    size_t body_size = req.len - sizeof(req);
    if (body_size < 12) return error_message(response, capacity, req, port, _EINVAL);
    uint8_t family = body[0];
    if (family != 2 && family != 10) return error_message(response, capacity, req, port, _EOPNOTSUPP);
    int host = family == 2 ? AF_INET : AF_INET6;
    size_t size = family == 2 ? 4 : 16;
    struct route_lookup lookup = {0};
    bool have_dst = false;
    for (size_t at = 12; at + sizeof(struct nl_attr) <= body_size;) {
        struct nl_attr a;
        memcpy(&a, body + at, sizeof(a));
        if (a.len < sizeof(a) || a.len > body_size - at)
            return error_message(response, capacity, req, port, _EINVAL);
        if ((a.type & 0x3fff) == 1) { // RTA_DST
            if (a.len - sizeof(a) != size) return error_message(response, capacity, req, port, _EINVAL);
            memcpy(lookup.dst, body + at + sizeof(a), size);
            have_dst = true;
        }
        at += ALIGN4(a.len);
    }
    struct sockaddr_storage target = {0}, source = {0};
    socklen_t target_len, source_len = sizeof(source);
    if (host == AF_INET) {
        struct sockaddr_in *in = (void *) &target;
        in->sin_family = AF_INET; in->sin_port = htons(9);
        memcpy(&in->sin_addr, lookup.dst, 4);
        target_len = sizeof(*in);
    } else {
        struct sockaddr_in6 *in6 = (void *) &target;
        in6->sin6_family = AF_INET6; in6->sin6_port = htons(9);
        memcpy(&in6->sin6_addr, lookup.dst, 16);
        target_len = sizeof(*in6);
    }
#ifdef __APPLE__
    target.ss_len = (uint8_t) target_len;
#endif
    (void) have_dst; // Linux treats an absent RTA_DST as the unspecified address.
    int probe = socket(host, SOCK_DGRAM, 0);
    if (probe < 0) return error_message(response, capacity, req, port, errno_map());
    int error = 0;
    if (connect(probe, (struct sockaddr *) &target, target_len) < 0 ||
            getsockname(probe, (struct sockaddr *) &source, &source_len) < 0)
        error = errno_map();
    close(probe);
    if (error) return error_message(response, capacity, req, port, error);
    const uint8_t *src = host == AF_INET ? (const uint8_t *) &((struct sockaddr_in *) &source)->sin_addr
                                         : (const uint8_t *) &((struct sockaddr_in6 *) &source)->sin6_addr;
    bool local = false;
    struct ifaddrs *all;
    if (getifaddrs(&all) < 0) return error_message(response, capacity, req, port, errno_map());
    for (struct ifaddrs *ifa = all; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_name || !ifa->ifa_addr || ifa->ifa_addr->sa_family != host) continue;
        const uint8_t *mine = ip_bytes(ifa->ifa_addr);
        if (!lookup.ifindex && !memcmp(mine, src, size)) lookup.ifindex = if_nametoindex(ifa->ifa_name);
        if (!memcmp(mine, lookup.dst, size)) local = true;
    }
    freeifaddrs(all);
    int snapshot = lr_route_snapshot(host, route_lookup_cb, &lookup);
    if (snapshot < 0 && snapshot != _EOPNOTSUPP)
        return error_message(response, capacity, req, port, snapshot);
    struct builder b = {response, 0, capacity, 0, req.seq, port, 0, true};
    struct { uint8_t family, dst_len, src_len, tos, table, protocol, scope, type; uint32_t flags; } rt = {
        .family = family, .dst_len = (uint8_t)(size * 8), .table = 254,
        .scope = local ? 254 : 0, .type = local ? 2 : 1, .flags = 0x200, // RTN_LOCAL/UNICAST, RTM_F_CLONED
    };
    begin(&b, RTM_NEWROUTE_);
    append(&b, &rt, sizeof(rt));
    uint32_t table = local ? 255 : 254;
    attr(&b, 15, &table, sizeof(table)); // RTA_TABLE
    attr(&b, 1, lookup.dst, size); // RTA_DST
    if (lookup.ifindex) { uint32_t oif = lookup.ifindex; attr(&b, 4, &oif, sizeof(oif)); } // RTA_OIF
    attr(&b, 7, src, size); // RTA_PREFSRC
    if (!local && lookup.found && lookup.best.has_gateway) attr(&b, 5, lookup.best.gateway, size); // RTA_GATEWAY
    end(&b);
    if (b.error) return error_message(response, capacity, req, port, b.error);
    if (req.flags & 4) { // NLM_F_ACK
        int ack = error_message(b.data + b.used, capacity - b.used, req, port, 0);
        if (ack < 0) return error_message(response, capacity, req, port, ack);
        b.used += ack;
    }
    return b.used;
}

// RTM_GETNEIGH dump: the host IPv4 ARP cache through the public sysctl
// NET_RT_FLAGS/RTF_LLINFO. Current Darwin returns no entries to unprivileged
// (sandboxed) processes even when the cache is populated; only private
// interfaces expose it. An empty public answer is therefore reported as
// EOPNOTSUPP rather than as a genuinely empty neighbour table. Where entries
// are returned, Darwin has no NUD confirmation: dynamic entries are STALE,
// static ones PERMANENT, unresolved ones INCOMPLETE. No IPv6 ND table.
#ifdef __APPLE__
static int neigh_dump(struct builder *b) {
    int mib[6] = {CTL_NET, PF_ROUTE, 0, AF_INET, 2 /* NET_RT_FLAGS */, LR_RTF_LLINFO};
    size_t size = 0;
    if (sysctl(mib, 6, NULL, &size, NULL, 0) < 0) return errno_map();
    if (size > 4 * 1024 * 1024) return _ENOBUFS;
    size += size / 4 + 1024;
    char *table = malloc(size ? size : 1);
    if (!table) return _ENOMEM;
    if (sysctl(mib, 6, table, &size, NULL, 0) < 0) { int e = errno_map(); free(table); return e; }
    int result = 0;
    unsigned entries = 0;
    for (size_t at = 0; at < size && !result && !b->error;) {
        if (size - at < sizeof(struct lr_rt_msghdr)) { result = _EIO; break; }
        struct lr_rt_msghdr m;
        memcpy(&m, table + at, sizeof(m));
        if (m.msglen < sizeof(m) || m.msglen > size - at || m.version != 5) { result = _EIO; break; }
        const char *cursor = table + at + sizeof(m), *limit = table + at + m.msglen;
        at += m.msglen;
        const struct sockaddr *addrs[8] = {0};
        bool bad = false;
        for (int i = 0; i < 8; i++) {
            if (!(m.addrs & (1 << i))) continue;
            const struct sockaddr *sa = (const void *) cursor;
            if (limit - cursor < 2 || LR_SA_ROUNDUP(sa->sa_len) > (size_t)(limit - cursor)) { bad = true; break; }
            addrs[i] = sa;
            cursor += LR_SA_ROUNDUP(sa->sa_len);
        }
        if (bad) { result = _EIO; break; }
        const struct sockaddr *dst = addrs[0], *gw = addrs[1];
        if (!dst || dst->sa_family != AF_INET || dst->sa_len < sizeof(struct sockaddr_in)) continue;
        if (!gw || gw->sa_family != AF_LINK) continue;
        if (m.flags & (LR_RTF_BROADCAST | LR_RTF_MULTICAST)) continue;
        struct sockaddr_dl dl;
        memset(&dl, 0, sizeof(dl));
        memcpy(&dl, gw, gw->sa_len < sizeof(dl) ? gw->sa_len : sizeof(dl));
        size_t lladdr_offset = offsetof(struct sockaddr_dl, sdl_data) + dl.sdl_nlen;
        bool resolved = dl.sdl_alen > 0 && lladdr_offset + dl.sdl_alen <= gw->sa_len;
        struct { uint8_t family, pad1; uint16_t pad2; int32_t ifindex; uint16_t state; uint8_t flags, type; } nd = {
            .family = 2, .ifindex = dl.sdl_index ? dl.sdl_index : m.index, .type = 1,
            .state = !resolved ? 0x01 : (m.rmx.expire == 0 || (m.flags & LR_RTF_STATIC)) ? 0x80 : 0x04,
        };
        begin(b, RTM_NEWNEIGH_);
        append(b, &nd, sizeof(nd));
        attr(b, 1, &((const struct sockaddr_in *) dst)->sin_addr, 4); // NDA_DST
        if (resolved) attr(b, 2, (const char *) gw + lladdr_offset, dl.sdl_alen); // NDA_LLADDR
        end(b);
        entries++;
    }
    free(table);
    if (!result && !b->error && entries == 0) return _EOPNOTSUPP;
    return result ? result : b->error;
}
#else
static int neigh_dump(struct builder *b) { (void) b; return _EOPNOTSUPP; }
#endif

static int netlink_snapshot_one(const void *request, size_t size, uint32_t port,
                     void *response, size_t capacity) {
    struct nl_header req;
    if (size < sizeof(req)) return _EINVAL;
    memcpy(&req, request, sizeof(req));
    int error = _EINVAL;
    if (req.len < sizeof(req) || req.len > size || !(req.flags & NL_REQUEST)) goto fail;
    error = _EOPNOTSUPP;
    if (req.type == RTM_GETLINK_ && (req.flags & NL_DUMP) != NL_DUMP)
        return link_get(request, req, port, response, capacity);
    if (req.type == RTM_GETROUTE_ && (req.flags & NL_DUMP) != NL_DUMP)
        return route_get(request, req, port, response, capacity);
    if ((req.flags & NL_DUMP) != NL_DUMP ||
            (req.type != RTM_GETLINK_ && req.type != RTM_GETADDR_ && req.type != RTM_GETROUTE_ &&
             req.type != RTM_GETNEIGH_))
        goto fail;
    uint8_t family = req.len > sizeof(req) ? ((const uint8_t *) request)[sizeof(req)] : 0;
    // Link dumps are family-independent (iproute2 asks with AF_PACKET).
    if (req.type == RTM_GETLINK_ || family == 17) family = 0;
    error = _EAFNOSUPPORT;
    if (family != 0 && family != 2 && family != 10) goto fail;
    struct builder b = {response, 0, capacity, 0, req.seq, port, 0, false};
    if (req.type == RTM_GETNEIGH_) {
        // IPv4 only; an IPv6-only request gets an explicit error, not an empty table.
        if (family == 10) { error = _EOPNOTSUPP; goto fail; }
        int err = neigh_dump(&b);
        if (err < 0) { error = err; goto fail; }
    } else if (req.type == RTM_GETROUTE_) {
        for (int i = 0; i < 2 && !b.error; i++) {
            int host = i == 0 ? AF_INET : AF_INET6;
            if (family && family != (host == AF_INET ? 2 : 10)) continue;
            int err = lr_route_snapshot(host, route_message, &b);
            if (err < 0 && !b.error) { error = err; goto fail; }
        }
    } else {
        struct ifaddrs *all;
        if (getifaddrs(&all) < 0) { error = errno_map(); goto fail; }
        int ctl = req.type == RTM_GETLINK_ ? socket(AF_INET, SOCK_DGRAM, 0) : -1;
        for (struct ifaddrs *ifa = all; ifa && !b.error; ifa = ifa->ifa_next) {
            if (!ifa->ifa_name) continue;
            if (req.type == RTM_GETADDR_) {
                addr_message(&b, ifa, family);
            } else {
                struct ifaddrs *prev;
                for (prev = all; prev != ifa; prev = prev->ifa_next)
                    if (prev->ifa_name && !strcmp(prev->ifa_name, ifa->ifa_name)) break;
                if (prev == ifa) link_message(&b, ifa, all, ctl);
            }
        }
        if (ctl >= 0) close(ctl);
        freeifaddrs(all);
    }
    begin(&b, NL_DONE);
    int32_t status = 0;
    append(&b, &status, sizeof(status));
    end(&b);
    if (b.error) { error = b.error; goto fail; }
    return b.used;
fail:
    return error_message(response, capacity, req, port, error);
}

int netlink_snapshot(const void *request, size_t size, uint32_t port,
                     void *response, size_t capacity) {
    if (size < sizeof(struct nl_header)) return _EINVAL;
    size_t at = 0, used = 0;
    while (size - at >= sizeof(struct nl_header)) {
        struct nl_header req;
        memcpy(&req, (const char *)request + at, sizeof(req));
        // Linux ends a batch at trailing padding/an incomplete next header.
        // An invalid first request still receives the existing explicit error.
        if (used && (req.len < sizeof(req) || req.len > size - at)) break;
        int n = netlink_snapshot_one((const char *)request + at, size - at, port,
                                     (char *)response + used, capacity - used);
        if (n < 0) return n;
        used += n;
        if (req.len < sizeof(req) || req.len > size - at) break;
        size_t step = ALIGN4((size_t)req.len);
        if (step > size - at) break;
        at += step;
    }
    return used;
}
