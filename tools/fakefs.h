#ifndef FS_FAKEFSIFY_H
#define FS_FAKEFSIFY_H
#include <stdbool.h>

struct fakefsify_error {
    int line;
    enum {
        ERR_ARCHIVE,
        ERR_SQLITE,
        ERR_POSIX,
        ERR_CANCELLED,
    } type;
    int code;
    char *message;
};

struct progress {
    void *cookie;
    void (*callback)(void *cookie, double progress, const char *message, bool *cancel_out);
};

bool fakefs_import(const char *archive_path, const char *fs, struct fakefsify_error *err_out, struct progress progress);
// Like fakefs_import for archives whose root is one top-level directory
// (strip_prefix, e.g. "kali-arm64"); any entry outside it fails the import.
bool fakefs_import_prefix(const char *archive_path, const char *fs, const char *strip_prefix, struct fakefsify_error *err_out, struct progress progress);
// Imports an uncompressed or gzip/xz tar stream read from archive_fd (not closed).
// Progress callbacks receive -1: the producer reports progress.
bool fakefs_import_fd(int archive_fd, const char *fs, struct fakefsify_error *err_out, struct progress progress);
bool fakefs_export(const char *fs, const char *archive_path, struct fakefsify_error *err_out, struct progress progress);

#endif
