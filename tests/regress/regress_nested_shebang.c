// execve of a script whose interpreter is itself a script (Linux
// binfmt_script recursion, up to 4 levels, then ELOOP), and tabs as #!
// separators. Kali's msfconsole: #!/usr/share/metasploit-framework/ruby,
// which is a #!/bin/sh wrapper; LinuxRunner returned ENOEXEC, so tools fell
// back to running the Ruby file as a shell script.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void put(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0 || write(fd, text, strlen(text)) != (ssize_t) strlen(text)) { perror(path); exit(2); }
    close(fd); chmod(path, 0755);
}
// Runs path with one argument; returns its first output line, or the errno.
static int run(const char *path, char *out, size_t size) {
    int p[2]; if (pipe(p)) return -1;
    pid_t pid = fork();
    if (pid == 0) { dup2(p[1], 1); close(p[0]); char *argv[] = {(char *) path, "arg1", NULL}; execv(path, argv); _exit(100 + errno); }
    close(p[1]); ssize_t n = read(p[0], out, size - 1); out[n > 0 ? n : 0] = 0; close(p[0]);
    int status; waitpid(pid, &status, 0);
    char *nl = strchr(out, '\n'); if (nl) *nl = 0;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
int main(void) {
    char d[] = "/tmp/lr-shebang-XXXXXX"; if (!mkdtemp(d)) return 2;
    char w[256], s[256], t[256], chain[6][256], buf[512], text[512]; int f = 0;
    snprintf(w, sizeof w, "%s/wrapper", d); snprintf(s, sizeof s, "%s/script", d); snprintf(t, sizeof t, "%s/tabbed", d);
    put(w, "#!/bin/sh\necho \"wrapper:$1:$2\"\n");
    snprintf(text, sizeof text, "#!%s\nthis line is not shell\n", w); put(s, text);
    int code = run(s, buf, sizeof buf);
    snprintf(text, sizeof text, "wrapper:%s:arg1", s);
    if (code != 0 || strcmp(buf, text)) { printf("FAIL nested interpreter: exit=%d out=%s\n", code, buf); f = 1; } else puts("ok script interpreter is a script");
    put(t, "#!\t/bin/sh\t-e\necho tab-ok\n");
    code = run(t, buf, sizeof buf);
    if (code != 0 || strcmp(buf, "tab-ok")) { printf("FAIL tab separator: exit=%d out=%s\n", code, buf); f = 1; } else puts("ok tab-separated #! line");
    // 6 scripts each naming the next: deeper than Linux's 4 levels -> ELOOP.
    for (int i = 0; i < 6; i++) snprintf(chain[i], sizeof chain[i], "%s/c%d", d, i);
    put(chain[5], "#!/bin/sh\necho deep\n");
    for (int i = 4; i >= 0; i--) { snprintf(text, sizeof text, "#!%s\n", chain[i + 1]); put(chain[i], text); }
    code = run(chain[0], buf, sizeof buf);
    if (code != 100 + ELOOP) { printf("FAIL depth limit: exit=%d (want ELOOP)\n", code); f = 1; } else puts("ok ELOOP beyond 4 nested interpreters");
    if (!f) puts("REGRESS_NESTED_SHEBANG_PASS");
    return f;
}
