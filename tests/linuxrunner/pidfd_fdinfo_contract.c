// pidfd fdinfo contract: /proc/self/fdinfo/<pidfd> carries "Pid:" (and
// "NSpid:") for the target, -1 after it has exited. dbus-daemon resolves
// SO_PEERPIDFD peers this way; without the line every peer is pid -1.
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef SO_PEERPIDFD
#define SO_PEERPIDFD 77
#endif

static int failures;
static void check(const char *name, int ok) { printf("PIDFD_CASE %s %s\n", name, ok ? "PASS" : "FAIL"); if (!ok) failures++; }

static long fdinfo_pid(int fd) {
    char path[64], line[256];
    snprintf(path, sizeof path, "/proc/self/fdinfo/%d", fd);
    FILE *f = fopen(path, "r");
    if (!f) return -1000;
    long pid = -1000;
    while (fgets(line, sizeof line, f)) if (!strncmp(line, "Pid:", 4)) pid = strtol(line + 4, NULL, 10);
    fclose(f);
    return pid;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    int peer = -1;
    socklen_t len = sizeof peer;
    int got = getsockopt(sv[0], SOL_SOCKET, SO_PEERPIDFD, &peer, &len);
    check("peer-pidfd-created", got == 0 && peer >= 0);
    long pid = fdinfo_pid(peer);
    printf("PIDFD_PEER fdinfo=%ld getpid=%d\n", pid, getpid());
    check("peer-pidfd-fdinfo-pid", pid == getpid());

    pid_t child = fork();
    if (child == 0) { pause(); _exit(0); }
    int cfd = (int) syscall(SYS_pidfd_open, child, 0);
    check("child-pidfd-open", cfd >= 0);
    check("child-pidfd-fdinfo-live", fdinfo_pid(cfd) == child);
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    long after = fdinfo_pid(cfd);
    printf("PIDFD_CHILD exited fdinfo=%ld\n", after);
    check("child-pidfd-fdinfo-exited", after == -1);
    printf("PIDFD_RESULT %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
