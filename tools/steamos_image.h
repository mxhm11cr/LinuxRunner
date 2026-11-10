#ifndef LR_STEAMOS_IMAGE_H
#define LR_STEAMOS_IMAGE_H

// Reads Valve's published SteamFrame recovery image without a helper Linux
// system: the bzip2 disk image is decompressed only as far as the requested
// GPT partition, and that read-only Btrfs filesystem is streamed as a tar
// archive (owners, modes, times, symlinks, hard links, file capabilities).
// Nothing is executed from the image and the image is never modified.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Return false to cancel. fraction is 0...1; detail may be NULL.
typedef bool (*lr_steamos_progress_fn)(void *context, double fraction, const char *detail);

// SHA-256 of a file as 64 lowercase hex characters plus NUL.
int lr_steamos_sha256_file(const char *path, char hex[65], lr_steamos_progress_fn progress, void *context,
                           char *error, size_t error_size);

// Writes partition `name` (GPT name, e.g. "rootfs-A") from a .img or .img.bz2
// disk image to out_path (created exclusively). Stops decompressing once the
// partition is complete.
int lr_steamos_extract_partition(const char *image_path, const char *name, const char *out_path,
                                 lr_steamos_progress_fn progress, void *context, char *error, size_t error_size);

// Same, writing to an open descriptor (pipe or file).
int lr_steamos_extract_partition_fd(const char *image_path, const char *name, int out_fd,
                                    lr_steamos_progress_fn progress, void *context, char *error, size_t error_size);

// Streams the Btrfs filesystem in image_path as an uncompressed POSIX/PAX tar
// to out_fd, leaving out kernel, firmware, debug data and boot partitions
// (same exclusions as tools/steamos/export-steamframe.py). Entries are
// relative ("usr/bin/bash"). Device nodes, sockets and FIFOs are not written.
int lr_steamos_btrfs_export(const char *image_path, int out_fd, lr_steamos_progress_fn progress, void *context,
                            char *error, size_t error_size);

// The install stream: the exported image followed by pinned Arch Linux ARM
// packages (.pkg.tar.xz/.zst, verified by the caller) with their pacman local
// database entries under usr/lib/holo/pacmandb, and LinuxRunner's boot script.
int lr_steamos_install_stream(const char *image_path, const char *const *packages, size_t package_count, int out_fd,
                              lr_steamos_progress_fn progress, void *context, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif
#endif
