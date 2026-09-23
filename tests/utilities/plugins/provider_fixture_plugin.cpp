/* Test-only plugin exercising rung 2 of the plugin ladder: a plugin
 * registering itself as a named LazyFrame source (dftu.svc.providers@0)
 * rather than only consuming rows.
 *
 * Registers two providers:
 *   - "provider_fixture.rows": two known frames, the first answered
 *     synchronously, the second via a real dftu_task the host must await, then
 *     end of stream. Exercises both the sync and async dftu_cursor_vt::next
 *     paths in one cursor.
 *   - "provider_fixture.failing": a cursor whose first next() call fails, to
 *     prove a mid-stream error surfaces rather than being read as end of
 *     stream.
 *
 * The factory also probes dftu_svc_providers::register_provider's name gate
 * (host-reserved namespace, unqualified name, duplicate name) and records
 * whether every probe behaved as documented. Every exported
 * provider_fixture_* accessor lets the test read counters/results out of this
 * translation unit's statics by dlsym-ing this same shared object a second
 * time (dlopen on an already-resident path returns the same image).
 */

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/coro/task_abi.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/abi.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace {

std::atomic<int> g_rows_source_destroy{0};
std::atomic<int> g_rows_cursor_destroy{0};
std::atomic<int> g_failing_next_calls{0};
std::atomic<int> g_failing_cursor_destroy{0};
std::atomic<int> g_gate_ok{0};

const char* ROWS_NAMES[] = {"id", "val"};
const char* FAILING_NAMES[] = {"x"};

::dftu_dataframe* make_rows_frame(const int64_t* ids, const int64_t* vals,
                                  int64_t n) {
    ::dftu_series* id_col =
        ::dftu_series_new_flat(DFTU_TYPE_INT64, ids, n, nullptr);
    ::dftu_series* val_col =
        ::dftu_series_new_flat(DFTU_TYPE_INT64, vals, n, nullptr);
    ::dftu_series* cols[2] = {id_col, val_col};
    return ::dftu_dataframe_new(ROWS_NAMES, cols, 2);
}

/* ---- provider_fixture.rows: sync frame 1, async frame 2, then EOS ---- */

int32_t rows_schema(void*, const char* const** out_names) {
    *out_names = ROWS_NAMES;
    return 2;
}

struct RowsCursor {
    int call = 0;
};

dftu_task* rows_next(void* self, int64_t, ::dftu_result_frame* out) {
    auto* cur = static_cast<RowsCursor*>(self);
    const int call = cur->call++;
    if (call == 0) {
        const int64_t ids[] = {1, 2, 3};
        const int64_t vals[] = {10, 20, 30};
        out->ok = 1;
        out->u.value = make_rows_frame(ids, vals, 3);
        return nullptr;  // synchronous
    }
    if (call == 1) {
        // Asynchronous: a real coroutine task the host must co_await before
        // *out is valid.
        auto fill = [](::dftu_result_frame* o)
            -> dftracer::utils::coro::CoroTask<void> {
            const int64_t ids[] = {4, 5};
            const int64_t vals[] = {40, 50};
            o->ok = 1;
            o->u.value = make_rows_frame(ids, vals, 2);
            co_return;
        };
        return dftracer::utils::task_to_abi(fill(out));
    }
    out->ok = 1;
    out->u.value = nullptr;  // end of stream
    return nullptr;
}

void rows_cursor_destroy(void* self) {
    delete static_cast<RowsCursor*>(self);
    g_rows_cursor_destroy.fetch_add(1);
}

const ::dftu_cursor_vt ROWS_CURSOR_VT = {rows_next, rows_cursor_destroy};

void* rows_scan(void*, const ::dftu_scan_request*, int32_t*,
                void** out_cursor_self, const ::dftu_cursor_vt** out_vt) {
    auto* cur = new RowsCursor();
    *out_cursor_self = cur;
    *out_vt = &ROWS_CURSOR_VT;
    return cur;  // non-NULL sentinel; the real self is *out_cursor_self
}

void rows_source_destroy(void*) { g_rows_source_destroy.fetch_add(1); }

const ::dftu_source_vt ROWS_SOURCE_VT = {rows_schema, rows_scan,
                                         rows_source_destroy};

/* ---- provider_fixture.failing: next() fails on the first call ---- */

int32_t failing_schema(void*, const char* const** out_names) {
    *out_names = FAILING_NAMES;
    return 1;
}

dftu_task* failing_next(void*, int64_t, ::dftu_result_frame* out) {
    g_failing_next_calls.fetch_add(1);
    out->ok = 0;
    out->u.err = ::dftu_error{0, 0, DFTU_COND_IO,
                              "provider_fixture: injected mid-stream failure"};
    return nullptr;
}

void failing_cursor_destroy(void*) { g_failing_cursor_destroy.fetch_add(1); }

const ::dftu_cursor_vt FAILING_CURSOR_VT = {failing_next,
                                            failing_cursor_destroy};

void* failing_scan(void*, const ::dftu_scan_request*, int32_t*,
                   void** out_cursor_self, const ::dftu_cursor_vt** out_vt) {
    static int dummy = 0;
    *out_cursor_self = &dummy;
    *out_vt = &FAILING_CURSOR_VT;
    return &dummy;
}

void failing_source_destroy(void*) {}

const ::dftu_source_vt FAILING_SOURCE_VT = {failing_schema, failing_scan,
                                            failing_source_destroy};

/* ---- fold side: never exercised (the test never calls Plugins::run) ---- */

void* make_slice(void*) { return nullptr; }
dftu_task* on_batch(void*, const dftu_dataframe*, const dftu_plugin_host*) {
    return nullptr;
}
void merge(void*, void*) {}
void destroy_slice(void*) {}
void destroy(void*) {}

dftu_plugin g_plugin;

}  // namespace

extern "C" {

DFTU_PLUGIN_EXPORT int provider_fixture_gate_ok(void) {
    return g_gate_ok.load();
}
DFTU_PLUGIN_EXPORT int provider_fixture_source_destroy_count(void) {
    return g_rows_source_destroy.load();
}
DFTU_PLUGIN_EXPORT int provider_fixture_cursor_destroy_count(void) {
    return g_rows_cursor_destroy.load();
}
DFTU_PLUGIN_EXPORT int provider_fixture_failing_next_calls(void) {
    return g_failing_next_calls.load();
}
DFTU_PLUGIN_EXPORT int provider_fixture_failing_cursor_destroy_count(void) {
    return g_failing_cursor_destroy.load();
}

DFTU_PLUGIN_EXPORT dftu_plugin* dftracer_plugin(dftu_plugin_host* h,
                                                const dftu_value*) {
    const auto* providers = static_cast<const dftu_svc_providers*>(
        h->get_service(h->h, DFTU_SVC_PROVIDERS));
    if (!providers || !providers->register_provider) return nullptr;

    bool ok = true;
    ok = providers->register_provider(h->h, "provider_fixture.rows",
                                      &ROWS_SOURCE_VT, nullptr) == 0 &&
         ok;
    ok = providers->register_provider(h->h, "provider_fixture.failing",
                                      &FAILING_SOURCE_VT, nullptr) == 0 &&
         ok;
    // Host-reserved namespace: must be refused.
    ok = providers->register_provider(h->h, "dftu.reserved", &ROWS_SOURCE_VT,
                                      nullptr) != 0 &&
         ok;
    // Unqualified (no "<plugin>." prefix): must be refused.
    ok = providers->register_provider(h->h, "bareword", &ROWS_SOURCE_VT,
                                      nullptr) != 0 &&
         ok;
    // Duplicate of an already-registered name: must be refused, not silently
    // replaced.
    ok = providers->register_provider(h->h, "provider_fixture.rows",
                                      &ROWS_SOURCE_VT, nullptr) != 0 &&
         ok;
    g_gate_ok.store(ok ? 1 : 0);

    std::memset(&g_plugin, 0, sizeof(g_plugin));
    g_plugin.abi_version = DFTRACER_PLUGIN_ABI_VERSION;
    g_plugin.make_slice = make_slice;
    g_plugin.on_batch = on_batch;
    g_plugin.merge = merge;
    g_plugin.destroy_slice = destroy_slice;
    g_plugin.destroy = destroy;
    return &g_plugin;
}

}  // extern "C"
