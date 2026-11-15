// SPDX-License-Identifier: MIT
/* Test-only plugin: a provider that hands the engine a small pid/ts table whose
 * `ts` column is a TIMESTAMP and has one null row, so a node test reaches the
 * timestamp and null paths without a trace file.
 *
 *   pid: 1, 1, 1, 2      ts: 1000, null, 2000, 3000
 */

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/plugins/abi.h>

#include <cstdint>

namespace {

const char* const NAMES[] = {"pid", "ts"};
constexpr std::int32_t N_COLUMNS = 2;

const std::int64_t PIDS[] = {1, 1, 1, 2};
const std::int64_t TIMES[] = {1000, 0, 2000, 3000};
// One bit per row, least significant first: bit 1 (row 1) is clear.
const std::uint8_t TIME_VALIDITY[] = {0x0D};
constexpr std::int64_t ROWS = 4;

int column_index(const char* name) {
    for (int i = 0; i < N_COLUMNS; ++i)
        for (const char *a = name, *b = NAMES[i];; ++a, ++b) {
            if (*a != *b) break;
            if (*a == '\0') return i;
        }
    return -1;
}

::dftu_dataframe* make_frame(const int* order) {
    ::dftu_series* columns[N_COLUMNS] = {nullptr, nullptr};
    for (int i = 0; i < N_COLUMNS; ++i) {
        const int at = order[i];
        columns[i] =
            at == 0 ? dftu_series_new_flat(DFTU_TYPE_INT64, PIDS, ROWS, nullptr)
                    : dftu_series_new_flat(DFTU_TYPE_TIMESTAMP, TIMES, ROWS,
                                           TIME_VALIDITY);
        if (columns[i] == nullptr) {
            for (int k = 0; k < i; ++k) dftu_series_free(columns[k]);
            return nullptr;
        }
    }
    const char* names[N_COLUMNS] = {NAMES[order[0]], NAMES[order[1]]};
    return dftu_dataframe_new(names, columns, N_COLUMNS);
}

dftu_status schema(dftu_ctx*, void*, const char* const** out_names,
                   int32_t* out_count) {
    *out_names = NAMES;
    *out_count = N_COLUMNS;
    return DFTU_OK;
}

struct Cursor {
    /// The requested column order, resolved from the scan request, which is
    /// borrowed for the scan call only.
    int order[N_COLUMNS] = {0, 1};
    bool done = false;
};

dftu_status next(dftu_ctx* ctx, void* self, int64_t, ::dftu_dataframe** out,
                 dftu_task**) {
    auto* cursor = static_cast<Cursor*>(self);
    if (cursor->done) {
        *out = nullptr;
        return DFTU_END;
    }
    cursor->done = true;
    *out = make_frame(cursor->order);
    if (*out == nullptr)
        return dftu_fail(ctx, DFTU_E_INTERNAL,
                         "sessionize_input: cannot build a frame");
    return DFTU_OK;
}

void cursor_destroy(void* self) { delete static_cast<Cursor*>(self); }

const ::dftu_cursor_vt CURSOR_VT = {.struct_size = sizeof(dftu_cursor_vt),
                                    .next = next,
                                    .destroy = cursor_destroy};

dftu_status scan(dftu_ctx* ctx, void*, const ::dftu_plugin_host*,
                 const ::dftu_scan_request* request, int32_t*,
                 void** out_cursor_self, const ::dftu_cursor_vt** out_vt) {
    auto* cursor = new Cursor();
    // The host re-checks that a projection's columns come back in its order.
    if (request != nullptr && request->n_projection > 0) {
        if (request->n_projection != N_COLUMNS)
            return dftu_fail(ctx, DFTU_E_INVALID,
                             "sessionize_input: unknown projection");
        for (std::int32_t i = 0; i < N_COLUMNS; ++i) {
            const int at = column_index(request->projection[i]);
            if (at < 0) {
                delete cursor;
                return dftu_fail(ctx, DFTU_E_INVALID,
                                 "sessionize_input: unknown projection");
            }
            cursor->order[i] = at;
        }
    }
    *out_cursor_self = cursor;
    *out_vt = &CURSOR_VT;
    return DFTU_OK;
}

void source_destroy(void*) {}

const ::dftu_source_vt SOURCE_VT = {.struct_size = sizeof(dftu_source_vt),
                                    .schema = schema,
                                    .scan = scan,
                                    .destroy = source_destroy};

::dftu_plugin g_plugin;

void destroy(void* self) { (void)self; }

}  // namespace

DFTU_PLUGIN_INFO("sessionize_input", "1.0")

extern "C" DFTU_PLUGIN_EXPORT dftu_status
dftracer_plugin(dftu_ctx* ctx, dftu_plugin_host* h, const dftu_value* config,
                dftu_plugin** out) {
    (void)config;
    const auto* providers =
        (const ::dftu_svc_providers*)h->get_service(h->h, DFTU_SVC_PROVIDERS);
    if (providers == nullptr || providers->register_provider == nullptr)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED,
                         "sessionize_input: no provider registry");
    if (providers->register_provider(ctx, h->h, "sessionize_input.rows",
                                     &SOURCE_VT, nullptr) < 0)
        return DFTU_E_INVALID;
    g_plugin.struct_size = sizeof(g_plugin);
    g_plugin.destroy = destroy;
    *out = &g_plugin;
    return DFTU_OK;
}
