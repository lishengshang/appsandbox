/* ext4.h - minimal ext4 image writer.
 *
 * Feature subset (deliberately small):
 *   INCOMPAT  : FILETYPE | EXTENTS
 *   RO_COMPAT : SPARSE_SUPER | LARGE_FILE | HUGE_FILE | DIR_NLINK | EXTRA_ISIZE
 *
 * Layout:
 *   block_size       = 4096
 *   inode_size       = 256
 *   blocks_per_group = 32768  (128 MiB per group)
 *   inodes_per_group = 1024   (per-group default; total inodes = groups * 1024)
 *
 * The writer is two-phase from the caller's perspective:
 *   1. ext4_writer_open(): allocates layout, opens the phys disk, NO I/O yet
 *   2. ext4_writer_add_*(): each call buffers an entry into a per-dir tree
 *      and (for regular files) allocates + writes data blocks immediately
 *   3. ext4_writer_close(): writes superblocks, GDT, bitmaps, inode tables,
 *      and directory data blocks. All in one pass at close time.
 *
 * Memory: roughly 100 B/entry held in memory until close. ~5 MB for 52k.
 */
#ifndef POC_EXT4_H
#define POC_EXT4_H

#include <windows.h>
#include <stdint.h>

typedef struct ext4_writer ext4_writer_t;

/* Open a writer over [partition_lba_start, partition_lba_start+partition_lba_count)
 * on the given physical disk handle. Layout is computed but no I/O happens.
 * est_inodes is rounded up to a multiple of inodes_per_group. */
ext4_writer_t *ext4_writer_open(HANDLE phys,
                                uint64_t partition_lba_start,
                                uint64_t partition_lba_count,
                                uint32_t est_inodes);

/* Set the filesystem label (max 16 chars, copied). NULL for no label. */
void ext4_writer_set_label(ext4_writer_t *w, const char *label);

/* Add entries. Paths must be absolute (start with /). For a file, data may
 * be NULL iff size is 0. Symlink target is NOT null-terminated; pass length. */
int ext4_writer_add_dir(ext4_writer_t *w, const char *path,
                        uint16_t perms, uint32_t uid, uint32_t gid,
                        uint32_t mtime);
int ext4_writer_add_file(ext4_writer_t *w, const char *path,
                         uint16_t perms, uint32_t uid, uint32_t gid,
                         uint32_t mtime, const void *data, uint64_t size);
int ext4_writer_set_capability(ext4_writer_t *w, const char *path,
                               const void *data, size_t size);
int ext4_writer_add_symlink(ext4_writer_t *w, const char *path,
                            const char *target, size_t target_len,
                            uint32_t uid, uint32_t gid, uint32_t mtime);
int ext4_writer_add_special(ext4_writer_t *w, const char *path,
                            uint16_t mode_with_type,
                            uint32_t uid, uint32_t gid, uint32_t mtime,
                            uint32_t rdev);

/* Finalize: write superblocks, GDT backups, bitmaps, inode tables, and
 * directory data blocks. Returns 0 on success. */
int ext4_writer_close(ext4_writer_t *w);

/* Convenience: return the root partition's UUID once layout is decided.
 * Caller buffer must be >= 37 bytes (36 + null) for canonical text form. */
void ext4_writer_get_uuid_text(const ext4_writer_t *w, char *out, size_t cap);

/* Profiling: total bytes pushed into / flushed out of the coalescer,
 * and total elapsed perf-counter ticks spent in WriteFile, plus syscall
 * count. Useful for "where did the time go" reports. */
void ext4_writer_get_io_stats(const ext4_writer_t *w,
                              uint64_t *out_bytes_in,
                              uint64_t *out_bytes_flushed,
                              uint64_t *out_flush_ticks,
                              uint64_t *out_flush_calls);

#endif
