/* squashfs.h - read-only squashfs walker.
 *
 * Spec reference:
 *   https://dr-emann.github.io/squashfs/squashfs.html
 *
 * Format summary (all little-endian):
 *
 *   superblock (96 bytes at offset 0)
 *     - magic 0x73717368 ("hsqs")
 *     - compressor: 1=GZIP 2=LZMA 3=LZO 4=XZ 5=LZ4 6=ZSTD
 *     - block_size  (data block size, e.g. 128 KiB)
 *     - inode_table_start, directory_table_start
 *     - fragment_table_start, lookup_table_start
 *     - id_table_start, xattr_id_table_start
 *
 *   tables are stored as a chain of "metadata blocks":
 *     - 2-byte header: bit15 = is_uncompressed, bits 14..0 = on-disk length
 *     - up to 8 KiB of (possibly compressed) data which decompresses to <= 8 KiB
 *
 *   inodes are variable-size records inside the inode metadata stream.
 *     The first inode is at the offset specified by sb.root_inode_ref:
 *       high 48 bits = block start (offset relative to inode_table_start)
 *       low 16 bits  = byte offset within the *decompressed* block
 *
 *   directories are entries inside the directory metadata stream, grouped
 *     by header (shared inode_block, inode_base, type).
 *
 *   data blocks are stored contiguously, indexed by per-file block lists
 *     in the inode. The last partial block may be a "fragment" packed with
 *     others' tails into a fragment block.
 */
#ifndef POC_SQUASHFS_H
#define POC_SQUASHFS_H

#include <stdint.h>
#include <stdio.h>
#include <windows.h>

#define SQFS_MAGIC          0x73717368u
#define SQFS_METADATA_SIZE  8192u

enum sqfs_compressor {
    SQFS_COMP_GZIP = 1,
    SQFS_COMP_LZMA = 2,
    SQFS_COMP_LZO  = 3,
    SQFS_COMP_XZ   = 4,
    SQFS_COMP_LZ4  = 5,
    SQFS_COMP_ZSTD = 6
};

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t inode_count;
    uint32_t mod_time;
    uint32_t block_size;
    uint32_t fragment_entry_count;
    uint16_t compression_id;
    uint16_t block_log;
    uint16_t flags;
    uint16_t id_count;
    uint16_t version_major;
    uint16_t version_minor;
    uint64_t root_inode_ref;
    uint64_t bytes_used;
    uint64_t id_table_start;
    uint64_t xattr_id_table_start;
    uint64_t inode_table_start;
    uint64_t directory_table_start;
    uint64_t fragment_table_start;
    uint64_t lookup_table_start;
} sqfs_superblock_t;
#pragma pack(pop)

/* Open a squashfs file. Caller passes a wide path. Returns 0 on success
 * and fills *out_sb. The file handle stays inside the opaque context. */
typedef struct sqfs_ctx sqfs_ctx_t;

sqfs_ctx_t *sqfs_open(const wchar_t *path);
void        sqfs_close(sqfs_ctx_t *ctx);

/* Accessors. */
const sqfs_superblock_t *sqfs_sb(const sqfs_ctx_t *ctx);
const char *sqfs_compressor_name(uint16_t id);

/* Read raw bytes from the underlying file. Returns 0 on success. */
int sqfs_read_raw(sqfs_ctx_t *ctx, uint64_t off, void *buf, size_t len);

/* Decompress one squashfs metadata block starting at file offset `off`.
 * Writes up to SQFS_METADATA_SIZE bytes into `out`, returns the actual
 * decompressed length via *out_len, advances *off past the block.
 * Returns 0 on success. */
int sqfs_read_metadata_block(sqfs_ctx_t *ctx, uint64_t *off,
                             void *out, size_t *out_len);

/* Inode types from the squashfs spec. */
enum sqfs_inode_type {
    SQFS_DIR_TYPE   = 1,  SQFS_REG_TYPE   = 2,  SQFS_SYM_TYPE   = 3,
    SQFS_BLK_TYPE   = 4,  SQFS_CHR_TYPE   = 5,  SQFS_FIFO_TYPE  = 6,
    SQFS_SOCK_TYPE  = 7,
    SQFS_EDIR_TYPE  = 8,  SQFS_EREG_TYPE  = 9,  SQFS_ESYM_TYPE  = 10,
    SQFS_EBLK_TYPE  = 11, SQFS_ECHR_TYPE  = 12, SQFS_EFIFO_TYPE = 13,
    SQFS_ESOCK_TYPE = 14
};

/* What gets handed to the walker callback. */
typedef struct {
    const char    *path;          /* canonical absolute path, e.g. "/etc/hostname" */
    uint16_t       type;          /* sqfs_inode_type */
    uint16_t       mode;          /* unix mode bits (perms only, no type) */
    uint32_t       uid;
    uint32_t       gid;
    uint32_t       mtime;
    uint64_t       size;          /* file size (regular), symlink target len, etc. */
    uint32_t       rdev;          /* major/minor encoded for device nodes */
    const char    *symlink_target;/* non-NULL for symlinks; not null-terminated */
    uint32_t       symlink_target_size;
    /* For regular files, set by the walker, used by the callback to fetch
     * the bytes (deferred so the callback can stream / skip as needed). */
    uint64_t       _file_inode_off; /* internal: offset of file's inode in table */
    uint32_t       _xattr_idx;
} sqfs_entry_t;

/* Callback signature. Return 0 to continue, non-zero to abort traversal. */
typedef int (*sqfs_walk_cb_t)(const sqfs_entry_t *entry, void *user);

/* Walk the entire tree starting at root_inode_ref. Callback fires per
 * entry in pre-order (parent before children). Returns 0 on success. */
int sqfs_walk(sqfs_ctx_t *ctx, sqfs_walk_cb_t cb, void *user);

/* Read the data blocks of a regular file given an entry from the walker.
 * Allocates *out_data with malloc, sets *out_size. Caller frees.
 * Designed for Phase 4 ingestion - never holds the whole file in memory
 * for very large files... TODO streaming variant if Ubuntu has multi-GB
 * files we can't fit. For now, simple bulk read. */
int sqfs_read_file(sqfs_ctx_t *ctx, const sqfs_entry_t *entry,
                   void **out_data, size_t *out_size);

int sqfs_read_capability(sqfs_ctx_t *ctx, const sqfs_entry_t *entry,
                         void **out_data, size_t *out_size);

#endif
