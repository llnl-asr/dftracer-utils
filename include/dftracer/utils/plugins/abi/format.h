// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_ABI_FORMAT_H
#define DFTRACER_UTILS_PLUGINS_ABI_FORMAT_H

/** @file
 * dftu.svc.json and dftu.svc.yaml: the host's JSON and YAML readers and writers
 * lent to a plugin. Optional service group, fetched via
 * dftu_plugin_host::get_service with DFTU_SVC_JSON or DFTU_SVC_YAML; both ids
 * give a dftu_svc_format. Include dftracer/utils/plugins/abi.h rather than this
 * file directly. The calls are synchronous. Per-event JSON stays with the
 * host's columns.
 */

#include <dftracer/utils/plugins/abi/core.h>
#include <dftracer/utils/plugins/abi/value.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFTU_SVC_JSON "dftu.svc.json"
#define DFTU_SVC_YAML "dftu.svc.yaml"

/** A parsed document; owns the tree that `root` returns. */
typedef struct dftu_doc dftu_doc;

/** A streaming writer that builds one document in memory. */
typedef struct dftu_format_writer dftu_format_writer;

/** Every failure sets its message on `ctx` and returns the status. A call on a
 * document or a writer needs the `ctx` of the calling thread, which may be
 * NULL where a call takes none. Strings are given as a pointer and a byte
 * length and need no NUL.
 *
 * Reading: a document root is an object or an array in JSON; YAML also accepts
 * a scalar root. A JSON number is I64 when it fits int64, else F64; an integer
 * wider than 64 bits is a parse error. A YAML scalar is untyped text, so it is
 * STR, and NULL for an empty value, `~` and a bare `null`. A parse error
 * returns DFTU_E_PARSE and the message `line:column: reason` with 1-based line
 * and byte column. A refusal that the host cannot place has a message with no
 * position: nesting past 1000 levels in a tree that the reader accepted, a
 * string over 4 GiB, and a JSON text that only the reader refuses. A string
 * keeps an embedded NUL, and its `count` is its byte length.
 *
 * Writing: a call that breaks the grammar (a value in an object without a key,
 * a key outside an object or two keys in a row, an end with nothing open or of
 * the other container kind, a second root, finish with a container open or with
 * nothing written, a non-finite double in JSON, a YAML key whose written form
 * is over 1022 bytes) returns DFTU_E_INVALID and leaves the writer unchanged. A
 * YAML double that is not finite is written as `.nan`, `.inf` or `-.inf`. A
 * string is escaped by the host. `raw` appends one scalar token that the caller
 * guarantees is valid in the format. */
typedef struct dftu_svc_format {
    uint32_t struct_size; /**< sizeof(dftu_svc_format) */
    /** Parses `len` bytes of `text` into `*out`. The host copies the text, so
       it need not outlive the call. A document is freed with doc_free. */
    dftu_status (*parse)(dftu_ctx* ctx, void* h, const char* text, uint64_t len,
                         dftu_doc** out);
    /** The root value, built when the document was parsed. It and every value,
       key and string below it are valid until doc_free. NULL doc gives NULL. */
    const dftu_value* (*root)(const dftu_doc* doc);
    /** Frees a document and its tree; NULL is allowed. */
    void (*doc_free)(dftu_doc* doc);
    /** Starts a writer in `*out`; free it with writer_free. */
    dftu_status (*writer_new)(dftu_ctx* ctx, void* h, dftu_format_writer** out);
    dftu_status (*begin_object)(dftu_ctx* ctx, dftu_format_writer* w);
    dftu_status (*end_object)(dftu_ctx* ctx, dftu_format_writer* w);
    dftu_status (*begin_array)(dftu_ctx* ctx, dftu_format_writer* w);
    dftu_status (*end_array)(dftu_ctx* ctx, dftu_format_writer* w);
    /** The name of the next member of the open object. */
    dftu_status (*key)(dftu_ctx* ctx, dftu_format_writer* w, const char* k,
                       uint64_t len);
    dftu_status (*i64)(dftu_ctx* ctx, dftu_format_writer* w, int64_t v);
    dftu_status (*f64)(dftu_ctx* ctx, dftu_format_writer* w, double v);
    dftu_status (*str)(dftu_ctx* ctx, dftu_format_writer* w, const char* s,
                       uint64_t len);
    dftu_status (*boolean)(dftu_ctx* ctx, dftu_format_writer* w, int32_t v);
    dftu_status (*null)(dftu_ctx* ctx, dftu_format_writer* w);
    dftu_status (*raw)(dftu_ctx* ctx, dftu_format_writer* w, const char* token,
                       uint64_t len);
    /** Ends the document and gives its text in `*out`, which the plugin
       releases with `out->free_fn(out->data, out->ud)` exactly once. The
       writer is spent after a finish that succeeds: only writer_free is valid.
     */
    dftu_status (*finish)(dftu_ctx* ctx, dftu_format_writer* w,
                          dftu_bytes* out);
    /** Frees a writer and its text; NULL is allowed. */
    void (*writer_free)(dftu_format_writer* w);
} dftu_svc_format;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_FORMAT_H */
