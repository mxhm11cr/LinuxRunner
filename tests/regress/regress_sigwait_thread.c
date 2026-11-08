// A thread blocked in sigwait() must receive a signal sent to it with
// pthread_kill(), whatever the signal's disposition: Linux never ignores a
// blocked signal (kernel/signal.c sig_ignored() checks blocked/real_blocked).
// MariaDB stops its signal thread this way (SIGTERM, disposition SIG_IGN in
// bootstrap); LinuxRunner dropped it and mariadb-install-db hung forever,
// which also froze apt while installing Kali's standard tools.
// A process-directed signal (kill) blocked by the main thread must reach the
// thread that sigwaits for it (MariaDB: SIGTERM stayed pending on the main
// thread, its signal thread never woke, mariadb-install-db hung).
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int got;
static void *waiter(void *arg) {
    sigset_t *set = arg; int sig = 0;
    int r = sigwait(set, &sig);
    got = r ? -r : sig;
    return NULL;
}
static void caught(int sig) { (void) sig; }
static int run_mode(int sig, void (*disposition)(int), const char *name, int process_directed) {
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = disposition; sigemptyset(&sa.sa_mask);
    if (sigaction(sig, &sa, NULL)) { perror("sigaction"); return 1; }
    sigset_t set; sigemptyset(&set); sigaddset(&set, sig);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    got = 0; pthread_t t;
    if (pthread_create(&t, NULL, waiter, &set)) return 1;
    struct timespec pause = {0, 200 * 1000 * 1000}; nanosleep(&pause, NULL);
    if (process_directed ? kill(getpid(), sig) : pthread_kill(t, sig)) { puts("send failed"); return 1; }
    for (int i = 0; i < 50 && !got; i++) nanosleep(&pause, NULL);
    if (got != sig) { printf("FAIL %s: sigwait thread did not receive signal %d (got %d)\n", name, sig, got); return 1; }
    pthread_join(t, NULL);
    sigset_t pend; sigpending(&pend);
    if (sigismember(&pend, sig)) { printf("FAIL %s: signal also left pending\n", name); return 1; }
    printf("ok %s\n", name);
    return 0;
}
static int run(int sig, void (*disposition)(int), const char *name) { return run_mode(sig, disposition, name, 0); }
int main(void) {
    int f = 0;
    f |= run(SIGUSR2, SIG_DFL, "thread-directed blocked SIGUSR2, default disposition");
    f |= run(SIGTERM, SIG_IGN, "thread-directed blocked SIGTERM, ignored disposition");
    f |= run(SIGQUIT, SIG_IGN, "thread-directed blocked SIGQUIT, ignored disposition");
    f |= run_mode(SIGTERM, caught, "process-directed SIGTERM blocked in main, handler installed", 1);
    f |= run_mode(SIGHUP, SIG_DFL, "process-directed SIGHUP blocked in main, default disposition", 1);
    // An ignored, unblocked signal is still discarded (no queueing).
    signal(SIGUSR1, SIG_IGN); sigset_t one; sigemptyset(&one); sigaddset(&one, SIGUSR1);
    pthread_sigmask(SIG_UNBLOCK, &one, NULL); kill(getpid(), SIGUSR1);
    sigset_t pend; sigpending(&pend);
    if (sigismember(&pend, SIGUSR1)) { puts("FAIL: ignored unblocked SIGUSR1 left pending"); f = 1; }
    if (!f) puts("REGRESS_SIGWAIT_THREAD_PASS");
    return f;
}
