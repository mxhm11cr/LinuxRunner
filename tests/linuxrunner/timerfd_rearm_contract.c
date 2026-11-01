// timerfd as event loops use it (Hyprland, libwayland, GLib): one timerfd
// re-armed with absolute CLOCK_MONOTONIC deadlines. A deadline that has
// already passed must fire at once; re-arming a long timer to a short one
// must fire at the short deadline even when done in a tight loop; re-arming
// discards unread expirations; a zero value disarms.
// Prints TIMERFD_REARM_CONTRACT PASS.
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

static int fails;
static void check(const char *step, int ok) { if (!ok) { printf("TIMERFD_REARM_CONTRACT FAIL %s\n", step); fails++; } }
static struct timespec now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t; }
static struct timespec plus_ms(struct timespec t, long ms) { t.tv_nsec += (ms % 1000) * 1000000; t.tv_sec += ms / 1000 + t.tv_nsec / 1000000000; t.tv_nsec %= 1000000000; return t; }
static int readable(int fd, int ms) { struct pollfd p = {fd, POLLIN, 0}; return poll(&p, 1, ms) == 1; }
static void arm_abs(int fd, struct timespec at) { struct itimerspec v = {{0, 0}, at}; timerfd_settime(fd, TFD_TIMER_ABSTIME, &v, NULL); }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    check("create", fd >= 0);
    uint64_t n;
    // 1. Absolute deadline already in the past (and exactly "now") fires immediately.
    arm_abs(fd, plus_ms(now(), -50));
    check("past deadline fires", readable(fd, 2000) && read(fd, &n, 8) == 8 && n == 1);
    arm_abs(fd, now());
    check("deadline == now fires", readable(fd, 2000) && read(fd, &n, 8) == 8);
    // 2. Long timer re-armed to short, many times: each must fire promptly.
    int late = 0;
    for (int i = 0; i < 200; i++) {
        arm_abs(fd, plus_ms(now(), 60000));   // far deadline
        arm_abs(fd, plus_ms(now(), 2));       // then the next frame
        if (!readable(fd, 3000)) { late++; continue; }
        read(fd, &n, 8);
    }
    printf("TIMERFD_REARM_CONTRACT rearm_late=%d of 200\n", late);
    check("re-armed short deadline fires", late == 0);
    // 3. Re-arming discards unread expirations.
    arm_abs(fd, plus_ms(now(), -1));
    readable(fd, 2000);
    arm_abs(fd, plus_ms(now(), 60000));
    check("re-arm clears expirations", !readable(fd, 50) && read(fd, &n, 8) < 0 && errno == EAGAIN);
    // 4. Zero value disarms.
    struct itimerspec zero = {{0, 0}, {0, 0}}, cur;
    timerfd_settime(fd, 0, &zero, NULL); timerfd_gettime(fd, &cur);
    check("zero disarms", cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec == 0 && !readable(fd, 50));
    if (!fails) puts("TIMERFD_REARM_CONTRACT PASS");
    return fails != 0;
}
