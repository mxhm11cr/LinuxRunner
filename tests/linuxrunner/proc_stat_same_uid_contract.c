// A non-root process reads /proc/<pid>/stat of another process with the same
// uid, as Chromium/Electron do for their helpers. kstkesp/kstkeip need the
// inspect check, which once took the target's general_lock while stat already
// held it: the reader deadlocked with pids_lock held and froze every task.
// Run as root (it drops to uid/gid 1000). A watchdog process reports a hang.
// Build: gcc -O2 proc_stat_same_uid_contract.c   Prints PROC_STAT_SAME_UID_CONTRACT PASS.
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

static int fields(const char *path, unsigned long *sp, unsigned long *pc) {
    char buf[2048];
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char *p = strrchr(buf, ')');
    if (!p) return -1;
    int count = 2;
    for (char *tok = strtok(p + 1, " \n"); tok; tok = strtok(NULL, " \n")) {
        count++;
        if (count == 29) *sp = strtoul(tok, NULL, 10);
        if (count == 30) *pc = strtoul(tok, NULL, 10);
    }
    return count;
}

int main(void) {
    if (getuid() != 0) { puts("PROC_STAT_SAME_UID_CONTRACT SKIP run as root"); return 0; }
    // Watchdog in its own process: a deadlocked guest stops the reader, not this timer.
    pid_t reader = fork();
    if (reader == 0) {
        if (setgid(1000) || setuid(1000)) { perror("setuid"); _exit(2); }
        // Changing credentials clears dumpable, as on Linux; restore it so the
        // sibling (forked after) is inspectable by its own uid.
        prctl(PR_SET_DUMPABLE, 1);
        pid_t sibling = fork();
        if (sibling == 0) { for (;;) pause(); }
        char path[64];
        snprintf(path, sizeof path, "/proc/%d/stat", sibling);
        int fails = 0;
        for (int i = 0; i < 200; i++) {
            unsigned long sp = 0, pc = 0;
            int n = fields(path, &sp, &pc);
            if (n < 30) { printf("PROC_STAT_SAME_UID_CONTRACT FAIL fields=%d\n", n); fails++; break; }
            // Same uid and dumpable: inspectable, so the stack pointer is reported.
            if (sp == 0) { printf("PROC_STAT_SAME_UID_CONTRACT FAIL kstkesp=0 for an inspectable task\n"); fails++; break; }
        }
        fflush(stdout);
        kill(sibling, SIGKILL);
        waitpid(sibling, NULL, 0);
        _exit(fails != 0);
    }
    for (int t = 0; t < 300; t++) {
        int status;
        if (waitpid(reader, &status, WNOHANG) == reader) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) { puts("PROC_STAT_SAME_UID_CONTRACT PASS"); return 0; }
            printf("PROC_STAT_SAME_UID_CONTRACT FAIL reader status=%#x\n", status);
            return 1;
        }
        usleep(100000);
    }
    puts("PROC_STAT_SAME_UID_CONTRACT FAIL reader hung for 30 s");
    return 1;
}
