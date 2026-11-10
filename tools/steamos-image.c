// Host test tool for tools/steamos_image.c.
//   steamos-image sha256 FILE
//   steamos-image extract IMAGE(.img|.img.bz2) PARTITION OUT.img
//   steamos-image export PARTITION.img OUT.tar|-
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "steamos_image.h"

static bool report(void *context, double fraction, const char *detail) {
    (void) context; (void) detail;
    static int last = -1;
    int percent = (int) (fraction * 100);
    if (percent != last) { fprintf(stderr, "PROGRESS %d\n", percent); last = percent; }
    return true;
}

int main(int argc, char **argv) {
    char error[512] = "";
    if (argc == 3 && !strcmp(argv[1], "sha256")) {
        char hex[65];
        if (lr_steamos_sha256_file(argv[2], hex, NULL, NULL, error, sizeof(error))) { fprintf(stderr, "ERROR %s\n", error); return 1; }
        printf("%s  %s\n", hex, argv[2]);
        return 0;
    }
    if (argc == 5 && !strcmp(argv[1], "extract")) {
        int failed = strcmp(argv[4], "-") ? lr_steamos_extract_partition(argv[2], argv[3], argv[4], report, NULL, error, sizeof(error))
                                          : lr_steamos_extract_partition_fd(argv[2], argv[3], STDOUT_FILENO, report, NULL, error, sizeof(error));
        if (failed) { fprintf(stderr, "ERROR %s\n", error); return 1; }
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "export")) {
        int fd = strcmp(argv[3], "-") ? open(argv[3], O_WRONLY | O_CREAT | O_EXCL, 0644) : STDOUT_FILENO;
        if (fd < 0) { perror(argv[3]); return 1; }
        if (lr_steamos_btrfs_export(argv[2], fd, report, NULL, error, sizeof(error))) { fprintf(stderr, "ERROR %s\n", error); return 1; }
        return close(fd) ? 1 : 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "install-stream")) {
        if (lr_steamos_install_stream(argv[2], (const char *const *) argv + 4, (size_t) argc - 4, STDOUT_FILENO, report, NULL, error, sizeof(error))) { fprintf(stderr, "ERROR %s\n", error); return 1; }
        return 0;
    }
    fprintf(stderr, "usage: steamos-image sha256 FILE | extract IMAGE PARTITION OUT | export PARTITION OUT.tar|-\n");
    return 2;
}
