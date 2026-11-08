#ifndef FS_ERRQUEUE_H
#define FS_ERRQUEUE_H
// Linux IP_RECVERR / MSG_ERRQUEUE for IPv4 datagram sockets.
//
// Darwin has no socket error queue. Errors are reconstructed only from what
// the host really reports: ICMP errors received by a companion unprivileged
// ICMP datagram socket (outer and quoted headers are real packet bytes and are
// matched to the guest socket's local port and peer), and local EMSGSIZE from
// a send with DF set. Nothing is synthesized for packets the host never saw.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <netinet/in.h>

struct fd;

#define IP_ERRQUEUE_PAYLOAD 576

struct ip_queued_error {
    uint32_t ee_errno; // positive Linux errno
    uint8_t origin;    // SO_EE_ORIGIN_LOCAL (1) or SO_EE_ORIGIN_ICMP (2)
    uint8_t type, code;
    uint32_t info;     // next-hop MTU for EMSGSIZE / ICMP frag-needed
    bool has_offender;
    struct in_addr offender;
    struct in_addr dst; // original destination (network order)
    uint16_t dst_port;  // network order
    uint8_t ttl;        // outer IP TTL of the ICMP error (0 if local)
    size_t payload_len;
    uint8_t payload[IP_ERRQUEUE_PAYLOAD];
};

// IP_RECVERR on/off for an AF_INET SOCK_DGRAM socket. Returns 0 or a
// negative guest errno (for example when the host refuses ICMP sockets).
int ip_errqueue_set(struct fd *sock, bool enable);
bool ip_errqueue_enabled(struct fd *sock);
void ip_errqueue_free(struct fd *sock);
// Drains host ICMP into the queue; true when an error is waiting.
bool ip_errqueue_pending(struct fd *sock);
// 0 with *out filled, or _EAGAIN (Linux never blocks on the error queue).
int ip_errqueue_pop(struct fd *sock, struct ip_queued_error *out);
// Record a local error for a datagram to dst (errno positive Linux value).
void ip_errqueue_local(struct fd *sock, uint32_t linux_errno, uint32_t info,
                       const struct sockaddr_in *dst, const void *payload, size_t len);
// Host descriptor that becomes readable when a host ICMP error may be queued
// for this socket (for poll registration), or -1. Sockets only.
int ip_errqueue_host_fd(struct fd *sock);
// MTU of the interface the host would use to reach dst, or 0 if unknown.
uint32_t ip_errqueue_route_mtu(const struct sockaddr_in *dst);

#endif
