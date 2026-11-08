// IP socket-option and route-lookup contract (Build 902 networking).
// iputils ping sets IP_RETOPTS (Linux's IP_RECVRETOPTS) and reads the TTL from
// an IP_TTL control message; tracepath sets IP_MTU_DISCOVER; "ip route get"
// sends a non-dump RTM_GETROUTE. Loopback only; no external traffic.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <time.h> // before linux/errqueue.h (musl)
#include <linux/errqueue.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d %s (errno=%d %s)\n", __FILE__, __LINE__, #x, errno, strerror(errno)); failures++; } } while (0)

static void mtu_discover(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(s >= 0);
    int v = -1; socklen_t len = sizeof(v);
    CHECK(getsockopt(s, IPPROTO_IP, IP_MTU_DISCOVER, &v, &len) == 0 && len == sizeof(v));
    v = IP_PMTUDISC_PROBE;
    CHECK(setsockopt(s, IPPROTO_IP, IP_MTU_DISCOVER, &v, sizeof(v)) == 0);
    v = -1; len = sizeof(v);
    CHECK(getsockopt(s, IPPROTO_IP, IP_MTU_DISCOVER, &v, &len) == 0 && v == IP_PMTUDISC_PROBE);
    v = 9; errno = 0;
    CHECK(setsockopt(s, IPPROTO_IP, IP_MTU_DISCOVER, &v, sizeof(v)) == -1 && errno == EINVAL);
    v = IP_PMTUDISC_DONT;
    CHECK(setsockopt(s, IPPROTO_IP, IP_MTU_DISCOVER, &v, sizeof(v)) == 0);
    int one = 1;
    CHECK(setsockopt(s, IPPROTO_IP, IP_RETOPTS, &one, sizeof(one)) == 0);
    close(s);
    int t = socket(AF_INET, SOCK_STREAM, 0);
    v = IP_PMTUDISC_DO;
    CHECK(setsockopt(t, IPPROTO_IP, IP_MTU_DISCOVER, &v, sizeof(v)) == 0);
    close(t);
}

static void recv_ttl(void) {
    int rx = socket(AF_INET, SOCK_DGRAM, 0), tx = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(rx >= 0 && tx >= 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t alen = sizeof(a);
    CHECK(bind(rx, (struct sockaddr *) &a, sizeof(a)) == 0);
    CHECK(getsockname(rx, (struct sockaddr *) &a, &alen) == 0);
    int one = 1, ttl = 37;
    CHECK(setsockopt(rx, IPPROTO_IP, IP_RECVTTL, &one, sizeof(one)) == 0);
    CHECK(setsockopt(tx, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) == 0);
    CHECK(sendto(tx, "ttl", 3, 0, (struct sockaddr *) &a, sizeof(a)) == 3);
    char data[8], control[256];
    struct iovec iov = {data, sizeof(data)};
    struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
    CHECK(recvmsg(rx, &m, 0) == 3);
    int got = -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_TTL && c->cmsg_len == CMSG_LEN(sizeof(int)))
            memcpy(&got, CMSG_DATA(c), sizeof(got));
    CHECK(got == 37);
    close(rx); close(tx);
}

static void route_get(void) {
    int s = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (s < 0) { printf("SKIP route_get: route netlink unavailable (errno=%d)\n", errno); return; }
    struct { struct nlmsghdr h; struct rtmsg r; struct rtattr a; struct in_addr dst; } req = {
        .h = {sizeof(req), RTM_GETROUTE, NLM_F_REQUEST, 7, 0},
        .r = {.rtm_family = AF_INET, .rtm_dst_len = 32},
        .a = {RTA_LENGTH(4), RTA_DST},
        .dst = {htonl(INADDR_LOOPBACK)},
    };
    struct sockaddr_nl kernel = {.nl_family = AF_NETLINK};
    CHECK(sendto(s, &req, sizeof(req), 0, (struct sockaddr *) &kernel, sizeof(kernel)) == sizeof(req));
    char buf[4096];
    ssize_t n = recv(s, buf, sizeof(buf), 0);
    CHECK(n > (ssize_t) sizeof(struct nlmsghdr));
    struct nlmsghdr *h = (void *) buf;
    CHECK(h->nlmsg_type == RTM_NEWROUTE && h->nlmsg_seq == 7);
    if (h->nlmsg_type == RTM_NEWROUTE) {
        struct rtmsg *r = NLMSG_DATA(h);
        int len = RTM_PAYLOAD(h);
        bool have_dst = false, have_oif = false, have_src = false;
        for (struct rtattr *a = RTM_RTA(r); RTA_OK(a, len); a = RTA_NEXT(a, len)) {
            if (a->rta_type == RTA_DST) have_dst = !memcmp(RTA_DATA(a), &req.dst, 4);
            if (a->rta_type == RTA_OIF) have_oif = true;
            if (a->rta_type == RTA_PREFSRC) have_src = true;
        }
        CHECK(r->rtm_family == AF_INET && r->rtm_dst_len == 32 && r->rtm_type == RTN_LOCAL);
        CHECK(have_dst && have_oif && have_src);
    }
    close(s);
}

// tracepath/traceroute contract: IP_RECVERR + MSG_ERRQUEUE.
static long elapsed_ms(struct timespec a) {
    struct timespec b; clock_gettime(CLOCK_MONOTONIC, &b);
    return (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
}
static void error_queue(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(s >= 0);
    int one = 1;
    CHECK(setsockopt(s, IPPROTO_IP, IP_RECVERR, &one, sizeof(one)) == 0);
    int v = 0; socklen_t len = sizeof(v);
    CHECK(getsockopt(s, IPPROTO_IP, IP_RECVERR, &v, &len) == 0 && v == 1);
    char data[64], control[256];
    struct iovec iov = {data, sizeof(data)};
    struct sockaddr_in name;
    struct msghdr m = {.msg_name = &name, .msg_namelen = sizeof(name), .msg_iov = &iov, .msg_iovlen = 1,
                       .msg_control = control, .msg_controllen = sizeof(control)};
    // An empty error queue never blocks, even on a blocking socket.
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    errno = 0;
    CHECK(recvmsg(s, &m, MSG_ERRQUEUE) == -1 && errno == EAGAIN);
    CHECK(elapsed_ms(t0) < 200);
    // A datagram to a closed loopback port earns a real ICMP port-unreachable.
    struct sockaddr_in closed = {.sin_family = AF_INET, .sin_port = htons(9), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(sendto(s, "probe!", 6, 0, (struct sockaddr *) &closed, sizeof(closed)) == 6);
    struct pollfd p = {.fd = s, .events = POLLIN};
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK(poll(&p, 1, 3000) == 1 && (p.revents & POLLERR));
    CHECK(elapsed_ms(t0) < 500); // woken by the error, not by a timeout
    m.msg_controllen = sizeof(control); m.msg_namelen = sizeof(name);
    ssize_t n = recvmsg(s, &m, MSG_ERRQUEUE);
    // Payload is whatever the ICMP sender quoted: Linux quotes up to 576 bytes,
    // Darwin's own loopback port-unreachable quotes only the UDP header.
    errno = 0;
    CHECK(n >= 0 && n <= 6 && memcmp(data, "probe!", (size_t) n) == 0);
    CHECK(m.msg_flags & MSG_ERRQUEUE);
    CHECK(name.sin_port == htons(9) && name.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    struct sock_extended_err *ee = NULL;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_IP && c->cmsg_type == IP_RECVERR) ee = (void *) CMSG_DATA(c);
    CHECK(ee != NULL);
    if (ee) {
        CHECK(ee->ee_errno == ECONNREFUSED && ee->ee_origin == SO_EE_ORIGIN_ICMP && ee->ee_type == 3 && ee->ee_code == 3);
        struct sockaddr_in *offender = (void *) SO_EE_OFFENDER(ee);
        CHECK(offender->sin_family == AF_INET && offender->sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    }
    // A DF datagram above the path limit fails with EMSGSIZE and queues the MTU.
    int probe = IP_PMTUDISC_PROBE;
    CHECK(setsockopt(s, IPPROTO_IP, IP_MTU_DISCOVER, &probe, sizeof(probe)) == 0);
    static char big[60000];
    errno = 0;
    CHECK(sendto(s, big, sizeof(big), 0, (struct sockaddr *) &closed, sizeof(closed)) == -1 && errno == EMSGSIZE);
    m.msg_controllen = sizeof(control); m.msg_namelen = sizeof(name);
    n = recvmsg(s, &m, MSG_ERRQUEUE);
    CHECK(n >= 0);
    ee = NULL;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); n >= 0 && c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_IP && c->cmsg_type == IP_RECVERR) ee = (void *) CMSG_DATA(c);
    CHECK(ee && ee->ee_errno == EMSGSIZE && ee->ee_origin == SO_EE_ORIGIN_LOCAL && ee->ee_info >= 576 && ee->ee_info < sizeof(big));
    close(s);
}

int main(void) {
    mtu_discover();
    recv_ttl();
    route_get();
    error_queue();
    if (failures) { fprintf(stderr, "regress_ip_sockopts: %d failure(s)\n", failures); return 1; }
    puts("regress_ip_sockopts: PASS");
    return 0;
}
