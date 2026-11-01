#ifndef ISH_NETLINK_H
#define ISH_NETLINK_H

#include <stddef.h>
#include <stdint.h>

#define AF_NETLINK_ 16
#define NETLINK_ROUTE_ 0
#define NETLINK_BUFFER_SIZE 4096

struct sockaddr_nl_ {
    uint16_t family, pad;
    uint32_t pid, groups;
};

// Construct one bounded, complete multipart snapshot (including NLMSG_DONE),
// or NLMSG_ERROR. Returns its size, or a negative guest errno. No host changes.
int netlink_snapshot(const void *request, size_t size, uint32_t port,
                     void *response, size_t capacity);

// One reply datagram holds at most NETLINK_BUFFER_SIZE bytes; a snapshot can be
// up to NETLINK_SNAPSHOT_SIZE and is split at message boundaries.
#define NETLINK_SNAPSHOT_SIZE (64 * 1024)

// Host routes in Linux terms (main table only). family is the host AF_INET or
// AF_INET6; addresses are in network byte order.
struct lr_route {
    int family;
    uint8_t dst[16], gateway[16];
    unsigned dst_len, ifindex, mtu;
    _Bool has_gateway;
    uint8_t type, scope, protocol;
};
// Calls callback for each route until it returns nonzero (returned). Returns a
// negative guest errno if the host table cannot be read.
int lr_route_snapshot(int family, int (*callback)(const struct lr_route *, void *), void *context);

#endif
