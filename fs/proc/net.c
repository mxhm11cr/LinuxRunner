#include <ifaddrs.h>
#include <inttypes.h>
#include <net/if.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#ifdef __APPLE__
#include <net/if_dl.h>
#else
#include <linux/if_link.h>
#include <netpacket/packet.h>
#endif
#include "fs/proc.h"
#include "fs/netlink.h"
#include "kernel/errno.h"

// A fresh read-only host snapshot, like the route-netlink and ioctl views.
// Counters cover the host interface, not traffic attributable to this guest.
// Darwin does not expose every Linux counter; those legacy fields remain zero.
static int proc_show_net_dev(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct ifaddrs *all;
    if (getifaddrs(&all) < 0) return errno_map();
    proc_printf(buf, "Inter-|   Receive                                                |  Transmit\n");
    proc_printf(buf, " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n");
    for (struct ifaddrs *p = all; p; p = p->ifa_next) {
        if (!p->ifa_name || !p->ifa_addr || !p->ifa_data) continue;
#ifdef __APPLE__
        if (p->ifa_addr->sa_family != AF_LINK) continue;
        const struct if_data *d = p->ifa_data;
        uint64_t rx[] = {d->ifi_ibytes, d->ifi_ipackets, d->ifi_ierrors,
                        d->ifi_iqdrops, 0, 0, 0, d->ifi_imcasts};
        uint64_t tx[] = {d->ifi_obytes, d->ifi_opackets, d->ifi_oerrors,
                        0, 0, d->ifi_collisions, 0, 0};
#else
        if (p->ifa_addr->sa_family != AF_PACKET) continue;
        const struct rtnl_link_stats *d = p->ifa_data;
        uint64_t rx[] = {d->rx_bytes, d->rx_packets, d->rx_errors, d->rx_dropped,
                        d->rx_fifo_errors, d->rx_frame_errors, d->rx_compressed, d->multicast};
        uint64_t tx[] = {d->tx_bytes, d->tx_packets, d->tx_errors, d->tx_dropped,
                        d->tx_fifo_errors, d->collisions, d->tx_carrier_errors, d->tx_compressed};
#endif
        // IP aliases are excluded. No invented eth0 or loopback name is added.
        proc_printf(buf, "%6s:", p->ifa_name);
        for (unsigned i = 0; i < 8; i++) proc_printf(buf, " %" PRIu64, rx[i]);
        for (unsigned i = 0; i < 8; i++) proc_printf(buf, " %" PRIu64, tx[i]);
        proc_printf(buf, "\n");
    }
    freeifaddrs(all);
    return 0;
}

// Linux /proc/net/route (IPv4 main table) from the host routing snapshot.
// Addresses are the raw network-order words, printed as Linux does.
static int route_line(const struct lr_route *r, void *context) {
    struct proc_data *buf = context;
    char name[IF_NAMESIZE] = "*";
    if (r->ifindex && !if_indextoname(r->ifindex, name)) return 0;
    uint32_t dst, gateway, mask = r->dst_len ? htonl(~0u << (32 - r->dst_len)) : 0;
    memcpy(&dst, r->dst, 4); memcpy(&gateway, r->gateway, 4);
    unsigned flags = 0x1 | (r->has_gateway ? 0x2 : 0) | (r->dst_len == 32 ? 0x4 : 0) | (r->type == 7 ? 0x200 : 0);
    char line[160];
    int n = snprintf(line, sizeof(line), "%s\t%08X\t%08X\t%04X\t%d\t%u\t%d\t%08X\t%d\t%u\t%u",
            name, dst, gateway, flags, 0, 0, 0, mask, 0, 0, 0);
    proc_printf(buf, "%-127s\n", n > 0 ? line : "");
    return 0;
}
static int proc_show_net_route(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "%-127s\n", "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT");
    return lr_route_snapshot(AF_INET, route_line, buf);
}

// Linux /proc/net/if_inet6: one line per IPv6 address of a host interface.
static int proc_show_net_if_inet6(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct ifaddrs *all;
    if (getifaddrs(&all) < 0) return errno_map();
    for (struct ifaddrs *p = all; p; p = p->ifa_next) {
        if (!p->ifa_name || !p->ifa_addr || p->ifa_addr->sa_family != AF_INET6) continue;
        unsigned index = if_nametoindex(p->ifa_name);
        if (!index) continue;
        uint8_t a[16], m[16] = {0};
        memcpy(a, &((const struct sockaddr_in6 *) p->ifa_addr)->sin6_addr, 16);
        if (p->ifa_netmask) memcpy(m, &((const struct sockaddr_in6 *) p->ifa_netmask)->sin6_addr, 16);
        bool link = a[0] == 0xfe && (a[1] & 0xc0) == 0x80;
#ifdef __APPLE__
        if (link) a[2] = a[3] = 0; // Darwin embeds the scope in bytes 2..3
#endif
        bool loop = !memcmp(a, (uint8_t[16]) {[15] = 1}, 16);
        unsigned prefix = 0;
        for (int i = 0; i < 16 && m[i] == 0xff; i++) prefix += 8;
        for (int i = prefix / 8; i < 16 && prefix % 8 == 0 && m[i]; i++)
            for (uint8_t bit = 0x80; bit && (m[i] & bit); bit >>= 1) prefix++;
        unsigned scope = loop ? 0x10 : link ? 0x20 : (a[0] == 0xfe && (a[1] & 0xc0) == 0xc0) ? 0x40 : 0;
        for (int i = 0; i < 16; i++) proc_printf(buf, "%02x", a[i]);
        proc_printf(buf, " %02x %02x %02x %02x %8s\n", index, prefix, scope, 0x80, p->ifa_name);
    }
    freeifaddrs(all);
    return 0;
}

struct proc_children proc_net_children = PROC_CHILDREN({
    {"dev", .show = proc_show_net_dev},
    {"if_inet6", .show = proc_show_net_if_inet6},
    {"route", .show = proc_show_net_route},
});
