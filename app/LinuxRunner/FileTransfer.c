#include "FileTransfer.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <dirent.h>

bool lr_valid_leaf(const char *name) {
    return name && *name && strcmp(name, ".") && strcmp(name, "..") &&
        !strchr(name, '/') && !strchr(name, '\\') && strlen(name) <= 255;
}

int lr_open_subdirectory(int root_fd, const char *relative) {
    if (!relative || strlen(relative) > 4096 || *relative == '/') { errno = EINVAL; return -1; }
    int current = openat(root_fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (current < 0 || !*relative) return current;
    char path[4097]; strcpy(path, relative);
    char *component = path;
    for (;;) {
        char *slash = strchr(component, '/'); if (slash) *slash = 0;
        if (!lr_valid_leaf(component)) { close(current); errno = EINVAL; return -1; }
        int next = openat(current, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        int error = errno; close(current); if (next < 0) { errno = error; return -1; }
        current = next; if (!slash) return current; component = slash + 1;
    }
}

static int copy_regular(int input, int output, const struct lr_transfer_control *control) {
    struct stat st;
    if (fstat(input, &st) < 0) return errno;
    if (!S_ISREG(st.st_mode)) return EINVAL;
    // Transfers are streamed; bound disk consumption as well as memory.
    const uint64_t limit = 512ULL * 1024 * 1024;
    if (st.st_size < 0 || (uint64_t)st.st_size > limit) return EFBIG;
    char buffer[65536];
    uint64_t total = 0;
    for (;;) {
        if (control && control->cancelled && control->cancelled(control->context)) return ECANCELED;
        ssize_t count = read(input, buffer, sizeof(buffer));
        if (count < 0) { if (errno == EINTR) continue; return errno; }
        if (!count) break;
        total += (uint64_t)count;
        if (total > limit) return EFBIG;
        for (ssize_t offset = 0; offset < count;) {
            ssize_t written = write(output, buffer + offset, (size_t)(count - offset));
            if (written < 0) { if (errno == EINTR) continue; return errno; }
            if (!written) return EIO;
            offset += written;
        }
        if (control && control->progress) control->progress(control->context, total, st.st_size);
    }
    struct stat after;
    if (fstat(input, &after) < 0) return errno;
    // Refuse to publish a concurrently changed source as a verified copy.
    if (total != (uint64_t)st.st_size || after.st_size != st.st_size ||
        after.st_mtime != st.st_mtime || after.st_ctime != st.st_ctime) return EBUSY;
#ifdef __APPLE__
    if (after.st_mtimespec.tv_nsec != st.st_mtimespec.tv_nsec || after.st_ctimespec.tv_nsec != st.st_ctimespec.tv_nsec) return EBUSY;
#else
    if (after.st_mtim.tv_nsec != st.st_mtim.tv_nsec || after.st_ctim.tv_nsec != st.st_ctim.tv_nsec) return EBUSY;
#endif
    return fsync(output) < 0 ? errno : 0;
}

int lr_copy_in(int source_fd, int directory_fd, const char *name) {
    return lr_copy_in_controlled(source_fd, directory_fd, name, NULL);
}
int lr_copy_in_controlled(int source_fd, int directory_fd, const char *name, const struct lr_transfer_control *control) {
    return lr_copy_in_staged(source_fd, directory_fd, name, directory_fd, control);
}
int lr_copy_in_staged(int source_fd, int directory_fd, const char *name, int staging_fd, const struct lr_transfer_control *control) {
    if (!lr_valid_leaf(name)) return EINVAL;
    // Stage under a random name, then publish exclusively without overwriting.
    char staging[80];
    snprintf(staging, sizeof(staging), ".import-%08x-%08x", arc4random(), arc4random());
    int output = openat(staging_fd, staging, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (output < 0) return errno;
    int error = copy_regular(source_fd, output, control);
    if (close(output) < 0 && !error) error = errno;
    if (!error && control && control->cancelled && control->cancelled(control->context)) error = ECANCELED;
    if (!error && linkat(staging_fd, staging, directory_fd, name, 0) < 0) error = errno;
    unlinkat(staging_fd, staging, 0);
    return error;
}

int lr_clean_import_staging(int staging_fd) {
    int copy = openat(staging_fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (copy < 0) return errno;
    DIR *directory = fdopendir(copy);
    if (!directory) { int error = errno; close(copy); return error; }
    int error = 0; struct dirent *entry;
    for (;;) {
        errno = 0; entry = readdir(directory);
        if (!entry) { if (errno && !error) error = errno; break; }
        const char *name = entry->d_name;
        if (strlen(name) != 25 || strncmp(name, ".import-", 8) || name[16] != '-') continue;
        bool valid = true;
        for (int i = 8; i < 25; i++) if (i != 16 && !((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) valid = false;
        if (!valid) continue;
        struct stat st;
        if (!fstatat(staging_fd, name, &st, AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode) && unlinkat(staging_fd, name, 0) < 0) error = errno;
    }
    closedir(directory); return error;
}

int lr_copy_out(int directory_fd, const char *name, int destination_fd) {
    return lr_copy_out_controlled(directory_fd, name, destination_fd, NULL);
}
int lr_copy_out_controlled(int directory_fd, const char *name, int destination_fd, const struct lr_transfer_control *control) {
    if (!lr_valid_leaf(name)) return EINVAL;
    int input = openat(directory_fd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (input < 0) return errno;
    int error = copy_regular(input, destination_fd, control);
    close(input);
    return error;
}
