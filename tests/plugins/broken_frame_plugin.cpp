// SPDX-License-Identifier: MIT
/* Test-only plugin that hands the host broken results from every callback the
 * host checks.
 *
 * It registers:
 *   - "broken_frame.bad_column", a provider whose one frame carries a column
 *     that claims one null and has no validity bitmap, so a reader believes a
 *     row is null and cannot find which one. The offsets are sound, so nothing
 *     reads past the data when the frame is used unvalidated.
 *   - "broken_frame.bad_udf"(x), a UDF whose exec returns an Int64 column with
 *     the same broken validity.
 *   - "broken_frame.bad_node", a node whose cursor frame is the same broken
 *     frame.
 *   - "broken_frame.bad_format", a file format whose cursor frame is the same
 *     broken frame, for a file that starts with "BFRAME".
 *   - "broken_frame.bad_udaf"(x), an aggregate whose finalize returns the
 *     broken Int64 column.
 *   - "broken_frame.bad_op"(x), a column op whose result is the broken Int64
 *     column.
 *   - a fold whose transform returns the broken frame.
 *
 * The public C constructors refuse a broken column, so the two columns are
 * finished by hand through the private layout behind the dftu_series handle,
 * which is exactly what a plugin with a bug hands back.
 */

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>

#include <cstdint>
#include <cstring>

using namespace dftracer::utils;

namespace {

const char* BAD_NAMES[] = {"name"};

/* Two text rows that claim one null between them, with no bitmap to say
 * which. */
::dftu_series* broken_name_column() {
    static const std::int32_t OFFSETS[3] = {0, 1, 2};
    ::dftu_series* col =
        ::dftu_series_new_string(DFTU_TYPE_STRING, OFFSETS, "ab", 2, nullptr);
    if (col) col->null_count = 1;
    return col;
}

/* ---- broken_frame.bad_column: one frame, then end of stream ---- */

dftu_status bad_schema(dftu_ctx*, void*, const char* const** out_names,
                       std::int32_t* out_count) {
    *out_names = BAD_NAMES;
    *out_count = 1;
    return DFTU_OK;
}

struct BadCursor {
    int call = 0;
};

dftu_status bad_next(dftu_ctx*, void* self, std::int64_t,
                     ::dftu_dataframe** out, dftu_task**) {
    auto* cur = static_cast<BadCursor*>(self);
    if (cur->call++ > 0) {
        *out = nullptr;
        return DFTU_END;
    }
    ::dftu_series* cols[1] = {broken_name_column()};
    *out = ::dftu_dataframe_new(BAD_NAMES, cols, 1);
    return DFTU_OK;
}

void bad_cursor_destroy(void* self) { delete static_cast<BadCursor*>(self); }

const ::dftu_cursor_vt BAD_CURSOR_VT = {.struct_size = sizeof(dftu_cursor_vt),
                                        .next = bad_next,
                                        .destroy = bad_cursor_destroy};

dftu_status bad_scan(dftu_ctx*, void*, const dftu_plugin_host*,
                     const ::dftu_scan_request*, std::int32_t*,
                     void** out_cursor_self, const ::dftu_cursor_vt** out_vt) {
    *out_cursor_self = new BadCursor();
    *out_vt = &BAD_CURSOR_VT;
    return DFTU_OK;
}

void bad_source_destroy(void*) {}

const ::dftu_source_vt BAD_SOURCE_VT = {.struct_size = sizeof(dftu_source_vt),
                                        .schema = bad_schema,
                                        .scan = bad_scan,
                                        .destroy = bad_source_destroy};

/* ---- broken_frame.bad_udf(x): one Int64 column per batch ---- */

/* `rows` values that claim one null between them, with no bitmap. */
::dftu_series* broken_value_column(std::uint64_t rows) {
    dstd::Vector<std::int64_t> values;
    for (std::uint64_t i = 0; i < rows; ++i)
        values.push_back(static_cast<std::int64_t>(i));
    ::dftu_series* col =
        ::dftu_series_new_flat(DFTU_TYPE_INT64, values.data(),
                               static_cast<std::int64_t>(rows), nullptr);
    if (col && rows > 0) col->null_count = 1;
    return col;
}

dftu_status bad_bind(dftu_ctx*, void*, const dftu_type_info*, std::uint32_t,
                     const dftu_scalar* const*, dftu_type_info* out, void**) {
    out->struct_size = sizeof(dftu_type_info);
    out->id = DFTU_TYPE_INT64;
    return DFTU_OK;
}

dftu_status bad_exec(dftu_ctx*, void*, const dftu_series* const*, std::uint32_t,
                     std::uint64_t rows, ::dftu_series** out) {
    *out = broken_value_column(rows);
    return *out == nullptr ? DFTU_E_INVALID : DFTU_OK;
}

const ::dftu_udf_desc BAD_UDF = {sizeof(dftu_udf_desc),
                                 "broken_frame.bad_udf",
                                 0,
                                 1,
                                 1,
                                 bad_bind,
                                 bad_exec,
                                 nullptr};

/* Two text rows, every one of them valid: the input the node and format
 * cursors are pulled through. */
::dftu_series* good_name_column() {
    static const std::int32_t OFFSETS[3] = {0, 1, 2};
    return ::dftu_series_new_string(DFTU_TYPE_STRING, OFFSETS, "ab", 2,
                                    nullptr);
}

struct GoodCursor {
    int call = 0;
};

dftu_status good_next(dftu_ctx*, void* self, std::int64_t,
                      ::dftu_dataframe** out, dftu_task**) {
    auto* cur = static_cast<GoodCursor*>(self);
    if (cur->call++ > 0) {
        *out = nullptr;
        return DFTU_END;
    }
    ::dftu_series* cols[1] = {good_name_column()};
    *out = ::dftu_dataframe_new(BAD_NAMES, cols, 1);
    return DFTU_OK;
}

void good_cursor_destroy(void* self) { delete static_cast<GoodCursor*>(self); }

const ::dftu_cursor_vt GOOD_CURSOR_VT = {.struct_size = sizeof(dftu_cursor_vt),
                                         .next = good_next,
                                         .destroy = good_cursor_destroy};

dftu_status good_scan(dftu_ctx*, void*, const dftu_plugin_host*,
                      const ::dftu_scan_request*, std::int32_t*,
                      void** out_cursor_self, const ::dftu_cursor_vt** out_vt) {
    *out_cursor_self = new GoodCursor();
    *out_vt = &GOOD_CURSOR_VT;
    return DFTU_OK;
}

const ::dftu_source_vt GOOD_SOURCE_VT = {.struct_size = sizeof(dftu_source_vt),
                                         .schema = bad_schema,
                                         .scan = good_scan,
                                         .destroy = bad_source_destroy};

/* ---- broken_frame.bad_node: the broken frame behind a node cursor ---- */

dftu_status declare_name(dftu_ctx* ctx, ::dftu_schema* out) {
    if (::dftu_schema_add_field(out, "name", DFTU_TYPE_STRING, 1,
                                DFTU_TIME_UNIT_SECOND, nullptr, 0, 0, 0) < 0)
        return dftu_fail(ctx, DFTU_E_INVALID, "cannot declare the column");
    return DFTU_OK;
}

dftu_status node_schema(dftu_ctx* ctx, void*, const ::dftu_schema*,
                        const ::dftu_op_arg*, ::dftu_schema* out) {
    return declare_name(ctx, out);
}

struct NodeCursor {
    const ::dftu_cursor_vt* in_vt;
    void* in_self;
    int call = 0;
};

dftu_status node_next(dftu_ctx*, void* self, std::int64_t,
                      ::dftu_dataframe** out, dftu_task**) {
    auto* cur = static_cast<NodeCursor*>(self);
    if (cur->call++ > 0) {
        *out = nullptr;
        return DFTU_END;
    }
    ::dftu_series* cols[1] = {broken_name_column()};
    *out = ::dftu_dataframe_new(BAD_NAMES, cols, 1);
    return DFTU_OK;
}

void node_cursor_destroy(void* self) {
    auto* cur = static_cast<NodeCursor*>(self);
    if (cur->in_vt != nullptr && cur->in_vt->destroy != nullptr)
        cur->in_vt->destroy(cur->in_self);
    delete cur;
}

const ::dftu_cursor_vt NODE_CURSOR_VT = {.struct_size = sizeof(dftu_cursor_vt),
                                         .next = node_next,
                                         .destroy = node_cursor_destroy};

dftu_status node_open(dftu_ctx*, void*, const ::dftu_plugin_host*,
                      void* in_self, const ::dftu_cursor_vt* in_vt,
                      const ::dftu_op_arg*, std::uint64_t, void** out_self,
                      const ::dftu_cursor_vt** out_vt) {
    *out_self = new NodeCursor{in_vt, in_self};
    *out_vt = &NODE_CURSOR_VT;
    return DFTU_OK;
}

const ::dftu_node_vt BAD_NODE_VT = {.struct_size = sizeof(dftu_node_vt),
                                    .output_schema = node_schema,
                                    .open = node_open,
                                    .destroy = nullptr};

/* ---- broken_frame.bad_format: the broken frame behind a file cursor ---- */

std::int32_t format_probe(void*, const std::uint8_t* head, std::uint64_t n,
                          const char*) {
    static const char MAGIC[] = "BFRAME";
    if (n < sizeof(MAGIC) - 1) return 0;
    return std::memcmp(head, MAGIC, sizeof(MAGIC) - 1) == 0 ? 100 : 0;
}

dftu_status format_open(dftu_ctx*, void*, const char*, void** file) {
    static int handle = 0;
    *file = &handle;
    return DFTU_OK;
}

dftu_status format_schema(dftu_ctx* ctx, void*, ::dftu_schema* out) {
    return declare_name(ctx, out);
}

dftu_status format_chunks(dftu_ctx*, void*, std::uint64_t* count) {
    *count = 1;
    return DFTU_OK;
}

dftu_status format_scan(dftu_ctx*, void*, std::uint64_t,
                        const ::dftu_plugin_host*, const ::dftu_scan_request*,
                        std::int32_t*, void** out_cursor_self,
                        const ::dftu_cursor_vt** out_vt) {
    *out_cursor_self = new BadCursor();
    *out_vt = &BAD_CURSOR_VT;
    return DFTU_OK;
}

const ::dftu_file_format_vt BAD_FORMAT_VT = {
    .struct_size = sizeof(dftu_file_format_vt),
    .extensions = nullptr,
    .probe = format_probe,
    .open = format_open,
    .schema = format_schema,
    .chunks = format_chunks,
    .chunk_stats = nullptr,
    .scan = format_scan,
    .close = nullptr,
    .destroy = nullptr};

/* ---- broken_frame.bad_udaf(x): finalize hands back the broken column ---- */

struct BadAgg {
    std::int64_t total;
};

dftu_status udaf_bind(dftu_ctx*, void*, const dftu_type_info*, std::uint32_t,
                      const dftu_scalar* const*, dftu_type_info* out, void**) {
    out->struct_size = sizeof(dftu_type_info);
    out->id = DFTU_TYPE_INT64;
    return DFTU_OK;
}

void udaf_init(void*, void* const* states, std::uint64_t n) {
    for (std::uint64_t i = 0; i < n; ++i)
        static_cast<BadAgg*>(states[i])->total = 0;
}

dftu_status udaf_update(dftu_ctx*, void*, void* const* states,
                        const ::dftu_series* const* args, std::uint32_t,
                        std::uint64_t rows) {
    const auto* in =
        static_cast<const std::int64_t*>(::dftu_series_data(args[0]));
    for (std::uint64_t i = 0; i < rows; ++i) {
        auto* state = static_cast<BadAgg*>(states[i]);
        if (state != nullptr && in != nullptr) state->total += in[i];
    }
    return DFTU_OK;
}

dftu_status udaf_combine(dftu_ctx*, void*, void* const* into, void* const* from,
                         std::uint64_t n) {
    for (std::uint64_t i = 0; i < n; ++i)
        static_cast<BadAgg*>(into[i])->total +=
            static_cast<const BadAgg*>(from[i])->total;
    return DFTU_OK;
}

dftu_status udaf_finalize(dftu_ctx*, void*, void* const*, std::uint64_t n,
                          ::dftu_series** out) {
    *out = broken_value_column(n);
    return *out == nullptr ? DFTU_E_INVALID : DFTU_OK;
}

const ::dftu_udaf_desc BAD_UDAF = {sizeof(dftu_udaf_desc),
                                   "broken_frame.bad_udaf",
                                   DFTU_AGG_STATE_POD,
                                   1,
                                   1,
                                   sizeof(BadAgg),
                                   8,
                                   udaf_bind,
                                   udaf_init,
                                   udaf_update,
                                   udaf_combine,
                                   udaf_finalize,
                                   nullptr,
                                   nullptr,
                                   nullptr,
                                   nullptr,
                                   nullptr,
                                   nullptr};

/* ---- broken_frame.bad_op(x): the broken column as an op result ---- */

::dftu_series* bad_op(const ::dftu_series* in) {
    return broken_value_column(
        in == nullptr ? 1 : static_cast<std::uint64_t>(in->length));
}

const ::dftu_op_desc BAD_OP = {"broken_frame.bad_op",
                               DFTU_OP_SIG(SERIES, SERIES, NONE, NONE),
                               reinterpret_cast<::dftu_op_generic_fn>(&bad_op)};

/* ---- the fold: its transform returns the broken frame ---- */

dftu_status make_slice(dftu_ctx*, void*, void** out) {
    *out = new int(0);
    return DFTU_OK;
}

void destroy_slice(void* slice) { delete static_cast<int*>(slice); }

dftu_status merge(dftu_ctx*, void*, void*) { return DFTU_OK; }

dftu_status on_finalize(dftu_ctx*, void*, const ::dftu_plugin_host*,
                        dftu_task**) {
    return DFTU_OK;
}

dftu_status transform(dftu_ctx*, void*, const ::dftu_dataframe*,
                      const ::dftu_plugin_host*, ::dftu_dataframe** out) {
    ::dftu_series* cols[1] = {broken_name_column()};
    *out = ::dftu_dataframe_new(BAD_NAMES, cols, 1);
    return *out == nullptr ? DFTU_E_INVALID : DFTU_OK;
}

/* ---- the descriptor: six registrations and a fold ---- */

::dftu_plugin g_plugin;

void destroy(void* self) { (void)self; }

}  // namespace

// Raw C entry on purpose: it hands the host malformed and wrongly-sized C
// frame and column handles, because the host frame checks are what this
// fixture asserts.
DFTU_PLUGIN_INFO("broken_frame", "1.0")

DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value*,
                                               dftu_plugin** out) {
    const auto* providers = static_cast<const dftu_svc_providers*>(
        h->get_service(h->h, DFTU_SVC_PROVIDERS));
    if (!providers || !providers->register_provider)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no provider registry");
    const auto* ops =
        static_cast<const dftu_svc_ops*>(h->get_service(h->h, DFTU_SVC_OPS));
    if (!ops || !ops->register_udf)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no udf registry");

    const auto* nodes = static_cast<const dftu_svc_nodes*>(
        h->get_service(h->h, DFTU_SVC_NODES));
    if (!nodes || !nodes->register_node)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no node registry");
    const auto* formats = static_cast<const dftu_svc_file_formats*>(
        h->get_service(h->h, DFTU_SVC_FILE_FORMATS));
    if (!formats || !formats->register_format)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no file format registry");
    if (!ops->register_udaf)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no udaf registry");
    if (!ops->register_op)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no op registry");

    dftu_status st = providers->register_provider(
        ctx, h->h, "broken_frame.good_column", &GOOD_SOURCE_VT, nullptr);
    if (st < 0) return st;
    st = providers->register_provider(ctx, h->h, "broken_frame.bad_column",
                                      &BAD_SOURCE_VT, nullptr);
    if (st < 0) return st;
    st = ops->register_udf(ctx, h->h, &BAD_UDF, nullptr);
    if (st < 0) return st;
    st = nodes->register_node(ctx, h->h, "broken_frame.bad_node", &BAD_NODE_VT,
                              nullptr);
    if (st < 0) return st;
    st = formats->register_format(ctx, h->h, "broken_frame.bad_format",
                                  &BAD_FORMAT_VT, nullptr);
    if (st < 0) return st;
    st = ops->register_udaf(ctx, h->h, &BAD_UDAF, nullptr);
    if (st < 0) return st;
    st = ops->register_op(ctx, h->h, &BAD_OP);
    if (st < 0) return st;

    std::memset(&g_plugin, 0, sizeof(g_plugin));
    g_plugin.struct_size = sizeof(g_plugin);
    g_plugin.make_slice = make_slice;
    g_plugin.merge = merge;
    g_plugin.on_finalize = on_finalize;
    g_plugin.destroy_slice = destroy_slice;
    g_plugin.transform = transform;
    g_plugin.destroy = destroy;
    *out = &g_plugin;
    return DFTU_OK;
}
