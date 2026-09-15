/* squashfs.c - squashfs reader: superblock + metadata block decompressor. */
#include "squashfs.h"
#include "log.h"
#include "xz/xz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward decls (definition order is reader-friendly, not call-graph order).
 * sqfs_stream_t is defined further below alongside struct sqfs_ctx. */
struct sqfs_stream;
typedef struct sqfs_stream sqfs_stream_t;
int sqfs_read_metadata_block(sqfs_ctx_t *ctx, uint64_t *off,
                             void *out, size_t *out_len);
static int load_id_table(sqfs_ctx_t *ctx);
static int load_fragment_table(sqfs_ctx_t *ctx);
static int stream_load_range(sqfs_ctx_t *ctx, sqfs_stream_t *s,
                              uint64_t start, uint64_t end);

/* A decompressed "stream" of concatenated metadata blocks. We remember the
 * compressed start offset of each block (RELATIVE to table start, since
 * inode references and dir_entry.start_block are encoded that way) so we
 * can map an inode/dir reference to its decompressed position. */
struct sqfs_stream {
    uint8_t *buf;       /* concatenated decompressed bytes */
    size_t   len;
    size_t   cap;

    /* per-block mapping: c_off[i] = compressed offset relative to base,
     *                    d_off[i] = decompressed offset where block i starts */
    uint64_t *c_off;
    size_t   *d_off;
    size_t    n_blocks;
    size_t    cap_blocks;
    uint64_t  base;     /* absolute file offset of block 0 */
};

struct sqfs_ctx {
    HANDLE              file;
    sqfs_superblock_t   sb;
    sqfs_stream_t       inode_stream;
    sqfs_stream_t       dir_stream;
    uint32_t           *id_table;     /* uid/gid lookup */
    size_t              id_count;
    uint64_t           *fragment_index; /* fragment_entry[] flattened */
    size_t              fragment_count;
};

const char *sqfs_compressor_name(uint16_t id)
{
    switch (id) {
        case SQFS_COMP_GZIP: return "GZIP";
        case SQFS_COMP_LZMA: return "LZMA";
        case SQFS_COMP_LZO:  return "LZO";
        case SQFS_COMP_XZ:   return "XZ";
        case SQFS_COMP_LZ4:  return "LZ4";
        case SQFS_COMP_ZSTD: return "ZSTD";
        default:             return "UNKNOWN";
    }
}

sqfs_ctx_t *sqfs_open(const wchar_t *path)
{
    LOG_I("sqfs_open: %ls", path);

    /* Build the xz CRC32 table (idempotent - first caller wins).
     * Without this, xz_crc32 returns garbage and stream-flags CRC check
     * inside dec_stream_header fails with XZ_DATA_ERROR at in_pos=12. */
    static int crc_inited = 0;
    if (!crc_inited) { xz_crc32_init(); crc_inited = 1; }

    HANDLE h = CreateFileW(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        LOG_WIN32("CreateFileW(squashfs)", GetLastError());
        return NULL;
    }

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) {
        LOG_WIN32("GetFileSizeEx", GetLastError());
        CloseHandle(h);
        return NULL;
    }
    LOG_I("  file size: %lld bytes (%.2f MiB)",
          sz.QuadPart, (double)sz.QuadPart / (1024.0 * 1024.0));

    sqfs_ctx_t *ctx = (sqfs_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) { CloseHandle(h); return NULL; }
    ctx->file = h;

    /* Read + parse superblock. */
    DWORD br = 0;
    if (!ReadFile(h, &ctx->sb, sizeof(ctx->sb), &br, NULL) ||
        br != sizeof(ctx->sb)) {
        LOG_WIN32("ReadFile(superblock)", GetLastError());
        sqfs_close(ctx);
        return NULL;
    }

    if (ctx->sb.magic != SQFS_MAGIC) {
        LOG_E("  bad magic 0x%08x (expected 0x%08x)",
              ctx->sb.magic, SQFS_MAGIC);
        sqfs_close(ctx);
        return NULL;
    }

    LOG_I("  superblock:");
    LOG_I("    magic            0x%08x (OK)", ctx->sb.magic);
    LOG_I("    inode_count      %u", ctx->sb.inode_count);
    LOG_I("    block_size       %u (%u KiB)", ctx->sb.block_size, ctx->sb.block_size / 1024);
    LOG_I("    block_log        %u  (1 << %u = %u)",
          ctx->sb.block_log, ctx->sb.block_log, 1u << ctx->sb.block_log);
    LOG_I("    fragments        %u", ctx->sb.fragment_entry_count);
    LOG_I("    compressor       %u (%s)",
          ctx->sb.compression_id, sqfs_compressor_name(ctx->sb.compression_id));
    LOG_I("    flags            0x%04x", ctx->sb.flags);
    LOG_I("    version          %u.%u",
          ctx->sb.version_major, ctx->sb.version_minor);
    LOG_I("    root inode ref   0x%016llx (block=%llu off=%u)",
          (unsigned long long)ctx->sb.root_inode_ref,
          (unsigned long long)(ctx->sb.root_inode_ref >> 16),
          (unsigned)(ctx->sb.root_inode_ref & 0xffff));
    LOG_I("    bytes_used       %llu", (unsigned long long)ctx->sb.bytes_used);
    LOG_I("    inode_table      0x%llx", (unsigned long long)ctx->sb.inode_table_start);
    LOG_I("    directory_table  0x%llx", (unsigned long long)ctx->sb.directory_table_start);
    LOG_I("    fragment_table   0x%llx", (unsigned long long)ctx->sb.fragment_table_start);
    LOG_I("    lookup_table     0x%llx", (unsigned long long)ctx->sb.lookup_table_start);
    LOG_I("    id_table         0x%llx", (unsigned long long)ctx->sb.id_table_start);
    LOG_I("    xattr_id_table   0x%llx", (unsigned long long)ctx->sb.xattr_id_table_start);

    if (ctx->sb.version_major != 4) {
        LOG_E("  unsupported squashfs version %u.%u (need 4.x)",
              ctx->sb.version_major, ctx->sb.version_minor);
        sqfs_close(ctx);
        return NULL;
    }

    if (ctx->sb.compression_id != SQFS_COMP_XZ &&
        ctx->sb.compression_id != SQFS_COMP_GZIP) {
        LOG_E("  unsupported compressor %u (%s) - only XZ and GZIP impl'd",
              ctx->sb.compression_id, sqfs_compressor_name(ctx->sb.compression_id));
        sqfs_close(ctx);
        return NULL;
    }

    /* Validate block_size before any code divides/modulos by it (see
     * sqfs_read_file). It is image-controlled: must be non-zero, a power of
     * two, equal to (1 << block_log), and within the spec's 1 MiB ceiling. */
    if (ctx->sb.block_size == 0 ||
        (ctx->sb.block_size & (ctx->sb.block_size - 1)) != 0 ||
        ctx->sb.block_log >= 32 ||
        ctx->sb.block_size != (1u << ctx->sb.block_log) ||
        ctx->sb.block_size > (1u << 20)) {
        LOG_E("  invalid block_size %u (block_log %u)",
              ctx->sb.block_size, ctx->sb.block_log);
        sqfs_close(ctx);
        return NULL;
    }

    /* Pre-load id table, fragment table, inode table, dir table.
     * Each is small (a few MB max) and random access is O(1). */
    if (load_id_table(ctx) != 0) {
        LOG_E("  load_id_table failed"); sqfs_close(ctx); return NULL;
    }
    if (load_fragment_table(ctx) != 0) {
        LOG_E("  load_fragment_table failed"); sqfs_close(ctx); return NULL;
    }
    LOG_I("  loading inode table:");
    if (stream_load_range(ctx, &ctx->inode_stream,
                          ctx->sb.inode_table_start,
                          ctx->sb.directory_table_start) != 0) {
        LOG_E("  inode table load failed"); sqfs_close(ctx); return NULL;
    }
    LOG_I("  loading directory table:");
    if (stream_load_range(ctx, &ctx->dir_stream,
                          ctx->sb.directory_table_start,
                          ctx->sb.fragment_table_start) != 0) {
        LOG_E("  dir table load failed"); sqfs_close(ctx); return NULL;
    }

    return ctx;
}

static void stream_free(sqfs_stream_t *s)
{
    free(s->buf); s->buf = NULL;
    free(s->c_off); s->c_off = NULL;
    free(s->d_off); s->d_off = NULL;
}

void sqfs_close(sqfs_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->file != INVALID_HANDLE_VALUE) CloseHandle(ctx->file);
    stream_free(&ctx->inode_stream);
    stream_free(&ctx->dir_stream);
    free(ctx->id_table);
    free(ctx->fragment_index);
    free(ctx);
}

const sqfs_superblock_t *sqfs_sb(const sqfs_ctx_t *ctx) { return &ctx->sb; }

int sqfs_read_raw(sqfs_ctx_t *ctx, uint64_t off, void *buf, size_t len)
{
    /* Use OVERLAPPED with Offset to skip SetFilePointerEx. This makes the
     * call THREAD-SAFE: the file pointer isn't shared/mutated; the offset
     * is per-call. Even on a non-FILE_FLAG_OVERLAPPED handle this works -
     * the call is synchronous, just doesn't advance the file pointer.
     * Allows N decompress threads to read the same squashfs concurrently. */
    OVERLAPPED ov = { 0 };
    ov.Offset     = (DWORD)(off & 0xffffffffu);
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD br = 0;
    if (!ReadFile(ctx->file, buf, (DWORD)len, &br, &ov) || br != (DWORD)len) {
        DWORD err = GetLastError();
        LOG_WIN32("ReadFile(raw)", err);
        return -1;
    }
    return 0;
}

/* Append one decompressed block to the stream + record offsets. */
static int stream_append(sqfs_ctx_t *ctx, sqfs_stream_t *s, uint64_t c_off)
{
    if (s->cap < s->len + SQFS_METADATA_SIZE) {
        size_t newcap = (s->cap == 0) ? (1u << 20) : s->cap * 2;
        while (newcap < s->len + SQFS_METADATA_SIZE) newcap *= 2;
        uint8_t *nb = (uint8_t *)realloc(s->buf, newcap);
        if (!nb) return -1;
        s->buf = nb;
        s->cap = newcap;
    }
    if (s->n_blocks == s->cap_blocks) {
        size_t newcap = (s->cap_blocks == 0) ? 256 : s->cap_blocks * 2;
        uint64_t *nc = (uint64_t *)realloc(s->c_off, newcap * sizeof(uint64_t));
        size_t   *nd = (size_t   *)realloc(s->d_off, newcap * sizeof(size_t));
        if (!nc || !nd) { free(nc); free(nd); return -1; }
        s->c_off = nc; s->d_off = nd; s->cap_blocks = newcap;
    }

    uint64_t off = c_off;
    size_t block_len = 0;
    if (sqfs_read_metadata_block(ctx, &off, s->buf + s->len, &block_len) != 0) {
        return -1;
    }
    /* Store offset RELATIVE to stream base (= inode/dir table start). */
    s->c_off[s->n_blocks] = c_off - s->base;
    s->d_off[s->n_blocks] = s->len;
    s->n_blocks++;
    s->len += block_len;
    return (int)(off - c_off);    /* on-disk consumed */
}

/* Load a contiguous range of metadata blocks into a stream. */
static int stream_load_range(sqfs_ctx_t *ctx, sqfs_stream_t *s,
                              uint64_t start, uint64_t end)
{
    s->base = start;
    uint64_t cur = start;
    LOG_I("  loading metadata stream %llx..%llx (%llu bytes on disk)",
          (unsigned long long)start, (unsigned long long)end,
          (unsigned long long)(end - start));
    while (cur < end) {
        int adv = stream_append(ctx, s, cur);
        if (adv <= 0) {
            LOG_E("  stream_append at 0x%llx failed", (unsigned long long)cur);
            return -1;
        }
        cur += adv;
    }
    LOG_I("  -> %zu blocks, %zu decompressed bytes",
          s->n_blocks, s->len);
    return 0;
}

/* Convert a (compressed block offset, byte offset) reference into a flat
 * decompressed-stream offset. Returns -1 on lookup failure. */
static int64_t stream_resolve(const sqfs_stream_t *s,
                               uint64_t c_block_off, uint16_t byte_off)
{
    /* Linear scan - block lists are short (~hundreds). Could binary-search. */
    for (size_t i = 0; i < s->n_blocks; i++) {
        if (s->c_off[i] == c_block_off) {
            return (int64_t)(s->d_off[i] + byte_off);
        }
    }
    return -1;
}

/* The id_table is an indirection: at sb.id_table_start there's an array
 * of uint64_t pointing to metadata blocks; each block decompresses to a
 * uint32_t[] of UIDs/GIDs. */
static int load_id_table(sqfs_ctx_t *ctx)
{
    size_t n = ctx->sb.id_count;
    size_t bytes = n * sizeof(uint32_t);
    size_t n_blocks = (bytes + SQFS_METADATA_SIZE - 1) / SQFS_METADATA_SIZE;
    LOG_I("  id table: %zu entries -> %zu metadata block(s)", n, n_blocks);

    if (n == 0) { ctx->id_count = 0; return 0; }

    uint64_t *block_offs = (uint64_t *)malloc(n_blocks * sizeof(uint64_t));
    if (!block_offs) return -1;
    if (sqfs_read_raw(ctx, ctx->sb.id_table_start,
                      block_offs, n_blocks * sizeof(uint64_t)) != 0) {
        free(block_offs); return -1;
    }

    ctx->id_table = (uint32_t *)malloc(n_blocks * SQFS_METADATA_SIZE);
    if (!ctx->id_table) { free(block_offs); return -1; }

    size_t pos = 0;
    for (size_t i = 0; i < n_blocks; i++) {
        uint64_t off = block_offs[i];
        uint8_t tmp[SQFS_METADATA_SIZE];
        size_t len = 0;
        if (sqfs_read_metadata_block(ctx, &off, tmp, &len) != 0) {
            free(block_offs); free(ctx->id_table); ctx->id_table = NULL;
            return -1;
        }
        memcpy((uint8_t *)ctx->id_table + pos, tmp, len);
        pos += len;
    }
    ctx->id_count = pos / sizeof(uint32_t);
    free(block_offs);
    return 0;
}

/* Lookup uid/gid via id_index. */
static uint32_t id_lookup(const sqfs_ctx_t *ctx, uint16_t idx)
{
    if (idx >= ctx->id_count) return 0;
    return ctx->id_table[idx];
}

/* Fragment table: parallel structure to id_table. Each fragment entry
 * is 16 bytes: u64 start, u32 size, u32 unused. */
typedef struct {
    uint64_t start;
    uint32_t size;
    uint32_t unused;
} sqfs_fragment_entry_t;

static int load_fragment_table(sqfs_ctx_t *ctx)
{
    size_t n = ctx->sb.fragment_entry_count;
    size_t bytes = n * sizeof(sqfs_fragment_entry_t);
    size_t n_blocks = (bytes + SQFS_METADATA_SIZE - 1) / SQFS_METADATA_SIZE;
    LOG_I("  fragment table: %zu entries -> %zu metadata block(s)", n, n_blocks);

    if (n == 0) { ctx->fragment_count = 0; return 0; }

    uint64_t *block_offs = (uint64_t *)malloc(n_blocks * sizeof(uint64_t));
    if (!block_offs) return -1;
    if (sqfs_read_raw(ctx, ctx->sb.fragment_table_start,
                      block_offs, n_blocks * sizeof(uint64_t)) != 0) {
        free(block_offs); return -1;
    }

    ctx->fragment_index = (uint64_t *)malloc(n_blocks * SQFS_METADATA_SIZE);
    if (!ctx->fragment_index) { free(block_offs); return -1; }

    size_t pos = 0;
    for (size_t i = 0; i < n_blocks; i++) {
        uint64_t off = block_offs[i];
        uint8_t tmp[SQFS_METADATA_SIZE];
        size_t len = 0;
        if (sqfs_read_metadata_block(ctx, &off, tmp, &len) != 0) {
            free(block_offs); return -1;
        }
        memcpy((uint8_t *)ctx->fragment_index + pos, tmp, len);
        pos += len;
    }
    ctx->fragment_count = pos / sizeof(sqfs_fragment_entry_t);
    free(block_offs);
    return 0;
}

int sqfs_read_metadata_block(sqfs_ctx_t *ctx, uint64_t *off,
                             void *out, size_t *out_len)
{
    /* 2-byte header: top bit = uncompressed flag, low 15 bits = on-disk size. */
    uint16_t hdr;
    if (sqfs_read_raw(ctx, *off, &hdr, 2) != 0) return -1;

    int uncompressed = (hdr & 0x8000) ? 1 : 0;
    uint16_t disk_len = hdr & 0x7fff;
    if (disk_len == 0 || disk_len > SQFS_METADATA_SIZE) {
        LOG_E("metadata block at 0x%llx: bad disk_len=%u",
              (unsigned long long)*off, disk_len);
        return -1;
    }

    uint8_t inbuf[SQFS_METADATA_SIZE];
    if (sqfs_read_raw(ctx, *off + 2, inbuf, disk_len) != 0) return -1;

    if (uncompressed) {
        memcpy(out, inbuf, disk_len);
        *out_len = disk_len;
        *off += 2 + disk_len;
        return 0;
    }

    /* Compressed: dispatch on compressor. Only XZ + GZIP impl'd for now. */
    if (ctx->sb.compression_id == SQFS_COMP_XZ) {
        /* XZ_DYNALLOC: multi-call mode. We give all input at once + a
         * large enough output buffer, then loop until XZ_STREAM_END or
         * XZ_OK with no progress. Linux's squashfs driver uses PREALLOC;
         * DYNALLOC suits us since we only need a decoder per block. */
        struct xz_dec *dec = xz_dec_init(XZ_DYNALLOC, 1u << 16);
        if (!dec) { LOG_E("xz_dec_init OOM"); return -1; }

        struct xz_buf b = { 0 };
        b.in = inbuf;
        b.in_pos = 0;
        b.in_size = disk_len;
        b.out = (uint8_t *)out;
        b.out_pos = 0;
        b.out_size = SQFS_METADATA_SIZE;

        enum xz_ret r;
        for (;;) {
            size_t prev_in = b.in_pos, prev_out = b.out_pos;
            r = xz_dec_run(dec, &b);
            if (r == XZ_STREAM_END) break;
            if (r != XZ_OK) {
                LOG_E("xz_dec_run at 0x%llx: rc=%d (in=%zu/%zu out=%zu/%zu)",
                      (unsigned long long)*off, (int)r,
                      b.in_pos, b.in_size, b.out_pos, b.out_size);
                xz_dec_end(dec);
                return -1;
            }
            if (b.in_pos == prev_in && b.out_pos == prev_out) {
                LOG_E("xz_dec_run stalled: rc=%d in=%zu/%zu out=%zu/%zu",
                      (int)r, b.in_pos, b.in_size, b.out_pos, b.out_size);
                xz_dec_end(dec);
                return -1;
            }
        }
        xz_dec_end(dec);

        *out_len = b.out_pos;
        *off += 2 + disk_len;
        return 0;
    }

    LOG_E("compressor %u not implemented", ctx->sb.compression_id);
    return -1;
}

typedef struct {
    uint64_t next_block;
    uint64_t end;
    size_t pos;
    size_t len;
    uint8_t data[SQFS_METADATA_SIZE];
} sqfs_xattr_cursor_t;

static int xattr_next_block(sqfs_ctx_t *ctx, sqfs_xattr_cursor_t *c)
{
    uint16_t hdr;
    if (c->next_block >= c->end || c->end - c->next_block < 2 ||
        sqfs_read_raw(ctx, c->next_block, &hdr, sizeof(hdr)) != 0 ||
        (hdr & 0x7fff) > c->end - c->next_block - 2)
        return -1;
    if (sqfs_read_metadata_block(ctx, &c->next_block, c->data, &c->len) != 0 ||
        c->len == 0 || (c->next_block != c->end && c->len != SQFS_METADATA_SIZE))
        return -1;
    c->pos = 0;
    return 0;
}

static int xattr_cursor_start(sqfs_ctx_t *ctx, sqfs_xattr_cursor_t *c,
                              uint64_t start, uint64_t end, size_t offset)
{
    if (end > ctx->sb.bytes_used || start >= end || offset >= SQFS_METADATA_SIZE)
        return -1;
    c->next_block = start;
    c->end = end;
    if (xattr_next_block(ctx, c) != 0 || offset >= c->len) return -1;
    c->pos = offset;
    return 0;
}

static int xattr_read(sqfs_ctx_t *ctx, sqfs_xattr_cursor_t *c,
                       void *out, size_t size)
{
    uint8_t *p = (uint8_t *)out;
    while (size) {
        if (c->pos == c->len && xattr_next_block(ctx, c) != 0) return -1;
        size_t n = c->len - c->pos;
        if (n > size) n = size;
        if (p) { memcpy(p, c->data + c->pos, n); p += n; }
        c->pos += n;
        size -= n;
    }
    return 0;
}

int sqfs_read_capability(sqfs_ctx_t *ctx, const sqfs_entry_t *entry,
                         void **out_data, size_t *out_size)
{
    *out_data = NULL;
    *out_size = 0;
    if (entry->type != SQFS_EREG_TYPE || entry->_xattr_idx == UINT32_MAX)
        return 0;

    struct {
        uint64_t start;
        uint32_t count;
        uint32_t unused;
    } table;
    struct {
        uint64_t ref;
        uint32_t count;
        uint32_t size;
    } id;
    uint64_t table_off = ctx->sb.xattr_id_table_start;
    if (table_off > ctx->sb.bytes_used ||
        sizeof(table) > ctx->sb.bytes_used - table_off ||
        sqfs_read_raw(ctx, table_off, &table, sizeof(table)) != 0 ||
        entry->_xattr_idx >= table.count)
        return -1;

    uint64_t index_bytes = (((uint64_t)table.count * sizeof(id) +
                            SQFS_METADATA_SIZE - 1) / SQFS_METADATA_SIZE) * 8;
    table_off += sizeof(table);
    if (index_bytes != ctx->sb.bytes_used - table_off) return -1;
    uint64_t xattr_end, id_block;
    uint64_t id_offset = (uint64_t)entry->_xattr_idx * sizeof(id);
    if (sqfs_read_raw(ctx, table_off, &xattr_end, sizeof(xattr_end)) != 0 ||
        sqfs_read_raw(ctx, table_off + (id_offset / SQFS_METADATA_SIZE) * 8,
                      &id_block, sizeof(id_block)) != 0 ||
        table.start < sizeof(sqfs_superblock_t) || table.start >= xattr_end ||
        xattr_end > id_block || id_block >= ctx->sb.xattr_id_table_start)
        return -1;

    sqfs_xattr_cursor_t c;
    if (xattr_cursor_start(ctx, &c, id_block, ctx->sb.xattr_id_table_start,
                           (size_t)(id_offset % SQFS_METADATA_SIZE)) != 0 ||
        xattr_read(ctx, &c, &id, sizeof(id)) != 0 ||
        id.count == 0 ||
        (id.ref >> 16) >= xattr_end - table.start ||
        xattr_cursor_start(ctx, &c, table.start + (id.ref >> 16), xattr_end,
                           (size_t)(id.ref & 0xffff)) != 0)
        return -1;

    for (uint32_t i = 0; i < id.count; i++) {
        uint16_t attr[2];
        char name[sizeof("capability") - 1];
        uint32_t value_size;
        if (xattr_read(ctx, &c, attr, sizeof(attr)) != 0 ||
            attr[1] == 0 || (attr[0] & ~0x1ffu) != 0)
            return -1;
        int match = (attr[0] & 0xff) == 2 && attr[1] == sizeof(name);
        if (xattr_read(ctx, &c, match ? name : NULL, attr[1]) != 0 ||
            xattr_read(ctx, &c, &value_size, sizeof(value_size)) != 0 ||
            ((attr[0] & 0x100) && value_size != sizeof(uint64_t)))
            return -1;
        if (!match || memcmp(name, "capability", sizeof(name)) != 0) {
            if (xattr_read(ctx, &c, NULL, value_size) != 0) return -1;
            continue;
        }

        if (attr[0] & 0x100) {
            uint64_t ref;
            if (xattr_read(ctx, &c, &ref, sizeof(ref)) != 0 ||
                (ref >> 16) >= xattr_end - table.start ||
                xattr_cursor_start(ctx, &c, table.start + (ref >> 16), xattr_end,
                                   (size_t)(ref & 0xffff)) != 0 ||
                xattr_read(ctx, &c, &value_size, sizeof(value_size)) != 0)
                return -1;
        }
        if (value_size != 12 && value_size != 20 && value_size != 24) return -1;
        void *value = malloc(value_size);
        if (!value) return -1;
        if (xattr_read(ctx, &c, value, value_size) != 0) {
            free(value);
            return -1;
        }
        *out_data = value;
        *out_size = value_size;
        return 0;
    }
    return 0;
}

/* ---- Inode + directory parsing ----
 *
 * All structs are stored little-endian. We use memcpy into local typed
 * variables instead of casting (avoids alignment issues on platforms
 * that care, plus MSVC warning-clean).
 */

#pragma pack(push, 1)
typedef struct {
    uint16_t type;
    uint16_t mode;
    uint16_t uid_idx;
    uint16_t gid_idx;
    uint32_t mtime;
    uint32_t inode_number;
} sqfs_inode_common_t;

typedef struct {
    uint32_t block_idx;
    uint32_t nlink;
    uint16_t file_size;
    uint16_t block_offset;
    uint32_t parent_inode;
} sqfs_basic_dir_t;

typedef struct {
    uint32_t nlink;
    uint32_t file_size;
    uint32_t block_idx;
    uint32_t parent_inode;
    uint16_t index_count;
    uint16_t block_offset;
    uint32_t xattr_idx;
} sqfs_ext_dir_t;

typedef struct {
    uint32_t blocks_start;
    uint32_t frag_idx;
    uint32_t block_offset;
    uint32_t file_size;
    /* uint32_t block_sizes[] follows */
} sqfs_basic_file_t;

typedef struct {
    uint64_t blocks_start;
    uint64_t file_size;
    uint64_t sparse;
    uint32_t nlink;
    uint32_t frag_idx;
    uint32_t block_offset;
    uint32_t xattr_idx;
    /* uint32_t block_sizes[] follows */
} sqfs_ext_file_t;

typedef struct {
    uint32_t nlink;
    uint32_t target_size;
    /* char target[target_size] follows */
} sqfs_basic_symlink_t;

typedef struct {
    uint32_t nlink;
    uint32_t rdev;
} sqfs_basic_dev_t;

typedef struct {
    uint32_t count;
    uint32_t start_block;
    uint32_t inode_number;
} sqfs_dir_header_t;

typedef struct {
    uint16_t offset;
    int16_t  inode_offset;
    uint16_t type;
    uint16_t name_size;
    /* char name[name_size+1] follows */
} sqfs_dir_entry_t;
#pragma pack(pop)

/* Track ancestors for cycle detection (squashfs shouldn't have them, but
 * defensive). Static stack OK - max depth ~30 in real filesystems. */
#define WALK_MAX_DEPTH 64

typedef struct {
    sqfs_ctx_t       *ctx;
    sqfs_walk_cb_t    cb;
    void             *user;
    char              path_buf[4096];
    size_t            path_len;
    int               depth;
    int               cb_rc;     /* sticky non-zero stops further callbacks */
    /* stats */
    size_t            n_files;
    size_t            n_dirs;
    size_t            n_syms;
    size_t            n_special;
} walk_state_t;

/* Push a name onto path_buf, return previous length to pop later. */
static size_t path_push(walk_state_t *w, const char *name, size_t namelen)
{
    size_t prev = w->path_len;
    if (prev + 1 + namelen + 1 > sizeof(w->path_buf)) {
        return prev;  /* truncate silently rather than overflow */
    }
    if (prev > 1 || (prev == 1 && w->path_buf[0] != '/')) {
        w->path_buf[w->path_len++] = '/';
    } else if (prev == 0) {
        w->path_buf[w->path_len++] = '/';
    }
    memcpy(w->path_buf + w->path_len, name, namelen);
    w->path_len += namelen;
    w->path_buf[w->path_len] = 0;
    return prev;
}
static void path_pop(walk_state_t *w, size_t prev) {
    w->path_len = prev;
    w->path_buf[w->path_len] = 0;
}

/* Forward decls. */
static int walk_inode(walk_state_t *w, uint64_t inode_ref);

/* Resolve inode_ref (high48 = compressed block_off, low16 = byte_off)
 * to a pointer inside the decompressed inode stream + the inode common header. */
static int read_inode_common(sqfs_ctx_t *ctx, uint64_t ref,
                              const uint8_t **out_p, sqfs_inode_common_t *out_hdr)
{
    uint64_t block_off = ref >> 16;
    uint16_t byte_off  = (uint16_t)(ref & 0xffff);
    int64_t d = stream_resolve(&ctx->inode_stream, block_off, byte_off);
    if (d < 0 || (size_t)d + sizeof(sqfs_inode_common_t) > ctx->inode_stream.len) {
        LOG_E("inode ref 0x%llx resolves out of range (d=%lld)",
              (unsigned long long)ref, (long long)d);
        return -1;
    }
    *out_p = ctx->inode_stream.buf + d;
    memcpy(out_hdr, *out_p, sizeof(*out_hdr));
    return 0;
}

/* Walk a directory metadata block starting at (dir_block_off, byte_off)
 * with logical size `dir_size` (includes the 3 bytes of `. ..` accounting).
 * For each entry, recurse into the child inode. */
static int walk_directory(walk_state_t *w, uint32_t dir_block_off,
                           uint16_t byte_off, uint32_t dir_size)
{
    sqfs_ctx_t *ctx = w->ctx;
    int64_t d = stream_resolve(&ctx->dir_stream, dir_block_off, byte_off);
    if (d < 0 || (size_t)d > ctx->dir_stream.len) {
        LOG_E("dir block 0x%x byte_off=%u not in dir stream",
              dir_block_off, byte_off);
        return -1;
    }
    const uint8_t *base = ctx->dir_stream.buf + d;

    /* Per spec: file_size is 3 bytes more than the actual directory bytes,
     * accounting for `.` and `..` which aren't stored. So actual bytes = size - 3. */
    if (dir_size < 3) return 0;
    size_t bytes_left = dir_size - 3;
    /* dir_size is image-controlled; clamp the scan window to what actually
     * remains in the decompressed dir stream so `end` cannot run past buf. */
    {
        size_t avail = ctx->dir_stream.len - (size_t)d;
        if (bytes_left > avail) bytes_left = avail;
    }
    const uint8_t *p = base;
    const uint8_t *end = base + bytes_left;

    while (p < end) {
        if (p + sizeof(sqfs_dir_header_t) > end) {
            LOG_W("dir at block=0x%x: truncated header", dir_block_off);
            break;
        }
        sqfs_dir_header_t dh;
        memcpy(&dh, p, sizeof(dh));
        p += sizeof(dh);

        uint32_t count = dh.count + 1;  /* stored as count-1 */
        for (uint32_t i = 0; i < count; i++) {
            if (p + sizeof(sqfs_dir_entry_t) > end) {
                LOG_W("dir at block=0x%x: truncated entry", dir_block_off);
                return -1;
            }
            sqfs_dir_entry_t de;
            memcpy(&de, p, sizeof(de));
            p += sizeof(de);
            size_t name_len = (size_t)de.name_size + 1;
            if (p + name_len > end) {
                LOG_W("dir at block=0x%x: truncated name", dir_block_off);
                return -1;
            }
            char name[256];
            size_t name_copy = name_len < sizeof(name) - 1 ? name_len : sizeof(name) - 1;
            memcpy(name, p, name_copy);
            name[name_copy] = 0;
            p += name_len;

            /* Build inode ref for child. */
            uint64_t child_ref = ((uint64_t)dh.start_block << 16) | de.offset;

            size_t prev = path_push(w, name, name_copy);
            int rc = walk_inode(w, child_ref);
            path_pop(w, prev);
            if (rc) return rc;
            if (w->cb_rc) return w->cb_rc;
        }
    }
    return 0;
}

static int walk_inode(walk_state_t *w, uint64_t inode_ref)
{
    sqfs_ctx_t *ctx = w->ctx;
    if (w->depth >= WALK_MAX_DEPTH) {
        LOG_W("walk depth limit hit at %s", w->path_buf);
        return 0;
    }

    const uint8_t *p = NULL;
    sqfs_inode_common_t hdr;
    if (read_inode_common(ctx, inode_ref, &p, &hdr) != 0) return -1;
    const uint8_t *body = p + sizeof(hdr);
    size_t body_avail = ctx->inode_stream.len -
                        (size_t)(body - ctx->inode_stream.buf);

    sqfs_entry_t e = { 0 };
    e.path  = w->path_buf;
    e.type  = hdr.type;
    e.mode  = hdr.mode;
    e.uid   = id_lookup(ctx, hdr.uid_idx);
    e.gid   = id_lookup(ctx, hdr.gid_idx);
    e.mtime = hdr.mtime;
    e._file_inode_off = (size_t)(p - ctx->inode_stream.buf);
    e._xattr_idx = UINT32_MAX;

    switch (hdr.type) {
    case SQFS_DIR_TYPE: {
        if (body_avail < sizeof(sqfs_basic_dir_t)) return -1;
        sqfs_basic_dir_t d;
        memcpy(&d, body, sizeof(d));
        e.size = 0;
        if (w->cb_rc == 0) w->cb_rc = w->cb(&e, w->user);
        if (w->cb_rc) return w->cb_rc;
        w->n_dirs++;
        w->depth++;
        int rc = walk_directory(w, d.block_idx, d.block_offset, d.file_size);
        w->depth--;
        return rc;
    }
    case SQFS_EDIR_TYPE: {
        if (body_avail < sizeof(sqfs_ext_dir_t)) return -1;
        sqfs_ext_dir_t d;
        memcpy(&d, body, sizeof(d));
        e.size = 0;
        if (w->cb_rc == 0) w->cb_rc = w->cb(&e, w->user);
        if (w->cb_rc) return w->cb_rc;
        w->n_dirs++;
        w->depth++;
        int rc = walk_directory(w, d.block_idx, d.block_offset, d.file_size);
        w->depth--;
        return rc;
    }
    case SQFS_REG_TYPE: {
        if (body_avail < sizeof(sqfs_basic_file_t)) return -1;
        sqfs_basic_file_t f;
        memcpy(&f, body, sizeof(f));
        e.size = f.file_size;
        if (w->cb_rc == 0) w->cb_rc = w->cb(&e, w->user);
        w->n_files++;
        return w->cb_rc;
    }
    case SQFS_EREG_TYPE: {
        if (body_avail < sizeof(sqfs_ext_file_t)) return -1;
        sqfs_ext_file_t f;
        memcpy(&f, body, sizeof(f));
        e.size = f.file_size;
        e._xattr_idx = f.xattr_idx;
        if (w->cb_rc == 0) w->cb_rc = w->cb(&e, w->user);
        w->n_files++;
        return w->cb_rc;
    }
    case SQFS_SYM_TYPE:
    case SQFS_ESYM_TYPE: {
        if (body_avail < sizeof(sqfs_basic_symlink_t)) return -1;
        sqfs_basic_symlink_t s;
        memcpy(&s, body, sizeof(s));
        if (body_avail < sizeof(s) + s.target_size) return -1;
        e.size = s.target_size;
        e.symlink_target = (const char *)(body + sizeof(s));
        e.symlink_target_size = s.target_size;
        if (w->cb_rc == 0) w->cb_rc = w->cb(&e, w->user);
        w->n_syms++;
        return w->cb_rc;
    }
    case SQFS_BLK_TYPE: case SQFS_CHR_TYPE:
    case SQFS_EBLK_TYPE: case SQFS_ECHR_TYPE: {
        if (body_avail < sizeof(sqfs_basic_dev_t)) return -1;
        sqfs_basic_dev_t dv;
        memcpy(&dv, body, sizeof(dv));
        e.rdev = dv.rdev;
        if (w->cb_rc == 0) w->cb_rc = w->cb(&e, w->user);
        w->n_special++;
        return w->cb_rc;
    }
    case SQFS_FIFO_TYPE: case SQFS_SOCK_TYPE:
    case SQFS_EFIFO_TYPE: case SQFS_ESOCK_TYPE: {
        if (w->cb_rc == 0) w->cb_rc = w->cb(&e, w->user);
        w->n_special++;
        return w->cb_rc;
    }
    default:
        LOG_W("inode at %s: unknown type %u", w->path_buf, hdr.type);
        return 0;
    }
}

int sqfs_walk(sqfs_ctx_t *ctx, sqfs_walk_cb_t cb, void *user)
{
    walk_state_t w = { 0 };
    w.ctx = ctx;
    w.cb  = cb;
    w.user = user;
    w.path_buf[0] = 0;
    LOG_I("sqfs_walk: starting at root_inode_ref=0x%llx",
          (unsigned long long)ctx->sb.root_inode_ref);

    int rc = walk_inode(&w, ctx->sb.root_inode_ref);
    LOG_I("  walk done: rc=%d cb_rc=%d files=%zu dirs=%zu syms=%zu special=%zu",
          rc, w.cb_rc, w.n_files, w.n_dirs, w.n_syms, w.n_special);
    return rc ? rc : w.cb_rc;
}

/* Reusable XZ decoder for hot-path data block decompression. xz_dec_init
 * allocates ~100 KB of LZMA2 state; doing it per data block (50k+ for a
 * desktop rootfs) burns 5+ GB of allocator churn. xz_dec_reset is cheap.
 *
 * TLS: with the multi-threaded decompress pipeline each worker gets its
 * own instance (xz_dec is not thread-safe; one shared instance would
 * corrupt across concurrent decompresses). */
static __declspec(thread) struct xz_dec *g_data_dec = NULL;

static int data_decompress(const uint8_t *cbuf, size_t in_size,
                           uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!g_data_dec) {
        g_data_dec = xz_dec_init(XZ_DYNALLOC, 1u << 20);
        if (!g_data_dec) { LOG_E("xz_dec_init OOM"); return -1; }
    } else {
        xz_dec_reset(g_data_dec);
    }
    struct xz_buf b = { 0 };
    b.in = cbuf; b.in_size = in_size;
    b.out = out; b.out_size = out_cap;
    enum xz_ret r;
    for (;;) {
        size_t pi = b.in_pos, po = b.out_pos;
        r = xz_dec_run(g_data_dec, &b);
        if (r == XZ_STREAM_END) break;
        if (r != XZ_OK || (b.in_pos == pi && b.out_pos == po)) {
            LOG_E("data_decompress: rc=%d in=%zu/%zu out=%zu/%zu",
                  (int)r, b.in_pos, b.in_size, b.out_pos, b.out_size);
            return -1;
        }
    }
    *out_len = b.out_pos;
    return 0;
}

/* ---- File content read ----
 *
 * Reads all data blocks for a regular file, concatenating into out_data.
 * Fragment tail (if any) appended last.
 */
int sqfs_read_file(sqfs_ctx_t *ctx, const sqfs_entry_t *entry,
                   void **out_data, size_t *out_size)
{
    const uint8_t *p = ctx->inode_stream.buf + entry->_file_inode_off;
    sqfs_inode_common_t hdr;
    size_t inode_off;
    size_t body_avail;
    memcpy(&hdr, p, sizeof(hdr));
    const uint8_t *body = p + sizeof(hdr);

    uint64_t blocks_start;
    uint64_t file_size;
    uint32_t frag_idx;
    uint32_t frag_off;
    const uint32_t *block_sizes;
    size_t block_list_avail;   /* # of uint32 block_sizes[] entries in body */

    /* body must lie within the decompressed inode stream; bytes after the
     * fixed struct hold the variable-length block_sizes[] array. */
    inode_off = (size_t)(body - ctx->inode_stream.buf);
    if (body < ctx->inode_stream.buf || inode_off > ctx->inode_stream.len) {
        LOG_E("sqfs_read_file: %s: inode body out of range", entry->path);
        return -1;
    }
    body_avail = ctx->inode_stream.len - inode_off;

    if (hdr.type == SQFS_REG_TYPE) {
        sqfs_basic_file_t f;
        if (body_avail < sizeof(f)) {
            LOG_E("sqfs_read_file: %s: inode body too short", entry->path);
            return -1;
        }
        memcpy(&f, body, sizeof(f));
        blocks_start = f.blocks_start;
        file_size    = f.file_size;
        frag_idx     = f.frag_idx;
        frag_off     = f.block_offset;
        block_sizes  = (const uint32_t *)(body + sizeof(f));
        block_list_avail = (body_avail - sizeof(f)) / sizeof(uint32_t);
    } else if (hdr.type == SQFS_EREG_TYPE) {
        sqfs_ext_file_t f;
        if (body_avail < sizeof(f)) {
            LOG_E("sqfs_read_file: %s: inode body too short", entry->path);
            return -1;
        }
        memcpy(&f, body, sizeof(f));
        blocks_start = f.blocks_start;
        file_size    = f.file_size;
        frag_idx     = f.frag_idx;
        frag_off     = f.block_offset;
        block_sizes  = (const uint32_t *)(body + sizeof(f));
        block_list_avail = (body_avail - sizeof(f)) / sizeof(uint32_t);
    } else {
        LOG_E("sqfs_read_file: %s is not a regular file (type=%u)",
              entry->path, hdr.type);
        return -1;
    }

    size_t block_size = ctx->sb.block_size;
    if (block_size == 0) {
        LOG_E("sqfs_read_file: %s: zero block_size", entry->path);
        return -1;
    }
    size_t n_full = (size_t)(file_size / block_size);
    int has_frag = (frag_idx != 0xffffffffu);
    size_t tail = (size_t)(file_size % block_size);

    uint8_t *out = (uint8_t *)malloc(file_size ? file_size : 1);
    if (!out) return -1;

    uint64_t disk = blocks_start;
    size_t written = 0;

    /* Full blocks (and the last block if no fragment). */
    size_t block_list_count = has_frag ? n_full : (n_full + (tail ? 1 : 0));
    /* The block_sizes[] array is image-controlled in length only via the
     * inode body; reject if the inode does not actually carry that many
     * uint32 entries (else block_sizes[i] reads past inode_stream). */
    if (block_list_count > block_list_avail) {
        LOG_E("sqfs_read_file: %s: block_list_count %zu exceeds inode body (%zu)",
              entry->path, block_list_count, block_list_avail);
        free(out); return -1;
    }
    for (size_t i = 0; i < block_list_count; i++) {
        uint32_t bs = block_sizes[i];
        int uncompressed = (bs & 0x01000000) ? 1 : 0;
        size_t on_disk = bs & 0x00ffffffu;
        size_t this_block_logical = (i < n_full) ? block_size : tail;
        /* Bytes still free in `out`. Every write below must stay within it;
         * on valid input each block writes exactly this_block_logical and
         * the sum equals file_size, so the normal path is unchanged. */
        size_t remaining = (size_t)file_size - written;

        if (this_block_logical > remaining) {
            LOG_E("sqfs_read_file: %s: block %zu overruns output", entry->path, i);
            free(out); return -1;
        }

        if (on_disk == 0) {
            /* sparse - zero fill */
            memset(out + written, 0, this_block_logical);
            written += this_block_logical;
            continue;
        }

        if (uncompressed) {
            /* An uncompressed block holds its bytes verbatim; it must equal
             * the logical block length and fit in the remaining output. */
            if (on_disk != this_block_logical) {
                LOG_E("sqfs_read_file: %s: uncompressed block %zu size %zu != %zu",
                      entry->path, i, on_disk, this_block_logical);
                free(out); return -1;
            }
            if (sqfs_read_raw(ctx, disk, out + written, on_disk) != 0) {
                free(out); return -1;
            }
            written += on_disk;
        } else {
            /* Compressed data block. Reuse the cached XZ decoder via
             * data_decompress (xz_dec_reset is cheap, init is not). */
            uint8_t *cbuf = (uint8_t *)malloc(on_disk);
            if (!cbuf) { free(out); return -1; }
            if (sqfs_read_raw(ctx, disk, cbuf, on_disk) != 0) {
                free(cbuf); free(out); return -1;
            }
            size_t got = 0;
            /* Output cap is the true remaining logical block length, not the
             * full block_size: the tail block has only `tail` bytes left. */
            if (data_decompress(cbuf, on_disk, out + written, this_block_logical, &got) != 0) {
                LOG_E("data block decompress at 0x%llx", (unsigned long long)disk);
                free(cbuf); free(out); return -1;
            }
            if (got != this_block_logical) {
                LOG_E("sqfs_read_file: %s: block %zu decompressed %zu != %zu",
                      entry->path, i, got, this_block_logical);
                free(cbuf); free(out); return -1;
            }
            free(cbuf);
            written += got;
        }
        disk += on_disk;
    }

    /* Fragment tail. */
    if (has_frag && tail > 0) {
        if ((size_t)frag_idx >= ctx->fragment_count) {
            LOG_E("%s: fragment_idx %u out of range", entry->path, frag_idx);
            free(out); return -1;
        }
        sqfs_fragment_entry_t fe;
        memcpy(&fe,
               (const uint8_t *)ctx->fragment_index + frag_idx * sizeof(fe),
               sizeof(fe));

        int uncompressed = (fe.size & 0x01000000) ? 1 : 0;
        size_t on_disk = fe.size & 0x00ffffffu;
        uint8_t *fragbuf = (uint8_t *)malloc(on_disk);
        if (!fragbuf) { free(out); return -1; }
        if (sqfs_read_raw(ctx, fe.start, fragbuf, on_disk) != 0) {
            free(fragbuf); free(out); return -1;
        }
        uint8_t *fragdec = fragbuf;
        size_t fragdec_size = on_disk;
        uint8_t *fragdec_alloc = NULL;
        if (!uncompressed) {
            fragdec_alloc = (uint8_t *)malloc(block_size);
            if (!fragdec_alloc) { free(fragbuf); free(out); return -1; }
            size_t got = 0;
            if (data_decompress(fragbuf, on_disk, fragdec_alloc, block_size, &got) != 0) {
                free(fragdec_alloc); free(fragbuf); free(out); return -1;
            }
            fragdec = fragdec_alloc;
            fragdec_size = got;
        }
        if (frag_off + tail > fragdec_size) {
            LOG_E("%s: fragment slice out of range", entry->path);
            free(fragdec_alloc); free(fragbuf); free(out); return -1;
        }
        memcpy(out + written, fragdec + frag_off, tail);
        written += tail;
        free(fragdec_alloc); free(fragbuf);
    }

    if (written != file_size) {
        LOG_E("%s: written=%zu != file_size=%llu",
              entry->path, written, (unsigned long long)file_size);
        free(out); return -1;
    }

    *out_data = out;
    *out_size = (size_t)file_size;
    return 0;
}
