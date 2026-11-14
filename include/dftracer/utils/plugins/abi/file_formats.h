// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_ABI_FILE_FORMATS_H
#define DFTRACER_UTILS_PLUGINS_ABI_FILE_FORMATS_H

/** @file
 * dftu.svc.file_formats: lets a plugin claim a file format, so
 * dftracer::utils::dataframe::LazyFrame::scan_file (lazyframe.h) can read a
 * file the engine has no reader for, and register a sink that writes frames
 * out. Registration is a build-phase concern: call register_format and
 * register_sink from the plugin factory, the same phase
 * dftu_svc_providers::register_provider runs in. Include
 * dftracer/utils/plugins/abi.h rather than this file directly.
 *
 * A registered dftu_file_format_vt (defined below) is a C mirror of the C++
 * dftracer::utils::dataframe::FileFormatFactory (file_format_registry.h):
 * probe() scores a file head without opening it, open() opens it, schema()
 * and chunks() describe it without scanning, and scan() opens a
 * dftu_cursor_vt over one independently scannable chunk, honoring a
 * dftu_scan_request exactly as dftu_source_vt::scan does. A sink is the
 * writing side: bind() fixes the output columns and options, then write()
 * takes whole dataframes until finalize() or abort().
 *
 * The sinks are declared here and registered by the host; nothing calls them
 * yet.
 */

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/plugins/abi/core.h>
#include <dftracer/utils/plugins/abi/providers.h>
#include <dftracer/utils/plugins/abi/value.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFTU_SVC_FILE_FORMATS "dftu.svc.file_formats"

/** What one chunk costs to read, for planning. */
typedef struct dftu_chunk_stats {
    uint32_t struct_size; /**< sizeof(dftu_chunk_stats) */
    /** Rows in this chunk, or -1 when the format cannot know without reading
       it. */
    int64_t rows;
} dftu_chunk_stats;

/** One file format a plugin claims. `self` is the plugin's own state for the
   format and lives until destroy(); a `file` handle below is one open file on
   it, so one format may hold many files open at once. */
typedef struct dftu_file_format_vt {
    uint32_t struct_size; /**< sizeof(dftu_file_format_vt) */
    /** Optional (NULL is fine). A NULL-terminated array of file extensions
       ("jsonl", "pf", ...) owned by the plugin and valid for its lifetime.
       ADVISORY: a hint for matching and for diagnostics, never consulted
       instead of probe(), so a format that recognizes nothing by extension
       leaves this NULL. */
    const char* const* extensions;
    /** Score how well `head[0..n)` matches this format, 0 (not mine) to 100
       (certain), or a negative value for "not mine" - the host clamps it into
       0..100. `head` is the first bytes of the file, borrowed for the call and
       never reallocated, and may be shorter than the file or empty. `path` is
       the file's path, borrowed for the call, for a format that decides by
       name. No plugin callback runs on a probe: it must not allocate through
       the host, and it reports nothing. The highest score wins; a tie at the
       top is an error the host reports, never a first-registered fallback.
       Called without any ctx, from any thread. */
    int32_t (*probe)(void* self, const uint8_t* head, uint64_t n,
                     const char* path);
    /** Open `path` and set `*file` to this format's handle for it; the
       handle is not a file descriptor and only this vtable's callbacks see
       it. The host closes it with close() exactly once when the last plan or
       cursor holding it is gone. On failure nothing is opened, `*file` is
       untouched, and the status and its ctx message are the error the host
       reports. */
    dftu_status (*open)(dftu_ctx* ctx, void* self, const char* path,
                        void** file);
    /** Append this file's columns, in order, to `out` - an empty dftu_schema
       the host allocates and frees around this one call (see
       dftu_schema_add_field). The host requires a non-empty schema: a file
       with no columns is an error. Called once per open, before any scan. */
    dftu_status (*schema)(dftu_ctx* ctx, void* file, dftu_schema* out);
    /** Set `*count` to the number of independently scannable chunks of the
       file, at least 1. The host calls scan() once per chunk; a format with no
       useful split reports 1 and reads the whole file in one scan. Called once
       per open, before any scan. */
    dftu_status (*chunks)(dftu_ctx* ctx, void* file, uint64_t* count);
    /** Optional (NULL means no statistics). Fill `*out`, whose struct_size the
       host sets, with what chunk `chunk` costs to read. The host does not
       call it yet; it is a hint for a planner, so a format may leave it
       NULL. */
    dftu_status (*chunk_stats)(dftu_ctx* ctx, void* file, uint64_t chunk,
                               dftu_chunk_stats* out);
    /** Open a cursor over chunk `chunk` honoring `req`, exactly as
       dftu_source_vt::scan does: `host` belongs to the cursor and stays valid
       until its destroy() returns, `out_pushed` has req->n_filters entries
       pre-filled DFTU_PUSHED_NO, and *out_cursor_self plus *out_vt are the
       cursor and its vtable, which must outlive it. `file` is borrowed and
       stays valid until close(). The projection rule is the same: a non-empty
       req->projection means every dftu_dataframe the cursor produces carries
       exactly those columns, in that order. The host requires every chunk of
       one scan to report the same pushed votes: the first chunk's votes are
       the scan's, and a later chunk that votes differently fails the scan. */
    dftu_status (*scan)(dftu_ctx* ctx, void* file, uint64_t chunk,
                        const dftu_plugin_host* host,
                        const dftu_scan_request* req, int32_t* out_pushed,
                        void** out_cursor_self, const dftu_cursor_vt** out_vt);
    /** Close one handle from open(). Called once, after every cursor scanning
       it is destroyed; may be NULL for a format that holds nothing open. */
    void (*close)(void* file);
    /** Release the format's `self`; called once, after every registration is
       gone and every file it opened is closed. */
    void (*destroy)(void* self);
} dftu_file_format_vt;

/** A writer a plugin registers under a name: it takes whole dataframes and
   writes them out in its own format. `self` is the plugin's state for the
   sink and lives until destroy(); a `sink` below is one bound output. */
typedef struct dftu_sink_vt {
    uint32_t struct_size; /**< sizeof(dftu_sink_vt) */
    /** Bind one output to the `n_columns` columns named `names` (each
       NUL-terminated, borrowed for the call) of types `types` (dftu_dtype
       codes, same order, borrowed) and set `*sink` to this sink's handle.
       `options` is a borrowed dftu_value OBJECT whose member values are all
       DFTU_VAL_STR strings (a tree the host owns for this one call), or NULL
       when the caller gave none; `n_columns` may be 0 for a sink that decides
       its own columns. On failure nothing is bound and the status with its ctx
       message is the error. The host calls finalize() or abort() on the handle
       exactly once, and that call releases it: no other callback sees it
       afterwards. */
    dftu_status (*bind)(dftu_ctx* ctx, void* self, const char* const* names,
                        const int32_t* types, uint32_t n_columns,
                        const dftu_value* options, void** sink);
    /** Write one frame. `batch` is borrowed for the call only: it and its
       columns are freed when this returns, so a sink that keeps the data
       copies it. Every frame of one output carries bind()'s columns, in that
       order. An error ends the output; the host calls abort(). */
    dftu_status (*write)(dftu_ctx* ctx, void* sink,
                         const dftu_dataframe* batch);
    /** Finish the output: flush and close it, and release the handle. Called
       once, after the last write(). */
    dftu_status (*finalize)(dftu_ctx* ctx, void* sink);
    /** Give up on the output: release the handle and anything written, and
       report nothing. Called once instead of finalize() after a failed write
       or a cancelled run, and never beside it. */
    void (*abort)(void* sink);
    /** Release the sink's `self`; called once, after every output bound from it
       is finished. */
    void (*destroy)(void* self);
} dftu_sink_vt;

/** Build-phase registry of file formats and sinks. Fetched via
   dftu_plugin_host::get_service(DFTU_SVC_FILE_FORMATS); the run-time host
   (inside a fold callback or a cursor) still answers this group so a plugin
   sees a consistent service surface, but refuses register_format and
   register_sink there - by the time a scan runs, the set of formats a
   scan_file can name is already settled. */
typedef struct dftu_svc_file_formats {
    uint32_t struct_size; /**< sizeof(dftu_svc_file_formats) */
    /** Register `vt`/`self` as a file format under `name`. `name` must be
       `<plugin>.<name>` (the bare and "dftu." namespaces are host-reserved,
       the same rule dftu_svc_providers::register_provider enforces); `vt` is
       copied, so it need not outlive the call, but `self` must outlive every
       file and scan the host later opens on it. Fails with DFTU_E_INVALID if
       `name` is NULL or empty, `vt` is NULL, `vt->struct_size` is not
       sizeof(dftu_file_format_vt), a required callback of `vt` is NULL, `name`
       fails the namespace rule, or the name is already registered - a second
       registration under the same name is refused, never a silent replace. */
    dftu_status (*register_format)(dftu_ctx* ctx, void* h, const char* name,
                                   const dftu_file_format_vt* vt, void* self);
    /** Register `vt`/`self` as a sink under `name`, with the same name rule
       and the same refusals as register_format. Nothing calls a registered
       sink yet. */
    dftu_status (*register_sink)(dftu_ctx* ctx, void* h, const char* name,
                                 const dftu_sink_vt* vt, void* self);
} dftu_svc_file_formats;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_FILE_FORMATS_H */
