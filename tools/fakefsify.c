#include <stdio.h>
#include <string.h>
#include <libgen.h>

#define ISH_INTERNAL
#include "fs/fake.h"
#include "tools/fakefs.h"

enum cmd {
    cmd_import,
    cmd_export,
};

int main(int argc, const char *argv[]) {
    enum cmd cmd = cmd_import;
    if (strcmp(basename((char *) argv[0]), "unfakefsify") == 0) {
        cmd = cmd_export;
    }

    if (argc != 3 && !(cmd == cmd_import && argc == 4)) {
        fprintf(stderr, "wrong number of arguments\n");
        switch (cmd) {
            case cmd_import:
                fprintf(stderr, "usage: %s <rootfs.tar.gz> <fakefs> [top-level-dir]\n", argv[0]);
                break;
            case cmd_export:
                fprintf(stderr, "usage: %s <fakefs> <rootfs.tar.gz>\n", argv[0]);
                break;
        }
        return 1;
    }

    struct fakefsify_error err;
    bool (*func)(const char *, const char *, struct fakefsify_error *, struct progress) = NULL;
    if (cmd == cmd_import)
        func = fakefs_import;
    else if (cmd == cmd_export)
        func = fakefs_export;
    // "-" imports a tar stream from standard input (e.g. steamos-image install-stream).
    bool ok = cmd == cmd_import && argc == 3 && strcmp(argv[1], "-") == 0 ? fakefs_import_fd(0, argv[2], &err, (struct progress) {}) :
              argc == 4 ? fakefs_import_prefix(argv[1], argv[2], argv[3], &err, (struct progress) {})
                        : (*func)(argv[1], argv[2], &err, (struct progress) {});
    if (!ok) {
        fprintf(stderr, "error!!1! %d %d %s\n", err.line, err.type, err.message);
        return 1;
    }
}
