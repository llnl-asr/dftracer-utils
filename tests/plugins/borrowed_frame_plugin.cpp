// SPDX-License-Identifier: MIT
/* Test-only plugin whose frame op hands the host a frame of columns that
 * borrow this library's own buffers: one column built with the C ABI call
 * dftu_series_new_flat_borrowed and one with the SDK's Column/FrameBuilder.
 * Every release callback appends 'r' to the file named by
 * BORROWED_FRAME_PLUGIN_MARKER, and the plugin's own destroy appends 'p', so
 * the test can see when each ran relative to the library closing.
 */

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/plugin/frame.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

namespace {

void mark(char c) {
    const char* path = std::getenv("BORROWED_FRAME_PLUGIN_MARKER");
    std::FILE* f = path ? std::fopen(path, "ab") : nullptr;
    if (f == nullptr) return;
    std::fputc(c, f);
    std::fclose(f);
}

void release_marked(void* ctx) {
    mark('r');
    std::free(ctx);
}

const char* const NAMES[] = {"value"};

dftu_dataframe* abi_frame(const dftu_dataframe*) {
    auto* values =
        static_cast<std::int64_t*>(std::malloc(3 * sizeof(std::int64_t)));
    if (values == nullptr) return nullptr;
    values[0] = 7;
    values[1] = 8;
    values[2] = 9;
    dftu_series* column = dftu_series_new_flat_borrowed(
        DFTU_TYPE_INT64, values, 3, nullptr, release_marked, values);
    if (column == nullptr) {
        std::free(values);
        return nullptr;
    }
    return dftu_dataframe_new(NAMES, &column, 1);
}

dftu_dataframe* sdk_frame(const dftu_dataframe*) {
    dstd::Vector<std::int64_t> values{4, 5, 6};
    FrameBuilder builder;
    builder.add("value", Column::from_int64(std::move(values)));
    auto built = builder.build();
    if (!built) return nullptr;
    return built->release();
}

const dftu_op_desc ABI_OP = {"borrowed_frame_plugin.abi",
                             DFTU_OP_SIG(FRAME, FRAME, NONE, NONE),
                             (dftu_op_generic_fn)&abi_frame};

const dftu_op_desc SDK_OP = {"borrowed_frame_plugin.sdk",
                             DFTU_OP_SIG(FRAME, FRAME, NONE, NONE),
                             (dftu_op_generic_fn)&sdk_frame};

void destroy(void* self) {
    (void)self;
    mark('p');
}

dftu_plugin g_plugin;

}  // namespace

// Raw C entry on purpose: it registers through the raw dftu_svc_ops
// struct and returns a hand-built descriptor with no fold, because that
// plugin shape and its borrowed column are what this fixture asserts.
DFTU_PLUGIN_INFO("borrowed_frame_plugin", "1.0")

extern "C" DFTU_PLUGIN_EXPORT dftu_status
dftracer_plugin(dftu_ctx* ctx, dftu_plugin_host* h, const dftu_value* config,
                dftu_plugin** out) {
    (void)config;
    auto* ops = (const dftu_svc_ops*)h->get_service(h->h, DFTU_SVC_OPS);
    if (ops == nullptr)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no op registry at load");
    if (ops->register_op(ctx, h->h, &ABI_OP) < 0) return DFTU_E_INVALID;
    if (ops->register_op(ctx, h->h, &SDK_OP) < 0) return DFTU_E_INVALID;
    g_plugin.struct_size = sizeof(g_plugin);
    g_plugin.destroy = destroy;
    *out = &g_plugin;
    return DFTU_OK;
}
