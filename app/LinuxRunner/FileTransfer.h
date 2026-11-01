#pragma once
#include <stdbool.h>
#include <stdint.h>

// Single directory level; fd-relative access refuses symlinks and special files.
bool lr_valid_leaf(const char *name);
int lr_copy_in(int source_fd, int directory_fd, const char *name);
int lr_copy_out(int directory_fd, const char *name, int destination_fd);
struct lr_transfer_control {
    bool (*cancelled)(void *);
    void (*progress)(void *, uint64_t, uint64_t);
    void *context;
};
int lr_copy_in_controlled(int source_fd, int directory_fd, const char *name, const struct lr_transfer_control *);
// App-private staging allows safe launch-time recovery without touching Shared.
int lr_copy_in_staged(int source_fd, int directory_fd, const char *name, int staging_fd, const struct lr_transfer_control *);
int lr_clean_import_staging(int staging_fd); // once at launch, before new copies
int lr_copy_out_controlled(int directory_fd, const char *name, int destination_fd, const struct lr_transfer_control *);
// Returns an owned descriptor, or -1 with errno. Empty path means the root.
// Walk each component without following symbolic links, including ancestors.
int lr_open_subdirectory(int root_fd, const char *relative);
