// Native reader for Valve's SteamFrame recovery image. See steamos_image.h.
//
// Btrfs support is deliberately narrow and read-only: one device, single or
// DUP block-group profiles, crc32c checksums, inline/regular/prealloc file
// extents compressed with zstd or zlib (or not at all). Every metadata block
// is checksummed before use; anything outside that scope is an error rather
// than a partial result.

#include "steamos_image.h"

#include <archive.h>
#include <archive_entry.h>
#include <bzlib.h>
#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#include "../deps/zstd/lib/zstd.h"

#define FAIL(...) do { snprintf(error, error_size, __VA_ARGS__); goto fail; } while (0)

static uint16_t le16(const uint8_t *p) { return (uint16_t) (p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return (uint64_t) le32(p) | (uint64_t) le32(p + 4) << 32; }

static bool read_exact(int fd, void *buffer, size_t size, uint64_t offset) {
    uint8_t *out = buffer;
    while (size) {
        ssize_t got = pread(fd, out, size, (off_t) offset);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { if (got == 0) errno = EIO; return false; }
        out += got; size -= (size_t) got; offset += (uint64_t) got;
    }
    return true;
}

static bool write_all(int fd, const void *buffer, size_t size) {
    const uint8_t *in = buffer;
    while (size) {
        ssize_t put = write(fd, in, size);
        if (put < 0 && errno == EINTR) continue;
        if (put <= 0) return false;
        in += put; size -= (size_t) put;
    }
    return true;
}

// MARK: SHA-256

int lr_steamos_sha256_file(const char *path, char hex[65], lr_steamos_progress_fn progress, void *context,
                           char *error, size_t error_size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    uint8_t *buffer = malloc(4 << 20);
    struct stat st;
    if (fd < 0 || !buffer || fstat(fd, &st) < 0) { snprintf(error, error_size, "Cannot read %s: %s", path, strerror(errno)); free(buffer); if (fd >= 0) close(fd); return -1; }
    CC_SHA256_CTX sha; CC_SHA256_Init(&sha);
    uint64_t done = 0;
    for (;;) {
        ssize_t got = read(fd, buffer, 4 << 20);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) { snprintf(error, error_size, "Read failed: %s", strerror(errno)); free(buffer); close(fd); return -1; }
        if (got == 0) break;
        CC_SHA256_Update(&sha, buffer, (CC_LONG) got);
        done += (uint64_t) got;
        if (progress && !progress(context, st.st_size ? (double) done / (double) st.st_size : 0, NULL)) {
            snprintf(error, error_size, "Cancelled"); free(buffer); close(fd); errno = ECANCELED; return -1;
        }
    }
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &sha);
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    free(buffer); close(fd);
    return 0;
}

// MARK: GPT partition extraction

#define HEAD_BYTES (4u << 20)

struct gpt_target { uint64_t offset, size; };

static bool find_partition(const uint8_t *head, size_t head_size, const char *name, struct gpt_target *target,
                           char *error, size_t error_size) {
    static const unsigned sectors[] = {512, 4096};
    for (unsigned s = 0; s < 2; s++) {
        unsigned sector = sectors[s];
        if (head_size < sector * 2u) continue;
        const uint8_t *header = head + sector;
        if (memcmp(header, "EFI PART", 8)) continue;
        uint32_t header_size = le32(header + 12), checksum = le32(header + 16);
        if (header_size < 92 || header_size > sector) { snprintf(error, error_size, "Invalid GPT header size"); return false; }
        uint8_t check[4096];
        memcpy(check, header, header_size); memset(check + 16, 0, 4);
        if ((uint32_t) crc32(0, check, header_size) != checksum) { snprintf(error, error_size, "GPT header checksum mismatch"); return false; }
        uint64_t table_lba = le64(header + 72);
        uint32_t count = le32(header + 80), entry_size = le32(header + 84), table_checksum = le32(header + 88);
        uint64_t table_size = (uint64_t) count * entry_size;
        if (!count || entry_size < 128 || entry_size % 128 || table_size > (1u << 20) ||
            table_lba * sector + table_size > head_size) { snprintf(error, error_size, "Invalid GPT entry table"); return false; }
        const uint8_t *table = head + table_lba * sector;
        if ((uint32_t) crc32(0, table, (uInt) table_size) != table_checksum) { snprintf(error, error_size, "GPT entry table checksum mismatch"); return false; }
        size_t wanted = strlen(name);
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *entry = table + (size_t) i * entry_size;
            char label[37] = {0};
            bool ascii = true;
            for (int c = 0; c < 36; c++) {
                uint16_t unit = le16(entry + 56 + c * 2);
                if (!unit) break;
                if (unit > 0x7e) { ascii = false; break; }
                label[c] = (char) unit;
            }
            if (!ascii || strlen(label) != wanted || memcmp(label, name, wanted)) continue;
            uint64_t first = le64(entry + 32), last = le64(entry + 40);
            if (first > last || last >= UINT64_MAX / sector) { snprintf(error, error_size, "Invalid partition bounds"); return false; }
            target->offset = first * sector;
            target->size = (last - first + 1) * sector;
            return true;
        }
        snprintf(error, error_size, "Partition %s not found in the image", name);
        return false;
    }
    snprintf(error, error_size, "No GPT partition table found");
    return false;
}

int lr_steamos_extract_partition_fd(const char *image_path, const char *name, int out,
                                    lr_steamos_progress_fn progress, void *context, char *error, size_t error_size) {
    int in = -1, result = -1;
    bool bz_open = false, compressed = false, fresh_stream = false;
    bz_stream bz; memset(&bz, 0, sizeof(bz));
    uint8_t *head = malloc(HEAD_BYTES), *input = malloc(1 << 20), *output = malloc(4 << 20);
    struct gpt_target target = {0, 0};
    bool located = false;
    uint64_t produced = 0; // decompressed image bytes seen so far
    size_t head_used = 0;
    if (!head || !input || !output) FAIL("Out of memory");
    in = open(image_path, O_RDONLY | O_CLOEXEC);
    if (in < 0) FAIL("Cannot open the SteamOS image: %s", strerror(errno));
    uint8_t magic[3] = {0};
    if (read_exact(in, magic, 3, 0)) compressed = !memcmp(magic, "BZh", 3);

    uint64_t in_offset = 0;
    bool input_done = false;
    if (compressed) {
        if (BZ2_bzDecompressInit(&bz, 0, 0) != BZ_OK) FAIL("bzip2 initialisation failed");
        bz_open = true;
    }
    for (;;) {
        size_t chunk = 0;
        if (compressed) {
            if (bz.avail_in == 0 && !input_done) {
                ssize_t got = pread(in, input, 1 << 20, (off_t) in_offset);
                if (got < 0 && errno == EINTR) continue;
                if (got < 0) FAIL("Read failed: %s", strerror(errno));
                if (got == 0) input_done = true;
                in_offset += (uint64_t) got;
                bz.next_in = (char *) input; bz.avail_in = (unsigned) got;
            }
            if (input_done && bz.avail_in == 0 && !bz_open) break;
            bz.next_out = (char *) output; bz.avail_out = 4 << 20;
            int status = bz_open ? BZ2_bzDecompress(&bz) : BZ_STREAM_END;
            if (status != BZ_OK && status != BZ_STREAM_END) FAIL("The SteamOS image is not a valid bzip2 file (%d)", status);
            chunk = (4u << 20) - bz.avail_out;
            if (bz_open && (bz.total_in_lo32 || bz.total_in_hi32)) fresh_stream = false;
            if (status == BZ_STREAM_END) {
                // Parallel bzip2 tools write several concatenated streams.
                BZ2_bzDecompressEnd(&bz); bz_open = false;
                if (bz.avail_in || !input_done) {
                    char *next = bz.next_in; unsigned avail = bz.avail_in;
                    memset(&bz, 0, sizeof(bz));
                    if (BZ2_bzDecompressInit(&bz, 0, 0) != BZ_OK) FAIL("bzip2 initialisation failed");
                    bz_open = true; fresh_stream = true; bz.next_in = next; bz.avail_in = avail;
                }
            }
            if (!chunk && input_done && bz.avail_in == 0 && !bz_open) break;
            if (!chunk && input_done && bz.avail_in == 0 && bz_open && fresh_stream) break; // no further stream
            if (!chunk && input_done && bz.avail_in == 0 && bz_open) FAIL("The SteamOS image ends unexpectedly");
        } else {
            ssize_t got = pread(in, output, 4 << 20, (off_t) in_offset);
            if (got < 0 && errno == EINTR) continue;
            if (got < 0) FAIL("Read failed: %s", strerror(errno));
            if (got == 0) break;
            in_offset += (uint64_t) got; chunk = (size_t) got;
        }
        if (!chunk) continue;
        uint64_t start = produced;
        produced += chunk;
        if (!located) {
            size_t take = chunk < HEAD_BYTES - head_used ? chunk : HEAD_BYTES - head_used;
            memcpy(head + head_used, output, take); head_used += take;
            if (head_used < HEAD_BYTES && !(input_done && (!compressed || (!bz_open && !bz.avail_in)))) continue;
            if (!find_partition(head, head_used, name, &target, error, error_size)) goto fail;
            located = true;
            // Write any part of the partition already held in the head buffer.
            if (target.offset < head_used) {
                uint64_t end = head_used < target.offset + target.size ? head_used : target.offset + target.size;
                if (!write_all(out, head + target.offset, (size_t) (end - target.offset))) FAIL("Write failed: %s", strerror(errno));
            }
            // The rest of this chunk beyond the head buffer.
            uint64_t chunk_tail = start + take;
            if (take < chunk) {
                uint64_t lo = chunk_tail > target.offset ? chunk_tail : target.offset;
                uint64_t hi = produced < target.offset + target.size ? produced : target.offset + target.size;
                if (hi > lo && !write_all(out, output + (lo - start), (size_t) (hi - lo))) FAIL("Write failed: %s", strerror(errno));
            }
        } else {
            uint64_t lo = start > target.offset ? start : target.offset;
            uint64_t hi = produced < target.offset + target.size ? produced : target.offset + target.size;
            if (hi > lo && !write_all(out, output + (lo - start), (size_t) (hi - lo))) FAIL("Write failed: %s", strerror(errno));
        }
        if (located && produced >= target.offset + target.size) break;
        if (progress && located && !progress(context, (double) produced / (double) (target.offset + target.size), NULL)) {
            errno = ECANCELED; FAIL("Cancelled");
        }
    }
    if (!located) {
        if (!find_partition(head, head_used, name, &target, error, error_size)) goto fail;
        FAIL("The SteamOS image ends before partition %s", name);
    }
    if (produced < target.offset + target.size) FAIL("The SteamOS image ends before partition %s is complete", name);
    result = 0;
fail:
    if (bz_open) BZ2_bzDecompressEnd(&bz);
    if (in >= 0) close(in);
    free(head); free(input); free(output);
    return result;
}

int lr_steamos_extract_partition(const char *image_path, const char *name, const char *out_path,
                                 lr_steamos_progress_fn progress, void *context, char *error, size_t error_size) {
    int out = open(out_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (out < 0) { snprintf(error, error_size, "Cannot create %s: %s", out_path, strerror(errno)); return -1; }
    int result = lr_steamos_extract_partition_fd(image_path, name, out, progress, context, error, error_size);
    if (!result && fsync(out) < 0) { snprintf(error, error_size, "Write failed: %s", strerror(errno)); result = -1; }
    if (close(out) < 0 && !result) { snprintf(error, error_size, "Write failed: %s", strerror(errno)); result = -1; }
    if (result) unlink(out_path);
    return result;
}

// MARK: Btrfs

#define BTRFS_SUPER_OFFSET 0x10000
#define KEY_INODE_ITEM 1
#define KEY_XATTR_ITEM 24
#define KEY_DIR_INDEX 96
#define KEY_EXTENT_DATA 108
#define KEY_ROOT_ITEM 132
#define KEY_CHUNK_ITEM 228
#define FIRST_FREE_OBJECTID 256
#define LAST_FREE_OBJECTID ((uint64_t) -256)
#define HEADER_SIZE 101

static uint32_t crc32c_table[256];
static void crc32c_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0x82F63B78u : c >> 1;
        crc32c_table[i] = c;
    }
}
static uint32_t crc32c(const uint8_t *data, size_t size) {
    uint32_t c = 0xFFFFFFFFu;
    while (size--) c = crc32c_table[(c ^ *data++) & 0xff] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

struct chunk { uint64_t logical, length, physical; };

struct inode {
    uint32_t mode, uid, gid, nlink;
    uint64_t size;
    int64_t mtime; uint32_t mtime_ns;
    uint32_t dir_first, dir_count, ext_first, ext_count;
    uint32_t cap_offset, cap_size;
    bool present;
    char *first_path; // set once for hard-linked files
};
struct dirent_rec { uint64_t child; uint32_t name_offset; uint16_t name_size; uint8_t location_type; uint8_t type; };
struct extent {
    uint64_t file_offset, ram_bytes, disk_bytenr, disk_bytes, offset, bytes;
    uint32_t inline_offset, inline_size;
    uint8_t compression, type;
};
struct tree {
    uint64_t id;
    struct inode *inodes; size_t inode_count; // indexed by objectid - 256
    struct dirent_rec *dirs; size_t dir_count, dir_capacity;
    struct extent *extents; size_t extent_count, extent_capacity;
    uint8_t *pool; size_t pool_size, pool_capacity;
    uint64_t root_dir;
    struct tree *next;
};
struct subvolume { uint64_t id, bytenr, root_dir; uint8_t level; };

struct btrfs {
    int fd;
    uint64_t device_size;
    uint8_t fsid[16];
    uint32_t nodesize, sectorsize;
    struct chunk *chunks; size_t chunk_count, chunk_capacity;
    struct subvolume *subvolumes; size_t subvolume_count, subvolume_capacity;
    struct tree *trees;
    uint8_t *node_buffers[16];
    uint8_t *compressed, *decompressed, *data;
    ZSTD_DCtx *zstd;
    char *error; size_t error_size;
};

#define BTRFS_FAIL(fs, ...) do { snprintf((fs)->error, (fs)->error_size, __VA_ARGS__); return false; } while (0)

static bool grow(void **items, size_t *capacity, size_t needed, size_t item_size) {
    if (needed <= *capacity) return true;
    size_t next = *capacity ? *capacity : 64;
    while (next < needed) next *= 2;
    void *grown = realloc(*items, next * item_size);
    if (!grown) return false;
    *items = grown; *capacity = next;
    return true;
}

static bool add_chunk(struct btrfs *fs, uint64_t logical, const uint8_t *item, size_t size) {
    if (size < 48 + 32) BTRFS_FAIL(fs, "Truncated Btrfs chunk item");
    uint64_t length = le64(item), type = le64(item + 24);
    uint16_t stripes = le16(item + 44);
    if (!stripes || size < 48 + (size_t) stripes * 32) BTRFS_FAIL(fs, "Invalid Btrfs chunk item");
    // single, DUP, RAID1 copies on this one device: any stripe holds the data.
    if (type & (8 | 64 | 128 | 256)) BTRFS_FAIL(fs, "Striped Btrfs block groups are not supported");
    uint64_t physical = le64(item + 48 + 8);
    if (length == 0 || physical + length > fs->device_size || logical + length < logical) BTRFS_FAIL(fs, "Btrfs chunk outside the partition");
    for (size_t i = 0; i < fs->chunk_count; i++) if (fs->chunks[i].logical == logical) return true;
    if (!grow((void **) &fs->chunks, &fs->chunk_capacity, fs->chunk_count + 1, sizeof(struct chunk))) BTRFS_FAIL(fs, "Out of memory");
    fs->chunks[fs->chunk_count++] = (struct chunk) {logical, length, physical};
    return true;
}

static bool map_logical(struct btrfs *fs, uint64_t logical, uint64_t size, uint64_t *physical) {
    for (size_t i = 0; i < fs->chunk_count; i++) {
        struct chunk *c = &fs->chunks[i];
        if (logical >= c->logical && logical - c->logical < c->length) {
            if (size > c->length - (logical - c->logical)) BTRFS_FAIL(fs, "Btrfs read crosses a chunk boundary");
            *physical = c->physical + (logical - c->logical);
            return true;
        }
    }
    BTRFS_FAIL(fs, "Unmapped Btrfs address %llu", (unsigned long long) logical);
}

static bool read_logical(struct btrfs *fs, uint64_t logical, void *buffer, size_t size) {
    uint64_t physical;
    if (!map_logical(fs, logical, size, &physical)) return false;
    if (!read_exact(fs->fd, buffer, size, physical)) BTRFS_FAIL(fs, "Read failed: %s", strerror(errno));
    return true;
}

typedef bool (*leaf_fn)(struct btrfs *fs, void *context, uint64_t objectid, uint8_t type, uint64_t offset,
                        const uint8_t *data, uint32_t size);

static bool walk(struct btrfs *fs, uint64_t bytenr, int level, uint64_t owner_check, leaf_fn fn, void *context) {
    if (level < 0 || level >= 8) BTRFS_FAIL(fs, "Invalid Btrfs tree level");
    uint8_t *node = fs->node_buffers[level];
    if (!read_logical(fs, bytenr, node, fs->nodesize)) return false;
    if (le32(node) != crc32c(node + 32, fs->nodesize - 32)) BTRFS_FAIL(fs, "Btrfs metadata checksum mismatch at %llu", (unsigned long long) bytenr);
    if (le64(node + 0x30) != bytenr || memcmp(node + 0x20, fs->fsid, 16) || node[0x64] != level)
        BTRFS_FAIL(fs, "Unexpected Btrfs metadata block at %llu", (unsigned long long) bytenr);
    (void) owner_check;
    uint32_t count = le32(node + 0x60);
    if (level == 0) {
        if ((size_t) count * 25 > fs->nodesize - HEADER_SIZE) BTRFS_FAIL(fs, "Invalid Btrfs leaf");
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *item = node + HEADER_SIZE + (size_t) i * 25;
            uint32_t offset = le32(item + 17), size = le32(item + 21);
            if ((uint64_t) offset + size > fs->nodesize - HEADER_SIZE) BTRFS_FAIL(fs, "Invalid Btrfs item");
            if (!fn(fs, context, le64(item), item[8], le64(item + 9), node + HEADER_SIZE + offset, size)) return false;
        }
        return true;
    }
    if ((size_t) count * 33 > fs->nodesize - HEADER_SIZE) BTRFS_FAIL(fs, "Invalid Btrfs node");
    // Child reads reuse lower-level buffers, so copy the pointers first.
    uint64_t *children = malloc(sizeof(uint64_t) * (count ? count : 1));
    if (!children) BTRFS_FAIL(fs, "Out of memory");
    for (uint32_t i = 0; i < count; i++) children[i] = le64(node + HEADER_SIZE + (size_t) i * 33 + 17);
    for (uint32_t i = 0; i < count; i++)
        if (!walk(fs, children[i], level - 1, owner_check, fn, context)) { free(children); return false; }
    free(children);
    return true;
}

static bool chunk_leaf(struct btrfs *fs, void *context, uint64_t objectid, uint8_t type, uint64_t offset, const uint8_t *data, uint32_t size) {
    (void) context; (void) objectid;
    return type != KEY_CHUNK_ITEM || add_chunk(fs, offset, data, size);
}

static bool root_leaf(struct btrfs *fs, void *context, uint64_t objectid, uint8_t type, uint64_t offset, const uint8_t *data, uint32_t size) {
    (void) context; (void) offset;
    if (type != KEY_ROOT_ITEM || (objectid != 5 && (objectid < FIRST_FREE_OBJECTID || objectid > LAST_FREE_OBJECTID))) return true;
    if (size < 239) BTRFS_FAIL(fs, "Truncated Btrfs root item");
    for (size_t i = 0; i < fs->subvolume_count; i++) if (fs->subvolumes[i].id == objectid) return true;
    if (!grow((void **) &fs->subvolumes, &fs->subvolume_capacity, fs->subvolume_count + 1, sizeof(struct subvolume))) BTRFS_FAIL(fs, "Out of memory");
    fs->subvolumes[fs->subvolume_count++] = (struct subvolume) {objectid, le64(data + 176), le64(data + 168), data[238]};
    return true;
}

static bool pool_add(struct btrfs *fs, struct tree *t, const uint8_t *data, size_t size, uint32_t *offset) {
    if (t->pool_size + size > UINT32_MAX) BTRFS_FAIL(fs, "Btrfs filesystem too large");
    if (!grow((void **) &t->pool, &t->pool_capacity, t->pool_size + size + 1, 1)) BTRFS_FAIL(fs, "Out of memory");
    memcpy(t->pool + t->pool_size, data, size);
    *offset = (uint32_t) t->pool_size;
    t->pool_size += size;
    return true;
}

static struct inode *tree_inode(struct btrfs *fs, struct tree *t, uint64_t objectid) {
    if (objectid < FIRST_FREE_OBJECTID || objectid - FIRST_FREE_OBJECTID > (64u << 20)) {
        snprintf(fs->error, fs->error_size, "Unsupported Btrfs inode number"); return NULL;
    }
    size_t index = (size_t) (objectid - FIRST_FREE_OBJECTID);
    if (index >= t->inode_count) {
        size_t next = t->inode_count ? t->inode_count : 1024;
        while (next <= index) next *= 2;
        struct inode *grown = realloc(t->inodes, next * sizeof(struct inode));
        if (!grown) { snprintf(fs->error, fs->error_size, "Out of memory"); return NULL; }
        memset(grown + t->inode_count, 0, (next - t->inode_count) * sizeof(struct inode));
        t->inodes = grown; t->inode_count = next;
    }
    return &t->inodes[index];
}

static bool fs_leaf(struct btrfs *fs, void *context, uint64_t objectid, uint8_t type, uint64_t offset, const uint8_t *data, uint32_t size) {
    struct tree *t = context;
    if (objectid < FIRST_FREE_OBJECTID || objectid > LAST_FREE_OBJECTID) return true; // orphan and free-inode bookkeeping
    if (type != KEY_INODE_ITEM && type != KEY_XATTR_ITEM && type != KEY_DIR_INDEX && type != KEY_EXTENT_DATA) return true;
    struct inode *ino = tree_inode(fs, t, objectid);
    if (!ino) return false;
    switch (type) {
        case KEY_INODE_ITEM:
            if (size < 160) BTRFS_FAIL(fs, "Truncated Btrfs inode");
            ino->present = true;
            ino->size = le64(data + 16); ino->nlink = le32(data + 40);
            ino->uid = le32(data + 44); ino->gid = le32(data + 48); ino->mode = le32(data + 52);
            ino->mtime = (int64_t) le64(data + 136); ino->mtime_ns = le32(data + 144);
            break;
        case KEY_DIR_INDEX: {
            if (size < 30) BTRFS_FAIL(fs, "Truncated Btrfs directory entry");
            uint16_t data_size = le16(data + 25), name_size = le16(data + 27);
            if (30u + data_size + name_size != size || !name_size) BTRFS_FAIL(fs, "Invalid Btrfs directory entry");
            if (!grow((void **) &t->dirs, &t->dir_capacity, t->dir_count + 1, sizeof(struct dirent_rec))) BTRFS_FAIL(fs, "Out of memory");
            if (!ino->dir_count) ino->dir_first = (uint32_t) t->dir_count;
            else if (ino->dir_first + ino->dir_count != t->dir_count) BTRFS_FAIL(fs, "Unordered Btrfs directory index");
            struct dirent_rec *d = &t->dirs[t->dir_count];
            d->child = le64(data); d->location_type = data[8]; d->type = data[29]; d->name_size = name_size;
            if (!pool_add(fs, t, data + 30, name_size, &d->name_offset)) return false;
            t->dir_count++; ino->dir_count++;
            break;
        }
        case KEY_XATTR_ITEM: {
            if (size < 30) BTRFS_FAIL(fs, "Truncated Btrfs xattr");
            // Several attributes can share one item when their names collide in the hash.
            const uint8_t *p = data, *end = data + size;
            while (end - p >= 30) {
                uint16_t data_size = le16(p + 25), name_size = le16(p + 27);
                if ((size_t) (end - p) < 30u + name_size + data_size) BTRFS_FAIL(fs, "Invalid Btrfs xattr");
                if (name_size == 19 && !memcmp(p + 30, "security.capability", 19)) {
                    if (!pool_add(fs, t, p + 30 + name_size, data_size, &ino->cap_offset)) return false;
                    ino->cap_size = data_size;
                }
                p += 30u + name_size + data_size;
            }
            break;
        }
        case KEY_EXTENT_DATA: {
            if (size < 21) BTRFS_FAIL(fs, "Truncated Btrfs extent");
            if (!grow((void **) &t->extents, &t->extent_capacity, t->extent_count + 1, sizeof(struct extent))) BTRFS_FAIL(fs, "Out of memory");
            if (!ino->ext_count) ino->ext_first = (uint32_t) t->extent_count;
            else if (ino->ext_first + ino->ext_count != t->extent_count) BTRFS_FAIL(fs, "Unordered Btrfs extents");
            struct extent *e = &t->extents[t->extent_count];
            memset(e, 0, sizeof(*e));
            e->file_offset = offset; e->ram_bytes = le64(data + 8); e->compression = data[16]; e->type = data[20];
            if (data[17] || le16(data + 18)) BTRFS_FAIL(fs, "Encrypted or encoded Btrfs extents are not supported");
            if (e->type == 0) {
                if (!pool_add(fs, t, data + 21, size - 21, &e->inline_offset)) return false;
                e->inline_size = size - 21;
            } else if (e->type == 1 || e->type == 2) {
                if (size < 53) BTRFS_FAIL(fs, "Truncated Btrfs extent");
                e->disk_bytenr = le64(data + 21); e->disk_bytes = le64(data + 29);
                e->offset = le64(data + 37); e->bytes = le64(data + 45);
            } else BTRFS_FAIL(fs, "Unknown Btrfs extent type");
            if (e->compression > 3 || e->compression == 2) BTRFS_FAIL(fs, "Btrfs LZO compression is not supported");
            t->extent_count++; ino->ext_count++;
            break;
        }
    }
    return true;
}

static struct tree *load_tree(struct btrfs *fs, uint64_t id) {
    for (struct tree *t = fs->trees; t; t = t->next) if (t->id == id) return t;
    struct subvolume *sub = NULL;
    for (size_t i = 0; i < fs->subvolume_count; i++) if (fs->subvolumes[i].id == id) sub = &fs->subvolumes[i];
    if (!sub) { snprintf(fs->error, fs->error_size, "Btrfs subvolume %llu not found", (unsigned long long) id); return NULL; }
    struct tree *t = calloc(1, sizeof(*t));
    if (!t) { snprintf(fs->error, fs->error_size, "Out of memory"); return NULL; }
    t->id = id; t->root_dir = sub->root_dir;
    t->next = fs->trees; fs->trees = t;
    if (!walk(fs, sub->bytenr, sub->level, id, fs_leaf, t)) return NULL;
    return t;
}

static bool open_btrfs(struct btrfs *fs, const char *path) {
    crc32c_init();
    fs->fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fs->fd < 0 || fstat(fs->fd, &st) < 0) BTRFS_FAIL(fs, "Cannot open %s: %s", path, strerror(errno));
    fs->device_size = (uint64_t) st.st_size;
    uint8_t super[4096];
    if (!read_exact(fs->fd, super, sizeof(super), BTRFS_SUPER_OFFSET)) BTRFS_FAIL(fs, "Cannot read the Btrfs superblock");
    if (memcmp(super + 0x40, "_BHRfS_M", 8)) BTRFS_FAIL(fs, "The partition is not a Btrfs filesystem");
    if (le16(super + 0xc4) != 0) BTRFS_FAIL(fs, "Only crc32c Btrfs checksums are supported");
    if (le32(super) != crc32c(super + 32, sizeof(super) - 32)) BTRFS_FAIL(fs, "Btrfs superblock checksum mismatch");
    uint64_t incompat = le64(super + 0xbc);
    if (incompat & ~(uint64_t) 0x77f) BTRFS_FAIL(fs, "Unsupported Btrfs features 0x%llx", (unsigned long long) incompat);
    if (le64(super + 0x88) != 1) BTRFS_FAIL(fs, "Multi-device Btrfs filesystems are not supported");
    memcpy(fs->fsid, incompat & 0x400 ? super + 0x23b : super + 0x20, 16);
    fs->sectorsize = le32(super + 0x90); fs->nodesize = le32(super + 0x94);
    if (fs->nodesize < 4096 || fs->nodesize > 65536 || fs->sectorsize < 4096 || fs->sectorsize > 65536)
        BTRFS_FAIL(fs, "Unsupported Btrfs block sizes");
    for (int i = 0; i < 16; i++) if (!(fs->node_buffers[i] = malloc(fs->nodesize))) BTRFS_FAIL(fs, "Out of memory");
    uint32_t array_size = le32(super + 0xa0);
    if (array_size > 2048) BTRFS_FAIL(fs, "Invalid Btrfs system chunk array");
    const uint8_t *p = super + 0x32b, *end = p + array_size;
    while (p < end) {
        if (end - p < 17 + 48) BTRFS_FAIL(fs, "Invalid Btrfs system chunk array");
        uint64_t logical = le64(p + 9);
        uint16_t stripes = le16(p + 17 + 44);
        size_t item = 48 + (size_t) stripes * 32;
        if ((size_t) (end - p) < 17 + item || p[8] != KEY_CHUNK_ITEM) BTRFS_FAIL(fs, "Invalid Btrfs system chunk array");
        if (!add_chunk(fs, logical, p + 17, item)) return false;
        p += 17 + item;
    }
    if (!walk(fs, le64(super + 0x58), super[0xc7], 3, chunk_leaf, NULL)) return false;
    if (!walk(fs, le64(super + 0x50), super[0xc6], 1, root_leaf, NULL)) return false;
    fs->compressed = malloc(1 << 20); fs->decompressed = malloc(1 << 20); fs->data = malloc(1 << 20);
    fs->zstd = ZSTD_createDCtx();
    if (!fs->compressed || !fs->decompressed || !fs->data || !fs->zstd) BTRFS_FAIL(fs, "Out of memory");
    return true;
}

static void close_btrfs(struct btrfs *fs) {
    while (fs->trees) {
        struct tree *t = fs->trees; fs->trees = t->next;
        for (size_t i = 0; i < t->inode_count; i++) free(t->inodes[i].first_path);
        free(t->inodes); free(t->dirs); free(t->extents); free(t->pool); free(t);
    }
    for (int i = 0; i < 16; i++) free(fs->node_buffers[i]);
    free(fs->chunks); free(fs->subvolumes); free(fs->compressed); free(fs->decompressed); free(fs->data);
    if (fs->zstd) ZSTD_freeDCtx(fs->zstd);
    if (fs->fd >= 0) close(fs->fd);
}

// Decompresses one Btrfs compressed extent (at most 128 KiB of output).
static bool decompress(struct btrfs *fs, uint8_t method, const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size) {
    if (method == 3) {
        ZSTD_DCtx_reset(fs->zstd, ZSTD_reset_session_only);
        ZSTD_inBuffer input = {in, in_size, 0};
        ZSTD_outBuffer output = {out, out_size, 0};
        while (output.pos < output.size) {
            size_t status = ZSTD_decompressStream(fs->zstd, &output, &input);
            if (ZSTD_isError(status)) BTRFS_FAIL(fs, "Corrupt zstd data in the SteamOS image");
            if (status == 0) break; // frame complete; trailing bytes are sector padding
            if (input.pos == input.size && output.pos < output.size) break;
        }
        if (output.pos < output.size) memset(out + output.pos, 0, output.size - output.pos);
        return true;
    }
    if (method == 1) {
        z_stream z; memset(&z, 0, sizeof(z));
        if (inflateInit(&z) != Z_OK) BTRFS_FAIL(fs, "zlib initialisation failed");
        z.next_in = (Bytef *) in; z.avail_in = (uInt) in_size;
        z.next_out = out; z.avail_out = (uInt) out_size;
        int status = inflate(&z, Z_FINISH);
        size_t produced = out_size - z.avail_out;
        inflateEnd(&z);
        if (status != Z_STREAM_END && status != Z_BUF_ERROR && status != Z_OK) BTRFS_FAIL(fs, "Corrupt zlib data in the SteamOS image");
        if (produced < out_size) memset(out + produced, 0, out_size - produced);
        return true;
    }
    BTRFS_FAIL(fs, "Unsupported Btrfs compression");
}

typedef bool (*data_fn)(void *context, const uint8_t *data, size_t size);

static bool emit_zeros(struct btrfs *fs, uint64_t size, data_fn fn, void *context) {
    memset(fs->data, 0, 1 << 20);
    while (size) {
        size_t step = size < (1u << 20) ? (size_t) size : (1u << 20);
        if (!fn(context, fs->data, step)) return false;
        size -= step;
    }
    return true;
}

// Streams a file's contents (exactly inode size bytes) to fn.
static bool read_file(struct btrfs *fs, struct tree *t, struct inode *ino, data_fn fn, void *context) {
    uint64_t position = 0, size = ino->size;
    for (uint32_t i = 0; i < ino->ext_count && position < size; i++) {
        struct extent *e = &t->extents[ino->ext_first + i];
        if (e->file_offset < position) BTRFS_FAIL(fs, "Overlapping Btrfs extents");
        if (e->file_offset >= size) break;
        if (e->file_offset > position) { if (!emit_zeros(fs, e->file_offset - position, fn, context)) return false; position = e->file_offset; }
        uint64_t remaining = size - position;
        if (e->type == 0) {
            const uint8_t *in = t->pool + e->inline_offset;
            size_t length;
            if (e->compression) {
                if (e->ram_bytes > (1u << 20)) BTRFS_FAIL(fs, "Invalid inline Btrfs extent");
                if (!decompress(fs, e->compression, in, e->inline_size, fs->decompressed, (size_t) e->ram_bytes)) return false;
                in = fs->decompressed; length = (size_t) e->ram_bytes;
            } else length = e->inline_size;
            if (length > remaining) length = (size_t) remaining;
            if (!fn(context, in, length)) return false;
            position += length;
            continue;
        }
        uint64_t length = e->bytes < remaining ? e->bytes : remaining;
        if (e->type == 2 || e->disk_bytenr == 0) {
            if (!emit_zeros(fs, length, fn, context)) return false;
        } else if (e->compression) {
            if (e->disk_bytes > (1u << 20) || e->ram_bytes > (1u << 20) || e->offset > e->ram_bytes || length > e->ram_bytes - e->offset)
                BTRFS_FAIL(fs, "Invalid compressed Btrfs extent");
            if (!read_logical(fs, e->disk_bytenr, fs->compressed, (size_t) e->disk_bytes)) return false;
            if (!decompress(fs, e->compression, fs->compressed, (size_t) e->disk_bytes, fs->decompressed, (size_t) e->ram_bytes)) return false;
            if (!fn(context, fs->decompressed + e->offset, (size_t) length)) return false;
        } else {
            uint64_t done = 0;
            while (done < length) {
                size_t step = length - done < (1u << 20) ? (size_t) (length - done) : (1u << 20);
                if (!read_logical(fs, e->disk_bytenr + e->offset + done, fs->data, step)) return false;
                if (!fn(context, fs->data, step)) return false;
                done += step;
            }
        }
        position += length;
    }
    if (position < size && !emit_zeros(fs, size - position, fn, context)) return false;
    return true;
}

// MARK: Export

static const char *const excluded[] = {"boot", "efi", "esp", "persist", "usr/lib/modules", "usr/lib/firmware", "usr/lib/debug", NULL};
static const char *const empty_directories[] = {"dev", "proc", "sys", "run", "tmp", NULL};
static bool listed(const char *const *list, const char *path) {
    for (; *list; list++) if (!strcmp(*list, path)) return true;
    return false;
}

struct export {
    struct btrfs *fs;
    struct archive *archive;
    bool counting;
    uint64_t total_bytes, written_bytes, entries;
    lr_steamos_progress_fn progress; void *context;
    char path[4096];
    double last_fraction;
};

static bool write_data(void *context, const uint8_t *data, size_t size) {
    struct export *x = context;
    while (size) {
        la_ssize_t put = archive_write_data(x->archive, data, size);
        if (put <= 0) { snprintf(x->fs->error, x->fs->error_size, "Archive write failed: %s", archive_error_string(x->archive) ?: "unknown"); return false; }
        data += put; size -= (size_t) put; x->written_bytes += (uint64_t) put;
    }
    double fraction = x->total_bytes ? (double) x->written_bytes / (double) x->total_bytes : 0;
    if (x->progress && fraction - x->last_fraction >= 0.001) {
        x->last_fraction = fraction;
        if (!x->progress(x->context, fraction > 1 ? 1 : fraction, NULL)) { snprintf(x->fs->error, x->fs->error_size, "Cancelled"); errno = ECANCELED; return false; }
    }
    return true;
}

struct symlink_buffer { char target[4096]; size_t size; bool overflow; };
static bool collect_symlink(void *context, const uint8_t *data, size_t size) {
    struct symlink_buffer *b = context;
    if (b->size + size >= sizeof(b->target)) { b->overflow = true; return false; }
    memcpy(b->target + b->size, data, size); b->size += size;
    return true;
}

static bool export_entry(struct export *x, struct tree *t, uint64_t objectid, int depth);

static bool export_directory(struct export *x, struct tree *t, struct inode *dir, int depth) {
    struct btrfs *fs = x->fs;
    size_t base = strlen(x->path);
    for (uint32_t i = 0; i < dir->dir_count; i++) {
        struct dirent_rec *d = &t->dirs[dir->dir_first + i];
        const char *name = (const char *) t->pool + d->name_offset;
        if (memchr(name, '/', d->name_size) || memchr(name, 0, d->name_size) ||
            (d->name_size == 1 && name[0] == '.') || (d->name_size == 2 && name[0] == '.' && name[1] == '.'))
            BTRFS_FAIL(fs, "Invalid file name in the SteamOS image");
        if (base + (base ? 1 : 0) + d->name_size >= sizeof(x->path)) BTRFS_FAIL(fs, "Path too long in the SteamOS image");
        size_t at = base;
        if (base) x->path[at++] = '/';
        memcpy(x->path + at, name, d->name_size); x->path[at + d->name_size] = 0;
        bool ok;
        if (d->location_type == KEY_ROOT_ITEM) {
            struct tree *sub = load_tree(fs, d->child);
            ok = sub && export_entry(x, sub, sub->root_dir, depth + 1);
        } else if (d->location_type == KEY_INODE_ITEM) {
            ok = export_entry(x, t, d->child, depth + 1);
        } else BTRFS_FAIL(fs, "Invalid Btrfs directory entry");
        x->path[base] = 0;
        if (!ok) return false;
    }
    return true;
}

static bool export_entry(struct export *x, struct tree *t, uint64_t objectid, int depth) {
    struct btrfs *fs = x->fs;
    if (depth > 256) BTRFS_FAIL(fs, "Directory nesting too deep in the SteamOS image");
    if (listed(excluded, x->path)) return true;
    struct inode *ino = tree_inode(fs, t, objectid);
    if (!ino) return false;
    if (!ino->present) BTRFS_FAIL(fs, "Missing Btrfs inode for %s", x->path);
    uint32_t kind = ino->mode & S_IFMT;
    if (kind != S_IFDIR && kind != S_IFREG && kind != S_IFLNK) return true; // never materialize devices, sockets or FIFOs
    bool root = x->path[0] == 0;
    if (x->counting) {
        if (kind == S_IFREG && !(ino->nlink > 1 && ino->first_path)) {
            x->total_bytes += ino->size;
            if (ino->nlink > 1 && !(ino->first_path = strdup(x->path))) BTRFS_FAIL(fs, "Out of memory");
        }
    } else if (!root) {
        struct archive_entry *entry = archive_entry_new();
        if (!entry) BTRFS_FAIL(fs, "Out of memory");
        archive_entry_set_pathname(entry, x->path);
        archive_entry_set_mode(entry, ino->mode);
        archive_entry_set_uid(entry, ino->uid);
        archive_entry_set_gid(entry, ino->gid);
        archive_entry_set_mtime(entry, ino->mtime, ino->mtime_ns);
        bool data = false;
        if (kind == S_IFLNK) {
            struct symlink_buffer link = {.size = 0};
            if (!read_file(fs, t, ino, collect_symlink, &link) || link.overflow || !link.size) {
                archive_entry_free(entry);
                BTRFS_FAIL(fs, "Invalid symbolic link %s", x->path);
            }
            link.target[link.size] = 0;
            archive_entry_set_symlink(entry, link.target);
        } else if (kind == S_IFREG) {
            // Hard links: the counting pass recorded the first path in walk order.
            if (ino->nlink > 1 && ino->first_path && strcmp(ino->first_path, x->path)) {
                archive_entry_set_hardlink(entry, ino->first_path);
                archive_entry_set_size(entry, 0);
            } else {
                archive_entry_set_size(entry, (la_int64_t) ino->size);
                data = true;
            }
            if (ino->cap_size) archive_entry_xattr_add_entry(entry, "security.capability", t->pool + ino->cap_offset, ino->cap_size);
        }
        if (archive_write_header(x->archive, entry) != ARCHIVE_OK) {
            snprintf(fs->error, fs->error_size, "Archive write failed: %s", archive_error_string(x->archive) ?: "unknown");
            archive_entry_free(entry); return false;
        }
        archive_entry_free(entry);
        x->entries++;
        if (data && !read_file(fs, t, ino, write_data, x)) return false;
    }
    if (kind == S_IFDIR && !listed(empty_directories, x->path)) return export_directory(x, t, ino, depth);
    return true;
}

// MARK: Overlay packages

// True when the exported image already has a directory at path (no symlinks followed).
static bool image_has_directory(struct btrfs *fs, struct tree *t, const char *path) {
    struct inode *ino = tree_inode(fs, t, t->root_dir);
    while (ino && *path) {
        const char *slash = strchr(path, '/');
        size_t length = slash ? (size_t) (slash - path) : strlen(path);
        struct inode *next = NULL;
        for (uint32_t i = 0; i < ino->dir_count && !next; i++) {
            struct dirent_rec *d = &t->dirs[ino->dir_first + i];
            if (d->name_size != length || memcmp(t->pool + d->name_offset, path, length)) continue;
            if (d->location_type == KEY_ROOT_ITEM) {
                if (!(t = load_tree(fs, d->child))) return false;
                next = tree_inode(fs, t, t->root_dir);
            } else next = tree_inode(fs, t, d->child);
        }
        if (!next || !next->present || (next->mode & S_IFMT) != S_IFDIR) return false;
        ino = next;
        path = slash ? slash + 1 : path + length;
    }
    return ino != NULL;
}

struct overlay { char **directories; size_t count, capacity; time_t now; };

static bool write_header(struct export *x, struct archive_entry *entry) {
    if (archive_write_header(x->archive, entry) == ARCHIVE_OK) { x->entries++; return true; }
    snprintf(x->fs->error, x->fs->error_size, "Archive write failed: %s", archive_error_string(x->archive) ?: "unknown");
    return false;
}

static bool overlay_directory(struct export *x, struct tree *top, struct overlay *o, const char *path) {
    if (!*path || listed(excluded, path)) return true;
    for (size_t i = 0; i < o->count; i++) if (!strcmp(o->directories[i], path)) return true;
    char parent[4096];
    snprintf(parent, sizeof(parent), "%s", path);
    char *slash = strrchr(parent, '/');
    if (slash) { *slash = 0; if (!overlay_directory(x, top, o, parent)) return false; }
    if (!grow((void **) &o->directories, &o->capacity, o->count + 1, sizeof(char *)) || !(o->directories[o->count] = strdup(path)))
        BTRFS_FAIL(x->fs, "Out of memory");
    o->count++;
    if (image_has_directory(x->fs, top, path)) return true;
    if (x->fs->error[0]) return false;
    struct archive_entry *entry = archive_entry_new();
    if (!entry) BTRFS_FAIL(x->fs, "Out of memory");
    archive_entry_set_pathname(entry, path);
    archive_entry_set_mode(entry, S_IFDIR | 0755);
    archive_entry_set_mtime(entry, o->now, 0);
    bool ok = write_header(x, entry);
    archive_entry_free(entry);
    return ok;
}

static bool overlay_parents(struct export *x, struct tree *top, struct overlay *o, const char *path) {
    char parent[4096];
    snprintf(parent, sizeof(parent), "%s", path);
    char *slash = strrchr(parent, '/');
    if (!slash) return true;
    *slash = 0;
    return overlay_directory(x, top, o, parent);
}

static bool overlay_bytes(struct export *x, struct tree *top, struct overlay *o, const char *path, const char *data, size_t size, mode_t mode) {
    if (!overlay_parents(x, top, o, path)) return false;
    struct archive_entry *entry = archive_entry_new();
    if (!entry) BTRFS_FAIL(x->fs, "Out of memory");
    archive_entry_set_pathname(entry, path);
    archive_entry_set_mode(entry, S_IFREG | mode);
    archive_entry_set_size(entry, (la_int64_t) size);
    archive_entry_set_mtime(entry, o->now, 0);
    bool ok = write_header(x, entry);
    archive_entry_free(entry);
    return ok && write_data(x, (const uint8_t *) data, size);
}

struct text { char *data; size_t size, capacity; };
static bool text_add(struct text *t, const char *data, size_t size) {
    if (!grow((void **) &t->data, &t->capacity, t->size + size + 1, 1)) return false;
    memcpy(t->data + t->size, data, size); t->size += size; t->data[t->size] = 0;
    return true;
}
static bool text_line(struct text *t, const char *line) { return text_add(t, line, strlen(line)) && text_add(t, "\n", 1); }

// pacman's local database entry for one package, from its .PKGINFO.
static bool package_desc(const char *pkginfo, time_t now, struct text *out) {
    static const char *const fields[][2] = {
        {"NAME", "pkgname"}, {"VERSION", "pkgver"}, {"BASE", "pkgbase"}, {"DESC", "pkgdesc"}, {"URL", "url"},
        {"ARCH", "arch"}, {"BUILDDATE", "builddate"}, {"PACKAGER", "packager"}, {"LICENSE", "license"},
        {"GROUPS", "group"}, {"REPLACES", "replaces"}, {"DEPENDS", "depend"}, {"OPTDEPENDS", "optdepend"},
        {"CONFLICTS", "conflict"}, {"PROVIDES", "provides"},
    };
    char size[64] = "0";
    for (size_t f = 0; f < sizeof(fields) / sizeof(fields[0]); f++) {
        bool header = false;
        size_t key = strlen(fields[f][1]);
        for (const char *line = pkginfo; *line; ) {
            const char *end = strchr(line, '\n');
            size_t length = end ? (size_t) (end - line) : strlen(line);
            if (length > key + 3 && !memcmp(line, fields[f][1], key) && !memcmp(line + key, " = ", 3)) {
                if (!header) {
                    char title[32]; snprintf(title, sizeof(title), "%%%s%%", fields[f][0]);
                    if (!text_line(out, title)) return false;
                    header = true;
                }
                if (!text_add(out, line + key + 3, length - key - 3) || !text_add(out, "\n", 1)) return false;
            }
            if (!strncmp(line, "size = ", 7) && length - 7 < sizeof(size)) { memcpy(size, line + 7, length - 7); size[length - 7] = 0; }
            line = end ? end + 1 : line + length;
        }
        if (header && !text_add(out, "\n", 1)) return false;
        if (!strcmp(fields[f][0], "BUILDDATE")) {
            char installed[64]; snprintf(installed, sizeof(installed), "%%INSTALLDATE%%\n%lld\n", (long long) now);
            if (!text_line(out, installed)) return false;
        }
    }
    char tail[160];
    // Packages are pinned by SHA-256 in the app, not checked by pacman's keyring here.
    snprintf(tail, sizeof(tail), "%%SIZE%%\n%s\n\n%%REASON%%\n0\n\n%%VALIDATION%%\nsha256\n", size);
    return text_line(out, tail);
}

static bool pkginfo_value(const char *pkginfo, const char *key, char *out, size_t size) {
    size_t length = strlen(key);
    for (const char *line = pkginfo; *line; ) {
        const char *end = strchr(line, '\n');
        size_t n = end ? (size_t) (end - line) : strlen(line);
        if (n > length + 3 && !memcmp(line, key, length) && !memcmp(line + length, " = ", 3) && n - length - 3 < size) {
            memcpy(out, line + length + 3, n - length - 3); out[n - length - 3] = 0;
            return true;
        }
        line = end ? end + 1 : line + n;
    }
    return false;
}

static bool safe_member(const char *name) {
    if (!*name || name[0] == '/' || name[0] == '.') return false;
    for (const char *p = name; *p; ) {
        const char *slash = strchr(p, '/');
        size_t n = slash ? (size_t) (slash - p) : strlen(p);
        if ((n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) return false;
        p = slash ? slash + 1 : p + n;
    }
    return true;
}

static bool overlay_package(struct export *x, struct tree *top, struct overlay *o, const char *package) {
    struct btrfs *fs = x->fs;
    struct archive *in = archive_read_new();
    struct text pkginfo = {0}, files = {0}, desc = {0};
    bool ok = false;
    uint8_t *buffer = malloc(1 << 16);
    if (!in || !buffer || !text_line(&files, "%FILES%")) { snprintf(fs->error, fs->error_size, "Out of memory"); goto done; }
    archive_read_support_filter_xz(in); archive_read_support_filter_zstd(in); archive_read_support_format_tar(in);
    if (archive_read_open_filename(in, package, 1 << 16) != ARCHIVE_OK) {
        snprintf(fs->error, fs->error_size, "Cannot read package: %s", archive_error_string(in) ?: "unknown"); goto done;
    }
    struct archive_entry *member;
    int status;
    while ((status = archive_read_next_header(in, &member)) == ARCHIVE_OK) {
        char name[4096];
        snprintf(name, sizeof(name), "%s", archive_entry_pathname(member));
        size_t length = strlen(name);
        while (length && name[length - 1] == '/') name[--length] = 0;
        if (!strcmp(name, ".PKGINFO")) {
            la_ssize_t got;
            while ((got = archive_read_data(in, buffer, 1 << 16)) > 0) if (!text_add(&pkginfo, (char *) buffer, (size_t) got)) goto oom;
            if (got < 0) { snprintf(fs->error, fs->error_size, "Cannot read package metadata"); goto done; }
            continue;
        }
        if (name[0] == '.') continue;
        if (!safe_member(name)) { snprintf(fs->error, fs->error_size, "Unsafe package member"); goto done; }
        unsigned type = archive_entry_filetype(member);
        if (!text_add(&files, name, length) || !text_add(&files, type == AE_IFDIR ? "/\n" : "\n", type == AE_IFDIR ? 2 : 1)) goto oom;
        if (type == AE_IFDIR) { if (!overlay_directory(x, top, o, name)) goto done; continue; }
        if (type != AE_IFREG && type != AE_IFLNK) { snprintf(fs->error, fs->error_size, "Unsupported package member type"); goto done; }
        if (!overlay_parents(x, top, o, name)) goto done;
        struct archive_entry *entry = archive_entry_new();
        if (!entry) goto oom;
        archive_entry_set_pathname(entry, name);
        archive_entry_set_mode(entry, archive_entry_mode(member));
        archive_entry_set_uid(entry, archive_entry_uid(member));
        archive_entry_set_gid(entry, archive_entry_gid(member));
        archive_entry_set_mtime(entry, archive_entry_mtime(member), 0);
        const char *hardlink = archive_entry_hardlink(member);
        if (type == AE_IFLNK) archive_entry_set_symlink(entry, archive_entry_symlink(member));
        else if (hardlink) {
            if (!safe_member(hardlink)) { archive_entry_free(entry); snprintf(fs->error, fs->error_size, "Unsafe package link"); goto done; }
            archive_entry_set_hardlink(entry, hardlink); archive_entry_set_size(entry, 0);
        } else archive_entry_set_size(entry, archive_entry_size(member));
        bool written = write_header(x, entry);
        archive_entry_free(entry);
        if (!written) goto done;
        if (type == AE_IFREG && !hardlink) {
            la_ssize_t got;
            while ((got = archive_read_data(in, buffer, 1 << 16)) > 0) if (!write_data(x, buffer, (size_t) got)) goto done;
            if (got < 0) { snprintf(fs->error, fs->error_size, "Cannot read package data"); goto done; }
        }
    }
    if (status != ARCHIVE_EOF) { snprintf(fs->error, fs->error_size, "Invalid package: %s", archive_error_string(in) ?: "unknown"); goto done; }
    char name[256], version[256], path[700];
    if (!pkginfo.data || !pkginfo_value(pkginfo.data, "pkgname", name, sizeof(name)) || !pkginfo_value(pkginfo.data, "pkgver", version, sizeof(version)) ||
        strchr(name, '/') || strchr(version, '/')) { snprintf(fs->error, fs->error_size, "Package metadata is missing"); goto done; }
    if (!text_add(&files, "\n", 1) || !package_desc(pkginfo.data, o->now, &desc)) goto oom;
    snprintf(path, sizeof(path), "usr/lib/holo/pacmandb/local/%s-%s/desc", name, version);
    if (!overlay_bytes(x, top, o, path, desc.data, desc.size, 0644)) goto done;
    snprintf(path, sizeof(path), "usr/lib/holo/pacmandb/local/%s-%s/files", name, version);
    if (!overlay_bytes(x, top, o, path, files.data, files.size, 0644)) goto done;
    ok = true;
    goto done;
oom:
    snprintf(fs->error, fs->error_size, "Out of memory");
done:
    if (in) archive_read_free(in);
    free(buffer); free(pkginfo.data); free(files.data); free(desc.data);
    return ok;
}

// Boot command for SteamOS environments (AppDelegate starts /usr/lib/linuxrunner/init.sh).
static const char steamos_init[] =
    "#!/bin/sh\nset -eu\nmkdir -p /tmp /run /var/log /var/lib/linuxrunner /home/mobile/Downloads\nchmod 1777 /tmp\n"
    "dbus-uuidgen --ensure\ntouch /var/lib/linuxrunner/initialized\nwhile :; do sleep 3600; done\n";

static int archive_fd_open(struct archive *a, void *context) { (void) a; (void) context; return ARCHIVE_OK; }
static la_ssize_t archive_fd_write(struct archive *a, void *context, const void *buffer, size_t size) {
    int fd = *(int *) context;
    for (;;) {
        ssize_t put = write(fd, buffer, size);
        if (put < 0 && errno == EINTR) continue;
        if (put < 0) archive_set_error(a, errno, "%s", strerror(errno));
        return put;
    }
}
static int archive_fd_close(struct archive *a, void *context) { (void) a; (void) context; return ARCHIVE_OK; }

int lr_steamos_btrfs_export(const char *image_path, int out_fd, lr_steamos_progress_fn progress, void *context,
                            char *error, size_t error_size) {
    return lr_steamos_install_stream(image_path, NULL, 0, out_fd, progress, context, error, error_size);
}

int lr_steamos_install_stream(const char *image_path, const char *const *packages, size_t package_count, int out_fd,
                              lr_steamos_progress_fn progress, void *context, char *error, size_t error_size) {
    struct btrfs fs = {.fd = -1, .error = error, .error_size = error_size};
    struct archive *archive = NULL;
    int result = -1;
    error[0] = 0;
    if (!open_btrfs(&fs, image_path)) goto fail;
    struct tree *top = load_tree(&fs, 5);
    if (!top) goto fail;
    struct export x = {.fs = &fs, .counting = true, .progress = progress, .context = context};
    if (!export_entry(&x, top, top->root_dir, 0)) goto fail;
    archive = archive_write_new();
    if (!archive || archive_write_set_format_pax_restricted(archive) != ARCHIVE_OK ||
        archive_write_set_bytes_in_last_block(archive, 1) != ARCHIVE_OK) FAIL("Archive setup failed");
    if (archive_write_open(archive, &out_fd, archive_fd_open, archive_fd_write, archive_fd_close) != ARCHIVE_OK)
        FAIL("Archive setup failed: %s", archive_error_string(archive) ?: "unknown");
    x.archive = archive; x.counting = false; x.path[0] = 0;
    if (!export_entry(&x, top, top->root_dir, 0)) goto fail;
    struct overlay o = {.now = time(NULL)};
    bool overlaid = true;
    for (size_t i = 0; i < package_count && overlaid; i++) overlaid = overlay_package(&x, top, &o, packages[i]);
    if (overlaid && package_count) overlaid = overlay_bytes(&x, top, &o, "usr/lib/linuxrunner/init.sh", steamos_init, sizeof(steamos_init) - 1, 0755);
    for (size_t i = 0; i < o.count; i++) free(o.directories[i]);
    free(o.directories);
    if (!overlaid) goto fail;
    if (archive_write_close(archive) != ARCHIVE_OK) FAIL("Archive write failed: %s", archive_error_string(archive) ?: "unknown");
    if (progress) progress(context, 1, NULL);
    result = 0;
fail:
    if (archive) archive_write_free(archive);
    close_btrfs(&fs);
    if (result && !error[0]) snprintf(error, error_size, "SteamOS image export failed");
    return result;
}
