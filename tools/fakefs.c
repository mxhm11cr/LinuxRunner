#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <archive.h>
#include <archive_entry.h>
#include "platform/guest_file_caps.h"

#define ISH_INTERNAL
#include "fs/fake-db.h"
#include "fs/sqlutil.h"
#include "tools/fakefs.h"
#include "util/fchdir.h"
#include "misc.h"

#ifndef MAX_PATH
#define MAX_PATH 4096
#endif

// I have a weird way of error handling
#define FILL_ERR(_type, _code, _message) do { \
    err_out->line = __LINE__; \
    err_out->type = _type; \
    err_out->code = _code; \
    err_out->message = strdup(_message); \
    return false; \
} while (0)
#define ARCHIVE_ERR(archive) FILL_ERR(ERR_ARCHIVE, archive_errno(archive), archive_error_string(archive))
#define POSIX_ERR() FILL_ERR(ERR_POSIX, errno, strerror(errno))
#undef HANDLE_ERR // for sqlite
#define HANDLE_ERR(db) FILL_ERR(ERR_SQLITE, sqlite3_extended_errcode(db), sqlite3_errmsg(db))
#define CANCEL() FILL_ERR(ERR_CANCELLED, 0, "");

static bool progress_update(struct progress *p, double progress, const char *message) {
    bool cancelled = false;
    if (p && p->callback)
        p->callback(p->cookie, progress, message, &cancelled);
    return !cancelled;
}

// This isn't linked with ish which is why there's so much copy/pasted code

// I hate this code
static bool path_normalize(const char *path, char *out) {
#define ends_path(c) (c == '\0' || c == '/')
    // normalized format:
    // ( '/' path-component ) *
    while (path[0] != '\0') {
        while (path[0] == '/')
            path++;
        if (path[0] == '\0')
            break; // if the path ends with a slash
        // path points to the start of a path component
        if (path[0] == '.' && path[1] == '.' && ends_path(path[2]))
            return false; // no dotdot allowed!
        if (path[0] == '.' && ends_path(path[1])) {
            path++;
        } else {
            *out++ = '/';
            while (path[0] != '/' && path[0] != '\0')
                *out++ = *path++;
        }
    }
    *out = '\0';
    return true;
}

static const char *schema = Q(
    create table meta (id integer unique default 0, db_inode integer);
    insert into meta (db_inode) values (0);
    create table stats (inode integer primary key, stat blob);
    create table paths (path blob primary key, inode integer references stats(inode));
    create index inode_to_path on paths (inode, path);
    // no index is needed on stats, because the rows are ordered by the primary key
    pragma user_version=3;
);

// Moves the normalized path under prefix ("/top") to the root. False when
// the path lies outside the prefix: such an archive is refused, not filtered.
static bool strip_import_prefix(const char *prefix, char *path) {
    if (prefix == NULL || prefix[0] == '\0') return true;
    size_t n = strlen(prefix);
    if (strncmp(path, prefix, n) != 0 || (path[n] != '\0' && path[n] != '/')) return false;
    memmove(path, path + n, strlen(path + n) + 1);
    return true;
}

struct import_resources {
    int root_fd, entry_fd;
    sqlite3 *db;
    struct archive *archive;
};

static bool valid_utf8(const char *text) {
    if (!text) return true;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        unsigned char lead = *p++;
        if (lead < 0x80) continue;
        unsigned count, code, minimum;
        if (lead >= 0xc2 && lead <= 0xdf) { count = 1; code = lead & 0x1f; minimum = 0x80; }
        else if (lead >= 0xe0 && lead <= 0xef) { count = 2; code = lead & 0x0f; minimum = 0x800; }
        else if (lead >= 0xf0 && lead <= 0xf4) { count = 3; code = lead & 7; minimum = 0x10000; }
        else return false;
        while (count--) {
            unsigned char next = *p++;
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}

static bool pax_original_names(struct archive *archive, struct archive_entry *entry) {
    int format = archive_format(archive);
    if (format != ARCHIVE_FORMAT_TAR_PAX_INTERCHANGE && format != ARCHIVE_FORMAT_TAR_PAX_RESTRICTED) return false;
    const char *warning = archive_error_string(archive);
    if (!warning) return false;
    // libarchive's PAX reader explicitly copies the original UTF-8 bytes back
    // into the entry when native-locale conversion fails (pax_header()). Those
    // bytes are the desired Linux names. Accept only this specific warning;
    // malformed UTF-8, checksum/codec errors and every other warning still fail.
    static const char *const messages[] = {
        "Pathname can't be converted from UTF-8 to current locale.",
        "Linkname can't be converted from UTF-8 to current locale.",
        "Uname can't be converted from UTF-8 to current locale.",
        "Gname can't be converted from UTF-8 to current locale.",
    };
    bool conversion = false;
    for (size_t i = 0; i < sizeof messages / sizeof *messages; i++)
        if (!strcmp(warning, messages[i])) conversion = true;
    if (!conversion || !archive_entry_pathname(entry)) return false;
    if (!valid_utf8(archive_entry_pathname(entry)) || !valid_utf8(archive_entry_symlink(entry)) ||
        !valid_utf8(archive_entry_hardlink(entry)) || !valid_utf8(archive_entry_uname(entry)) || !valid_utf8(archive_entry_gname(entry))) return false;
    if (!strncmp(warning, "Linkname", 8) && !archive_entry_symlink(entry) && !archive_entry_hardlink(entry)) return false;
    archive_clear_error(archive);
    return true;
}

static bool fakefs_import_contents(const char *archive_path, int archive_fd, const char *fs, const char *strip_prefix_raw, struct fakefsify_error *err_out, struct progress p, struct import_resources *resources) {
    char strip_prefix_buf[MAX_PATH] = "";
    const char *strip_prefix = NULL;
    if (strip_prefix_raw != NULL && strip_prefix_raw[0] != '\0') {
        if (strlen(strip_prefix_raw) >= sizeof(strip_prefix_buf) || !path_normalize(strip_prefix_raw, strip_prefix_buf) || strip_prefix_buf[0] == '\0') {
            FILL_ERR(ERR_POSIX, EINVAL, "invalid archive top-level directory");
        }
        strip_prefix = strip_prefix_buf;
    }
    int err = mkdir(fs, 0777);
    if (err < 0)
        POSIX_ERR();

    // make the data root dir
    char path_tmp[PATH_MAX];
    snprintf(path_tmp, sizeof(path_tmp), "%s/data", fs);
    err = mkdir(path_tmp, 0777);
    int root_fd = open(path_tmp, O_RDONLY);
    resources->root_fd = root_fd;
    if (root_fd < 0)
        POSIX_ERR();

    // open the database
    snprintf(path_tmp, sizeof(path_tmp), "%s/meta.db", fs);
    sqlite3 *db = NULL;
    err = sqlite3_open_v2(path_tmp, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    resources->db = db;
    CHECK_ERR();
    EXEC("pragma journal_mode=wal")
    EXEC("begin");
    EXEC(schema);

    // open the archive
    struct archive *archive = archive_read_new();
    resources->archive = archive;
    if (archive == NULL)
        ARCHIVE_ERR(archive);
    archive_read_support_filter_gzip(archive);
    archive_read_support_filter_xz(archive); // Kali publishes .tar.xz root filesystems
    archive_read_support_format_tar(archive);
    // A descriptor is a stream (for example a pipe from the SteamOS image
    // reader); its producer reports progress, so only cancellation is polled.
    size_t archive_bytes = 0;
    if (archive_fd >= 0) {
        if (archive_read_open_fd(archive, archive_fd, 65536) != ARCHIVE_OK)
            ARCHIVE_ERR(archive);
    } else {
        if (archive_read_open_filename(archive, archive_path, 65536) != ARCHIVE_OK)
            ARCHIVE_ERR(archive);
        struct stat real_stat;
        if (stat(archive_path, &real_stat) < 0)
            POSIX_ERR();
        archive_bytes = real_stat.st_size;
    }

    sqlite3_stmt *insert_stat = PREPARE("insert into stats (stat) values (?)");
    sqlite3_stmt *insert_path = PREPARE("insert or replace into paths values (?, ?)");
    sqlite3_stmt *insert_hardlink = PREPARE("insert or replace into paths values (?, (select inode from paths where path = ? limit 1))");

    bool archive_has_root = false;

    // do actual shit
    struct archive_entry *entry;
    while ((err = archive_read_next_header(archive, &entry)) == ARCHIVE_OK ||
            (err == ARCHIVE_WARN && pax_original_names(archive, entry))) {
        char entry_path[MAX_PATH];
        if (!path_normalize(archive_entry_pathname(entry), entry_path)) {
            // Avoid pwnage
            fprintf(stderr, "warning: skipped possible path traversal %s\n", archive_entry_pathname(entry));
            continue;
        }
        if (!strip_import_prefix(strip_prefix, entry_path)) {
            FILL_ERR(ERR_POSIX, EINVAL, "archive entry outside its expected top-level directory");
        }
        if (!progress_update(&p, archive_bytes ? (double) archive_filter_bytes(archive, -1) / archive_bytes : -1, entry_path))
            CANCEL();
        if (strcmp(entry_path, "") == 0)
            archive_has_root = true;

        const char *hardlink = archive_entry_hardlink(entry);
        if (hardlink) {
            char hardlink_path[MAX_PATH];
            if (!path_normalize(hardlink, hardlink_path)) {
                fprintf(stderr, "warning: almost pwned by hardlink %s\n", hardlink);
                continue;
            }
            if (!strip_import_prefix(strip_prefix, hardlink_path)) {
                FILL_ERR(ERR_POSIX, EINVAL, "archive hardlink outside its expected top-level directory");
            }
            if (linkat(root_fd, fix_path(hardlink_path), root_fd, fix_path(entry_path), 0) < 0)
                POSIX_ERR();
            sqlite3_bind_blob64(insert_hardlink, 1, entry_path, strlen(entry_path), SQLITE_TRANSIENT);
            sqlite3_bind_blob64(insert_hardlink, 2, hardlink_path, strlen(hardlink_path), SQLITE_TRANSIENT);
            STEP_RESET(insert_hardlink);
            continue;
        }

        // mkdir -p
        char *entry_path_copy = strdup(entry_path);
        char *slash = entry_path_copy;
        while ((slash = strchr(*slash ? slash + 1 : slash, '/')) != NULL) {
            *slash = '\0';
            int err = mkdirat(root_fd, fix_path(entry_path_copy), 0777);
            *slash = '/';
            if (err < 0) {
                if (errno == EEXIST) continue;
                POSIX_ERR();
            }
        }
        free(entry_path_copy);

        int fd = -1;
        // first, create node
        switch (archive_entry_filetype(entry)) {
            case AE_IFREG:
            case AE_IFLNK:
            case AE_IFBLK:
            case AE_IFCHR:
            case AE_IFSOCK:
                fd = openat(root_fd, fix_path(entry_path), O_WRONLY | O_CREAT | O_TRUNC, 0666);
                resources->entry_fd = fd;
                if (fd < 0) {
                    if (errno == EISDIR) continue; // assuming it's case insensitivity
                    POSIX_ERR();
                }
                break;

            case AE_IFDIR:
                err = mkdirat(root_fd, fix_path(entry_path), 0777);
                if (err < 0 && errno != EEXIST)
                    POSIX_ERR();
                break;

            case AE_IFIFO:
                lock_fchdir(root_fd);
                err = mkfifo(fix_path(entry_path), 0666);
                unlock_fchdir();
                break;
        }
        // second, fill in contents, if needed
        switch (archive_entry_filetype(entry)) {
            case AE_IFREG:
                // A duplicate archive member must not inherit an earlier
                // member's executable grants after replacing its contents.
                if (lr_guest_caps_remove(fd) < 0) {
                    bool absent = errno == ENODATA || errno == ENOTSUP || errno == EOPNOTSUPP;
#ifdef ENOATTR
                    absent |= errno == ENOATTR;
#endif
                    if (!absent) POSIX_ERR();
                }
                if (archive_read_data_into_fd(archive, fd) != ARCHIVE_OK)
                    ARCHIVE_ERR(archive);
                break;

            case AE_IFLNK:
                err = (int) write(fd, archive_entry_symlink(entry), strlen(archive_entry_symlink(entry)));
                if (err < 0)
                    POSIX_ERR();
                break;

            case AE_IFDIR:
            case AE_IFBLK:
            case AE_IFCHR:
            case AE_IFSOCK:
            case AE_IFIFO:
                break;
        }
        // Preserve guest file capabilities, never host security.capability.
        // Unknown host/SELinux attributes retain the existing unsupported policy.
        const char *xname; const void *xvalue; size_t xsize;
        archive_entry_xattr_reset(entry);
        while (archive_entry_xattr_next(entry, &xname, &xvalue, &xsize) == ARCHIVE_OK) {
            if (strcmp(xname, "security.capability")) continue;
            int validation = lr_guest_caps_validate(xvalue, xsize);
            if (fd < 0 || archive_entry_filetype(entry) != AE_IFREG || validation) {
                errno = validation == -2 ? EOPNOTSUPP : EINVAL; POSIX_ERR();
            }
            if (lr_guest_caps_set(fd, xvalue, xsize, 0) < 0) POSIX_ERR();
        }
        if (fd != -1)
            close(fd);
        resources->entry_fd = -1;

        struct timespec times[2] = {
            // for utimes, atime is first, mtime is second
            {.tv_sec = archive_entry_atime(entry), .tv_nsec = archive_entry_atime_nsec(entry)},
            {.tv_sec = archive_entry_mtime(entry), .tv_nsec = archive_entry_mtime_nsec(entry)},
            // utimes cannot set ctime
        };
        if (!archive_entry_atime_is_set(entry))
            times[0].tv_nsec = UTIME_OMIT;
        if (!archive_entry_mtime_is_set(entry))
            times[1].tv_nsec = UTIME_OMIT;
        err = utimensat(root_fd, fix_path(entry_path), times, 0);
        if (err < 0)
            POSIX_ERR();

        struct ish_stat stat = {
            .mode = (uint32_t) archive_entry_mode(entry),
            .uid = (uint32_t) archive_entry_uid(entry),
            .gid = (uint32_t) archive_entry_gid(entry),
            .rdev = (uint32_t) archive_entry_rdev(entry),
        };
        sqlite3_bind_blob64(insert_stat, 1, &stat, sizeof(stat), SQLITE_TRANSIENT);
        STEP_RESET(insert_stat);
        sqlite3_bind_blob64(insert_path, 1, entry_path, strlen(entry_path), SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert_path, 2, sqlite3_last_insert_rowid(db));
        STEP_RESET(insert_path);
    }
    if (err != ARCHIVE_EOF)
        ARCHIVE_ERR(archive);

    // Add a path entry for the root if it's missing
    if (!archive_has_root) {
        struct ish_stat stat = {.mode = S_IFDIR | 0755};
        sqlite3_bind_blob64(insert_stat, 1, &stat, sizeof(stat), SQLITE_TRANSIENT);
        STEP_RESET(insert_stat);
        sqlite3_bind_blob64(insert_path, 1, "", 0, SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert_path, 2, sqlite3_last_insert_rowid(db));
        STEP_RESET(insert_path);
    }

    FINALIZE(insert_stat);
    FINALIZE(insert_path);
    FINALIZE(insert_hardlink);
    EXEC("commit");
    if (archive_read_close(archive) != ARCHIVE_OK)
        ARCHIVE_ERR(archive);
    return true;
}

static bool fakefs_import_source(const char *archive_path, int archive_fd, const char *fs, const char *strip_prefix, struct fakefsify_error *err_out, struct progress p) {
    struct import_resources resources = {.root_fd = -1, .entry_fd = -1};
    bool result = fakefs_import_contents(archive_path, archive_fd, fs, strip_prefix, err_out, p, &resources);
    if (resources.entry_fd >= 0) close(resources.entry_fd);
    if (resources.archive) archive_read_free(resources.archive);
    if (resources.db) {
        sqlite3_stmt *statement;
        while ((statement = sqlite3_next_stmt(resources.db, NULL))) sqlite3_finalize(statement);
        sqlite3_close(resources.db);
    }
    if (resources.root_fd >= 0) close(resources.root_fd);
    return result;
}

bool fakefs_import_prefix(const char *archive_path, const char *fs, const char *strip_prefix, struct fakefsify_error *err_out, struct progress p) {
    return fakefs_import_source(archive_path, -1, fs, strip_prefix, err_out, p);
}

bool fakefs_import_fd(int archive_fd, const char *fs, struct fakefsify_error *err_out, struct progress p) {
    return fakefs_import_source(NULL, archive_fd, fs, NULL, err_out, p);
}

bool fakefs_import(const char *archive_path, const char *fs, struct fakefsify_error *err_out, struct progress p) {
    return fakefs_import_prefix(archive_path, fs, NULL, err_out, p);
}


bool fakefs_export(const char *fs, const char *archive_path, struct fakefsify_error *err_out, struct progress p) {
    // open the archive
    struct archive *archive = archive_write_new();
    if (archive == NULL)
        ARCHIVE_ERR(archive);
    archive_write_add_filter_gzip(archive);
    archive_write_set_format_pax(archive);
    if (archive_write_open_filename(archive, archive_path) != ARCHIVE_OK)
        ARCHIVE_ERR(archive);

    // open the data root dir
    char path_tmp[PATH_MAX];
    snprintf(path_tmp, sizeof(path_tmp), "%s/data", fs);
    int root_fd = open(path_tmp, O_RDONLY);
    if (root_fd < 0)
        POSIX_ERR();

    // open the database
    snprintf(path_tmp, sizeof(path_tmp), "%s/meta.db", fs);
    sqlite3 *db;
    int err = sqlite3_open_v2(path_tmp, &db, SQLITE_OPEN_READONLY, NULL);
    CHECK_ERR();
    EXEC("begin");

    sqlite3_stmt *count_stmt = PREPARE("select count(path) from paths");
    STEP(count_stmt);
    int64_t paths_total = sqlite3_column_int64(count_stmt, 0);
    FINALIZE(count_stmt);
    int64_t paths_done = 0;

    struct archive_entry_linkresolver *linkresolver = archive_entry_linkresolver_new();
    archive_entry_linkresolver_set_strategy(linkresolver, ARCHIVE_FORMAT_TAR_PAX_INTERCHANGE);

    sqlite3_stmt *query = PREPARE("select path, inode, stat from paths, stats using (inode)");
    while (STEP(query)) {
        struct archive_entry *entry = archive_entry_new();

        const char *path_in_db = sqlite3_column_blob(query, 0);
        size_t path_len = sqlite3_column_bytes(query, 0);
        char *path = malloc(path_len + 2);
        path[0] = '.';
        memcpy(path + 1, path_in_db, path_len);
        path[path_len + 1] = '\0';
        archive_entry_set_pathname(entry, path);

        if (!progress_update(&p, (double) paths_done / paths_total, path))
            CANCEL();

        archive_entry_set_ino64(entry, sqlite3_column_int64(query, 1));
        struct ish_stat stat = *(struct ish_stat *) sqlite3_column_blob(query, 2);
        archive_entry_set_mode(entry, stat.mode);
        archive_entry_set_uid(entry, stat.uid);
        archive_entry_set_gid(entry, stat.gid);
        archive_entry_set_rdev(entry, stat.rdev);

        struct stat real_stat;
        if (fstatat(root_fd, path, &real_stat, 0) < 0) {
            if (errno == ENOENT) {
                printf("skipping %s\n", path);
                goto skip;
            }
            POSIX_ERR();
        }
        archive_entry_set_size(entry, real_stat.st_size);
#if __APPLE__
#define TIMESPEC(x) st_##x##timespec
#elif __linux__
#define TIMESPEC(x) st_##x##tim
#endif
        archive_entry_set_atime(entry, real_stat.st_atime, real_stat.TIMESPEC(a).tv_nsec);
        archive_entry_set_mtime(entry, real_stat.st_mtime, real_stat.TIMESPEC(m).tv_nsec);
        archive_entry_set_ctime(entry, real_stat.st_ctime, real_stat.TIMESPEC(c).tv_nsec);

        int fd = -1;
        S_IFMT;
        if (S_ISREG(stat.mode) || S_ISLNK(stat.mode))
            fd = openat(root_fd, path, O_RDONLY);
        if (S_ISLNK(stat.mode)) {
            char buf[MAX_PATH+1];
            ssize_t len = read(fd, buf, sizeof(buf)-1);
            if (len < 0)
                POSIX_ERR();
            buf[len] = '\0';
            archive_entry_set_symlink(entry, buf);
        }

        if (S_ISREG(stat.mode) && fd >= 0) {
            uint8_t caps[LR_GUEST_CAPS_MAX];
            ssize_t length = lr_guest_caps_get(fd, caps, sizeof caps);
            if (length >= 0) {
                if (lr_guest_caps_validate(caps, length)) { errno = EINVAL; POSIX_ERR(); }
                archive_entry_xattr_add_entry(entry, "security.capability", caps, length);
            } else {
                bool absent = errno == ENODATA || errno == ENOTSUP || errno == EOPNOTSUPP;
#ifdef ENOATTR
                absent |= errno == ENOATTR;
#endif
                if (!absent) POSIX_ERR();
            }
        }

        struct archive_entry *sparse;
        archive_entry_linkify(linkresolver, &entry, &sparse);
        if (entry != NULL)
            archive_write_header(archive, entry);
        if (sparse != NULL)
            archive_write_header(archive, sparse);

        if (S_ISREG(stat.mode) && archive_entry_size(entry) != 0) {
            char buf[8192];
            ssize_t len;
            while ((len = read(fd, buf, sizeof(buf))) > 0) {
                ssize_t written = archive_write_data(archive, buf, len);
                if (written < 0)
                    ARCHIVE_ERR(archive);
                if (written != len)
                    printf("uh oh\n");
            }
            if (len < 0)
                POSIX_ERR();
        }
        if (fd != -1)
            close(fd);

    skip:
        paths_done++;
        free(path);
        archive_entry_free(entry);
    }

    FINALIZE(query);
    archive_entry_linkresolver_free(linkresolver);
    sqlite3_close(db);
    close(root_fd);
    if (archive_write_close(archive) != ARCHIVE_OK)
        ARCHIVE_ERR(archive);
    archive_write_free(archive);
    return true;
}
