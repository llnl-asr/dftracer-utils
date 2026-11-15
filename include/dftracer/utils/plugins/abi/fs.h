// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_ABI_FS_H
#define DFTRACER_UTILS_PLUGINS_ABI_FS_H

/** @file
 * dftu.svc.fs: the runtime file system layer lent to a plugin. Optional service
 * group, fetched via dftu_plugin_host::get_service(DFTU_SVC_FS). Include
 * dftracer/utils/plugins/abi.h rather than this file directly.
 */

#include <dftracer/utils/plugins/abi/core.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFTU_SVC_FS "dftu.svc.fs"

/** What a directory entry or a stat reports; the values match
 * dstd::fs::FileKind. */
typedef int32_t dftu_file_kind;
enum {
    DFTU_FILE_UNKNOWN = 0,
    DFTU_FILE_FILE = 1,
    DFTU_FILE_DIRECTORY = 2,
    DFTU_FILE_SYMLINK = 3,
    DFTU_FILE_OTHER = 4
};

/** The entries of one list_dir or find_all; host-owned until list_free. */
typedef struct dftu_path_list dftu_path_list;

/** One entry of a dftu_path_list. `path` is NUL-terminated, `path_len` bytes
 * long without the NUL, and valid until list_free. `size` and `mtime_ns` are 0
 * where the call did not stat. */
typedef struct dftu_path_entry {
    const char* path;
    uint64_t size;
    uint64_t mtime_ns;
    uint32_t path_len;
    dftu_file_kind kind;
} dftu_path_entry;

/** The options of find_all, as the runtime's FindOptions names them. A NULL
 * options pointer is the defaults: every file, recursive, with stat. A NULL
 * `patterns` or `skip_dirs` is an empty list; no patterns finds every file.
 * `limit` is the stat calls in flight at most, 0 for the executor's
 * concurrency. The host copies every string. */
typedef struct dftu_find_options {
    uint32_t struct_size; /**< sizeof(dftu_find_options) */
    uint32_t pattern_count;
    const char* const* patterns;
    const char* const* skip_dirs;
    uint64_t limit;
    uint32_t skip_dir_count;
    int32_t recursive;
    int32_t with_stat;
    int32_t skip_unreadable;
} dftu_find_options;

/** What stat_batch reports for one path, which it follows if it is a symlink;
 * `mtime_ns` is nanoseconds since the epoch. */
typedef struct dftu_fs_stat {
    uint64_t size;
    uint64_t mtime_ns;
    uint64_t dev;
    uint64_t ino;
    uint32_t mode;
    dftu_file_kind kind;
} dftu_fs_stat;

/** Each call that takes a `dftu_task** out` starts an operation and returns its
 * task for the host to await. The host copies every path it gets, so a path
 * need not outlive the call. A result slot, and a buffer or an array a call
 * names (`data`, `buf`, `paths`, `out`, `out_status`), must stay valid until
 * the task finishes. A failed operation finishes its task with the status of
 * its error (DFTU_E_NOT_FOUND for a missing path, DFTU_E_IO and the others)
 * and the message "<op>: <text>". A path may carry a scheme such as `s3://`
 * that a registered file system serves, except for exists, create_directories
 * and remove, which take local paths only. */
typedef struct dftu_svc_fs {
    uint32_t struct_size; /**< sizeof(dftu_svc_fs) */
    /** The whole file. `out` is filled on success; the plugin releases it with
       `out->free_fn(out->data, out->ud)` exactly once. */
    dftu_status (*read_file)(dftu_ctx* ctx, void* h, const char* path,
                             dftu_bytes* out, dftu_task** task);
    /** Creates or truncates the file and writes `len` bytes of `data`. */
    dftu_status (*write_file)(dftu_ctx* ctx, void* h, const char* path,
                              const void* data, uint64_t len, dftu_task** task);
    /** Opens the file, reads up to `len` bytes at `off` into `buf`, closes it.
       out_n receives the byte count, 0 at or past the end of the file. */
    dftu_status (*read_at)(dftu_ctx* ctx, void* h, const char* path,
                           uint64_t off, void* buf, uint64_t len,
                           int64_t* out_n, dftu_task** task);
    /** Opens the file, writes up to `len` bytes of `buf` at `off`, closes it.
       Creates the file if it does not exist. out_n receives
       the byte count. */
    dftu_status (*write_at)(dftu_ctx* ctx, void* h, const char* path,
                            uint64_t off, const void* buf, uint64_t len,
                            int64_t* out_n, dftu_task** task);
    /** The full paths of the entries of one directory, in no set order. `*out`
       is a list the plugin frees with list_free; it is NULL if the task fails.
     */
    dftu_status (*list_dir)(dftu_ctx* ctx, void* h, const char* path,
                            dftu_path_list** out, dftu_task** task);
    /** The regular files below `root`, sorted by path; the list is as for
       list_dir. */
    dftu_status (*find_all)(dftu_ctx* ctx, void* h, const char* root,
                            const dftu_find_options* options,
                            dftu_path_list** out, dftu_task** task);
    /** The number of entries of a list. */
    uint64_t (*list_count)(const dftu_path_list* list);
    /** Fills `out` with entry `i`; DFTU_E_INVALID if `i` is past the end. */
    dftu_status (*list_entry)(dftu_ctx* ctx, const dftu_path_list* list,
                              uint64_t i, dftu_path_entry* out);
    /** Frees a list and every entry path of it; NULL is allowed. */
    void (*list_free)(dftu_path_list* list);
    /** `*out` is 1 if the path exists, else 0. */
    dftu_status (*exists)(dftu_ctx* ctx, void* h, const char* path,
                          int32_t* out, dftu_task** task);
    /** `*out_created` is 1 if it made a directory, 0 if the path was one. */
    dftu_status (*create_directories)(dftu_ctx* ctx, void* h, const char* path,
                                      int32_t* out_created, dftu_task** task);
    /** `*out_removed` is 1 if it removed the path, 0 if it did not exist. */
    dftu_status (*remove)(dftu_ctx* ctx, void* h, const char* path,
                          int32_t* out_removed, dftu_task** task);
    /** One stat in `out[i]` and one status in `out_status[i]` for `paths[i]`,
       for i below `n`. A path that cannot be stat'ed, a missing one included
       (DFTU_E_NOT_FOUND), fails only its own slot, not the task. Every path
       must use the same scheme (one file system), else DFTU_E_INVALID. */
    dftu_status (*stat_batch)(dftu_ctx* ctx, void* h, const char* const* paths,
                              uint64_t n, dftu_fs_stat* out,
                              dftu_status* out_status, dftu_task** task);
} dftu_svc_fs;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_FS_H */
