// Guest contract: rmdir(2) and unlinkat(AT_REMOVEDIR) never follow a symlink
// in the final path component; they fail with ENOTDIR and leave both the link
// and its target alone. dpkg's path_remove_tree() depends on this to clear
// stale "*.dpkg-new" symlinks left by an interrupted unpack.
//   gcc -O2 rmdir_symlink_contract.c -o rmdir-symlink && ./rmdir-symlink
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
static void expect(int ok, const char *name) {
    printf("RMDIR_SYMLINK: %s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    char base[] = "/tmp/rmdir-symlink.XXXXXX";
    if (!mkdtemp(base)) { perror("mkdtemp"); return 2; }
    if (chdir(base)) { perror("chdir"); return 2; }
    mkdir("emptydir", 0755);
    close(open("file", O_CREAT | O_WRONLY, 0644));
    symlink("nowhere", "dangling");
    symlink("file", "tofile");
    symlink("emptydir", "toempty");
    struct stat st;

    errno = 0; expect(rmdir("dangling") == -1 && errno == ENOTDIR, "dangling-link-ENOTDIR");
    expect(lstat("dangling", &st) == 0 && S_ISLNK(st.st_mode), "dangling-link-kept");
    errno = 0; expect(rmdir("tofile") == -1 && errno == ENOTDIR, "file-link-ENOTDIR");
    errno = 0; expect(rmdir("toempty") == -1 && errno == ENOTDIR, "empty-dir-link-ENOTDIR");
    expect(stat("emptydir", &st) == 0 && S_ISDIR(st.st_mode), "empty-dir-target-kept");
    errno = 0; expect(unlinkat(AT_FDCWD, "toempty", AT_REMOVEDIR) == -1 && errno == ENOTDIR, "unlinkat-removedir-ENOTDIR");
    expect(stat("emptydir", &st) == 0, "empty-dir-target-kept-after-unlinkat");
    // The dpkg sequence: the stale link is removed with unlink, then recreated.
    expect(unlink("dangling") == 0 && symlink("again", "dangling") == 0, "stale-link-replaced");
    errno = 0; expect(rmdir("file") == -1 && errno == ENOTDIR, "regular-file-ENOTDIR");
    expect(rmdir("emptydir") == 0, "real-empty-dir-removed");

    unlink("dangling"); unlink("tofile"); unlink("toempty"); unlink("file");
    chdir("/"); rmdir(base);
    printf("RMDIR_SYMLINK_RESULT %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
