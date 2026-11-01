// ISH_GUEST_LISTEN=loopback contract: wildcard binds and implicit listen()
// binds land on loopback; explicit addresses are unchanged. Run with argument
// "loopback" (policy on) or "wildcard" (policy off). Prints one line per case
// and the bound ports, then holds the sockets for host-side inspection when
// LR_LISTEN_HOLD_SECONDS is set.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures;

static void check(const char *name, int ok) {
    printf("LISTEN_CASE %s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int v4(const char *name, const char *bind_addr, int expect_loopback) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET};
    if (bind_addr) {
        inet_pton(AF_INET, bind_addr, &a.sin_addr);
        if (bind(fd, (void *) &a, sizeof(a)) < 0) { perror("bind"); check(name, 0); return -1; }
    }
    if (listen(fd, 4) < 0) { perror("listen"); check(name, 0); return -1; }
    socklen_t len = sizeof(a);
    getsockname(fd, (void *) &a, &len);
    char text[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a.sin_addr, text, sizeof(text));
    printf("LISTEN_BOUND %s %s:%u\n", name, text, ntohs(a.sin_port));
    uint32_t want = expect_loopback ? htonl(INADDR_LOOPBACK) : htonl(INADDR_ANY);
    check(name, a.sin_port != 0 && a.sin_addr.s_addr == want);
    // A loopback client must still reach the server either way.
    int c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = a.sin_port, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    int connected = connect(c, (void *) &to, sizeof(to)) == 0;
    int accepted = connected ? accept(fd, NULL, NULL) : -1;
    char reached[96];
    snprintf(reached, sizeof(reached), "%s-loopback-client", name);
    check(reached, connected && accepted >= 0);
    close(c);
    if (accepted >= 0) close(accepted);
    return fd;
}

static int v6(const char *name, int expect_loopback) {
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
    struct sockaddr_in6 a = {.sin6_family = AF_INET6, .sin6_addr = in6addr_any};
    if (bind(fd, (void *) &a, sizeof(a)) < 0 || listen(fd, 4) < 0) { perror("v6"); check(name, 0); return -1; }
    socklen_t len = sizeof(a);
    getsockname(fd, (void *) &a, &len);
    char text[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &a.sin6_addr, text, sizeof(text));
    printf("LISTEN_BOUND %s [%s]:%u\n", name, text, ntohs(a.sin6_port));
    check(name, a.sin6_port != 0 && (expect_loopback ? IN6_IS_ADDR_LOOPBACK(&a.sin6_addr) : IN6_IS_ADDR_UNSPECIFIED(&a.sin6_addr)));
    return fd;
}

int main(int argc, char **argv) {
    if (argc != 2 || (strcmp(argv[1], "loopback") && strcmp(argv[1], "wildcard"))) return 2;
    int loopback = strcmp(argv[1], "loopback") == 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    v4("ipv4-wildcard-bind", "0.0.0.0", loopback);
    v4("ipv4-implicit-listen", NULL, loopback);
    v4("ipv4-explicit-loopback", "127.0.0.1", 1);
    v6("ipv6-wildcard-bind", loopback);
    const char *hold = getenv("LR_LISTEN_HOLD_SECONDS");
    if (hold) { printf("LISTEN_HOLD\n"); sleep(atoi(hold)); }
    printf("LISTEN_RESULT %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
