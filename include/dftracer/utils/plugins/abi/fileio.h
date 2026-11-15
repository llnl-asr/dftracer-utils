// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_ABI_FILEIO_H
#define DFTRACER_UTILS_PLUGINS_ABI_FILEIO_H

/** @file
 * dftu.svc.fileio: the host's file readers and writers lent to a plugin: line
 * and chunk readers over plain and gzip files, a gzip line writer, the parallel
 * sharded writer, whole-file gzip compress and decompress, and the file
 * helpers. Optional service group, fetched via
 * dftu_plugin_host::get_service(DFTU_SVC_FILEIO). Include
 * dftracer/utils/plugins/abi.h rather than this file directly.
 */

#include <dftracer/utils/plugins/abi/core.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFTU_SVC_FILEIO "dftu.svc.fileio"

typedef struct dftu_line_reader dftu_line_reader;   /**< host-owned */
typedef struct dftu_chunk_reader dftu_chunk_reader; /**< host-owned */
typedef struct dftu_gz_writer dftu_gz_writer;       /**< host-owned */

/** One batch of lines. `lines[i]` has no free_fn: the bytes are borrowed and
 * valid until the next lines_next or lines_close on the same reader. Line
 * `first_line + i` of the file (1-based) is `lines[i]`, without its '\n'. */
typedef struct dftu_line_batch {
    const dftu_bytes* lines;
    uint64_t first_line;
    uint32_t count;
} dftu_line_batch;

/** Options of gz_open; 0 in a field is the library default. */
typedef struct dftu_gz_options {
    uint32_t struct_size; /**< sizeof(dftu_gz_options) */
    int32_t level;
    uint64_t member_size; /**< uncompressed bytes per gzip member */
    uint32_t workers;
} dftu_gz_options;

/** What gz_close reports. */
typedef struct dftu_gz_summary {
    uint64_t parts;
    uint64_t members;
    uint64_t lines;
    uint64_t uc_bytes;
    uint64_t c_bytes;
} dftu_gz_summary;

/** Every call takes the caller's `ctx` first. An async call returns its
 * dftu_task in `*out` for the host to await; a result slot or an input buffer
 * the call names must stay valid until the task finishes. A failed task sets
 * the message "<op>: <text>". A path is copied when the call is made, so the
 * plugin's buffer need not outlive the call. Handles (readers, gz writers,
 * writers) are owned by the host: a close call ends one, and a handle the
 * plugin did not close is freed at the end of the fold (an unclosed gz writer
 * is aborted and its files are removed). Calls on one handle must not overlap.
 */
typedef struct dftu_svc_fileio {
    uint32_t struct_size; /**< sizeof(dftu_svc_fileio) */

    /** Concatenates `shards` into `target` and removes the shards. */
    dftu_status (*merge_shards)(dftu_ctx* ctx, void* h, const char* target,
                                const char* const* shards, uint32_t n,
                                dftu_task** out);
    dftu_status (*writer_create)(dftu_ctx* ctx, void* h, const char* path,
                                 uint32_t num_workers, int32_t gzip,
                                 dftu_writer** out);
    dftu_status (*writer_open)(dftu_ctx* ctx, void* h, dftu_writer* w,
                               dftu_task** out);
    /** `data` is borrowed until the task finishes. */
    dftu_status (*writer_chunk)(dftu_ctx* ctx, void* h, dftu_writer* w,
                                uint32_t worker, const void* data, uint64_t len,
                                dftu_task** out);
    /** The task ends the handle, even when it fails, so `w` must not be used
     * after it. */
    dftu_status (*writer_close)(dftu_ctx* ctx, void* h, dftu_writer* w,
                                dftu_task** out);

    /** Opens `path` for lines `start_line` to `end_line` (1-based, inclusive;
     * `start_line` 0 is the first line and `end_line` 0 is the last). A gzip
     * file (magic 1f 8b) is inflated; any other file is read as it is. A gzip
     * file whose last member is cut short gives its complete lines and no
     * error. A missing file is DFTU_E_NOT_FOUND. Synchronous. */
    dftu_status (*lines_open)(dftu_ctx* ctx, void* h, const char* path,
                              uint64_t start_line, uint64_t end_line,
                              dftu_line_reader** out);
    /** Reads up to `max_lines` lines into `*out` (0 is 1024). The task status
     * is DFTU_OK with `out->count` lines, or DFTU_END with none at the end of
     * the file. `*out` must stay valid until the task finishes. */
    dftu_status (*lines_next)(dftu_ctx* ctx, void* h, dftu_line_reader* r,
                              uint32_t max_lines, dftu_line_batch* out,
                              dftu_task** task);
    void (*lines_close)(void* h, dftu_line_reader* r);

    /** Opens `path` for reading in chunks of `chunk_size` bytes (0 is 64 KiB).
     * Synchronous; a missing file fails the first chunks_next with
     * DFTU_E_NOT_FOUND. */
    dftu_status (*chunks_open)(dftu_ctx* ctx, void* h, const char* path,
                               uint64_t chunk_size, dftu_chunk_reader** out);
    /** Reads the next chunk into `*out` (no free_fn; borrowed until the next
     * chunks_next or chunks_close). The task status is DFTU_END at the end of
     * the file. `*out` must stay valid until the task finishes. */
    dftu_status (*chunks_next)(dftu_ctx* ctx, void* h, dftu_chunk_reader* r,
                               dftu_bytes* out, dftu_task** task);
    void (*chunks_close)(void* h, dftu_chunk_reader* r);

    /** Starts a gzip line writer for `path`. `*out` is set when the call
     * returns, but the writer is usable only after the task finished OK.
     * `opts` may be NULL (all defaults). */
    dftu_status (*gz_open)(dftu_ctx* ctx, void* h, const char* path,
                           const dftu_gz_options* opts, dftu_gz_writer** out,
                           dftu_task** task);
    /** Appends whole lines; the host copies `data`. A non-empty `data` must end
     * with '\n', else DFTU_E_INVALID. Synchronous; it may wait for a free
     * slot. */
    dftu_status (*gz_append)(dftu_ctx* ctx, void* h, dftu_gz_writer* w,
                             const void* data, uint64_t len);
    /** Finishes the file. The task ends the handle, even when it fails, so
     * `w` must not be used after it. `*out` must stay valid until the task
     * finishes. */
    dftu_status (*gz_close)(dftu_ctx* ctx, void* h, dftu_gz_writer* w,
                            dftu_gz_summary* out, dftu_task** task);

    /** Whole-file gzip. `level` 0 and `member_size` 0 are the defaults.
     * `*out_size` receives the output size in bytes. A missing input is
     * DFTU_E_NOT_FOUND. */
    dftu_status (*compress)(dftu_ctx* ctx, void* h, const char* in_path,
                            const char* out_path, int32_t level,
                            uint64_t member_size, uint64_t* out_size,
                            dftu_task** task);
    dftu_status (*decompress)(dftu_ctx* ctx, void* h, const char* in_path,
                              const char* out_path, uint64_t* out_size,
                              dftu_task** task);

    /** File helpers, all synchronous; a missing file is DFTU_E_NOT_FOUND. The
     * logical path is the absolute normalized path; `out` is host-owned and
     * freed through its free_fn. `mtime` is in seconds. `sampled_hash` is a
     * fingerprint of the size and the first and last 64 KiB. */
    dftu_status (*logical_path)(dftu_ctx* ctx, void* h, const char* path,
                                dftu_bytes* out);
    dftu_status (*mtime)(dftu_ctx* ctx, void* h, const char* path,
                         int64_t* out_seconds);
    dftu_status (*size)(dftu_ctx* ctx, void* h, const char* path,
                        uint64_t* out);
    dftu_status (*sampled_hash)(dftu_ctx* ctx, void* h, const char* path,
                                uint64_t* out);
} dftu_svc_fileio;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_FILEIO_H */
