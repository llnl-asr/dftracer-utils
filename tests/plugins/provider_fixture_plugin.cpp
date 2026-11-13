// SPDX-License-Identifier: MIT
/* Test-only plugin exercising rung 2 of the plugin ladder: a plugin
 * registering itself as a named LazyFrame source (dftu.svc.providers)
 * rather than only consuming rows.
 *
 * Registers two providers:
 *   - "provider_fixture.rows": two known frames, then end of stream. A plugin
 *     built by the build host cannot start a task, so the async next path is
 *     covered in-process by test_plugin_providers.cpp.
 *   - "provider_fixture.failing": a cursor whose first next() call fails, to
 *     prove a mid-stream error surfaces rather than being read as end of
 *     stream.
 *   - "provider_fixture.sdk": rows id 0..9 written with the C++ source SDK
 *     (plugins/plugin/source.h), absorbing `id > k` at plan time.
 *
 * and one plan node, "provider_fixture.double", written with the C++ node SDK
 * (plugins/plugin/node.h): doubles "val" and keeps "id".
 *
 * The factory also probes dftu_svc_providers::register_provider's name gate
 * (host-reserved namespace, unqualified name, duplicate name) and records
 * whether every probe behaved as documented. Every exported
 * provider_fixture_* accessor lets the test read counters/results out of this
 * translation unit's statics by dlsym-ing this same shared object a second
 * time (dlopen on an already-resident path returns the same image).
 */

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/plugin.h>

#include <atomic>
#include <cstdint>
#include <cstring>

using namespace dftracer::utils;
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

/* ---- provider_fixture.rows: two frames, then EOS ---- */

dftu_status rows_schema(dftu_ctx*, void*, const char* const** out_names,
                        int32_t* out_count) {
    *out_names = ROWS_NAMES;
    *out_count = 2;
    return DFTU_OK;
}

struct RowsCursor {
    int call = 0;
};

dftu_status rows_next(dftu_ctx*, void* self, int64_t, ::dftu_dataframe** out,
                      dftu_task**) {
    auto* cur = static_cast<RowsCursor*>(self);
    const int call = cur->call++;
    if (call == 0) {
        const int64_t ids[] = {1, 2, 3};
        const int64_t vals[] = {10, 20, 30};
        *out = make_rows_frame(ids, vals, 3);
        return DFTU_OK;  // synchronous
    }
    if (call == 1) {
        const int64_t ids[] = {4, 5};
        const int64_t vals[] = {40, 50};
        *out = make_rows_frame(ids, vals, 2);
        return DFTU_OK;
    }
    *out = nullptr;
    return DFTU_END;
}

void rows_cursor_destroy(void* self) {
    delete static_cast<RowsCursor*>(self);
    g_rows_cursor_destroy.fetch_add(1);
}

const ::dftu_cursor_vt ROWS_CURSOR_VT = {.struct_size = sizeof(dftu_cursor_vt),
                                         .next = rows_next,
                                         .destroy = rows_cursor_destroy};

dftu_status rows_scan(dftu_ctx*, void*, const dftu_plugin_host*,
                      const ::dftu_scan_request*, int32_t*,
                      void** out_cursor_self, const ::dftu_cursor_vt** out_vt) {
    auto* cur = new RowsCursor();
    *out_cursor_self = cur;
    *out_vt = &ROWS_CURSOR_VT;
    return DFTU_OK;
}

void rows_source_destroy(void*) { g_rows_source_destroy.fetch_add(1); }

const ::dftu_source_vt ROWS_SOURCE_VT = {.struct_size = sizeof(dftu_source_vt),
                                         .schema = rows_schema,
                                         .scan = rows_scan,
                                         .destroy = rows_source_destroy};

/* ---- provider_fixture.failing: next() fails on the first call ---- */

dftu_status failing_schema(dftu_ctx*, void*, const char* const** out_names,
                           int32_t* out_count) {
    *out_names = FAILING_NAMES;
    *out_count = 1;
    return DFTU_OK;
}

dftu_status failing_next(dftu_ctx* ctx, void*, int64_t, ::dftu_dataframe**,
                         dftu_task**) {
    g_failing_next_calls.fetch_add(1);
    return dftu_fail(ctx, DFTU_E_IO,
                     "provider_fixture: injected mid-stream failure");
}

void failing_cursor_destroy(void*) { g_failing_cursor_destroy.fetch_add(1); }

const ::dftu_cursor_vt FAILING_CURSOR_VT = {
    .struct_size = sizeof(dftu_cursor_vt),
    .next = failing_next,
    .destroy = failing_cursor_destroy};

dftu_status failing_scan(dftu_ctx*, void*, const dftu_plugin_host*,
                         const ::dftu_scan_request*, int32_t*,
                         void** out_cursor_self,
                         const ::dftu_cursor_vt** out_vt) {
    static int dummy = 0;
    *out_cursor_self = &dummy;
    *out_vt = &FAILING_CURSOR_VT;
    return DFTU_OK;
}

void failing_source_destroy(void*) {}

const ::dftu_source_vt FAILING_SOURCE_VT = {
    .struct_size = sizeof(dftu_source_vt),
    .schema = failing_schema,
    .scan = failing_scan,
    .destroy = failing_source_destroy};

/* ---- fold side: never exercised (the test never calls Plugins::run) ---- */

dftu_status make_slice(dftu_ctx*, void*, void** out) {
    *out = nullptr;
    return DFTU_OK;
}
dftu_status on_batch(dftu_ctx*, void*, const dftu_dataframe*,
                     const dftu_plugin_host*) {
    return DFTU_OK;
}
/* ---- provider_fixture.sdk: the C++ source SDK ---- */

std::atomic<int> g_sdk_alive{0};
std::atomic<int> g_sdk_filters{0};

class SdkRange {
   public:
    SdkRange(int64_t lo, int64_t hi) : lo_(lo), hi_(hi) { ++g_sdk_alive; }
    SdkRange(const SdkRange&) = delete;
    ~SdkRange() { --g_sdk_alive; }

    dstd::Vector<dstd::String> names() const { return {"id", "val"}; }

    class Cursor {
       public:
        explicit Cursor(dftracer::utils::plugins::OwnedFrame f)
            : f_(std::move(f)) {}
        dstd::Result<dstd::Optional<dftracer::utils::plugins::OwnedFrame>> next(
            int64_t) {
            if (!f_) return dstd::nullopt;
            return std::move(f_);
        }

       private:
        dftracer::utils::plugins::OwnedFrame f_;
    };

    dstd::Result<dftracer::utils::plugins::ScanResult<Cursor>> scan(
        const dftracer::utils::plugins::ScanView&,
        const dftracer::utils::plugins::Host&) const {
        dstd::Vector<int64_t> ids, vals;
        for (int64_t i = lo_; i < hi_; ++i) {
            ids.push_back(i);
            vals.push_back(i * 10);
        }
        return dftracer::utils::plugins::ScanResult<Cursor>{
            dstd::make_unique<Cursor>(dftracer::utils::plugins::OwnedFrame(
                make_rows_frame(ids.data(), vals.data(),
                                static_cast<int64_t>(ids.size())))),
            {}};
    }

    dstd::Result<dstd::Optional<dftracer::utils::plugins::Applied<SdkRange>>>
    apply_filter(dftracer::utils::plugins::ExprView pred) const {
        auto cmp = pred.as_compare();
        if (!cmp || cmp->column != 0 || cmp->op != DFTU_CMP_GT ||
            cmp->rhs.kind != DFTU_SCALAR_TAG_I64 || cmp->rhs.value.i + 1 <= lo_)
            return dstd::nullopt;
        ++g_sdk_filters;
        return dftracer::utils::plugins::Applied<SdkRange>{
            dstd::make_unique<SdkRange>(cmp->rhs.value.i + 1, hi_)};
    }

   private:
    int64_t lo_;
    int64_t hi_;
};

class DoubleVal {
   public:
    void output_schema(const dftracer::utils::plugins::SchemaView& in,
                       const dftracer::utils::plugins::OpArgs&,
                       dftracer::utils::plugins::SchemaBuilder& out) const {
        in.copy_all(out);
    }

    class Cursor {
       public:
        explicit Cursor(dftracer::utils::plugins::InputCursor in)
            : in_(std::move(in)) {}
        dstd::Result<dstd::Optional<dftracer::utils::plugins::OwnedFrame>> next(
            int64_t max_rows) {
            DFTRACER_UTILS_TRY(
                dstd::Optional<dftracer::utils::plugins::OwnedFrame> f,
                in_.next(max_rows));
            if (!f) return dstd::nullopt;
            ::dftu_series* id = ::dftu_dataframe_column(f->get(), "id");
            ::dftu_series* val = ::dftu_dataframe_column(f->get(), "val");
            const int64_t n = ::dftu_series_length(val);
            const auto* v =
                static_cast<const int64_t*>(::dftu_series_data(val));
            dstd::Vector<int64_t> doubled(v, v + n);
            for (int64_t& x : doubled) x *= 2;
            ::dftu_series* cols[] = {
                id, ::dftu_series_new_flat(DFTU_TYPE_INT64, doubled.data(), n,
                                           nullptr)};
            ::dftu_series_free(val);
            return dftracer::utils::plugins::OwnedFrame(
                ::dftu_dataframe_new(ROWS_NAMES, cols, 2));
        }

       private:
        dftracer::utils::plugins::InputCursor in_;
    };

    dstd::Result<dstd::UniquePtr<Cursor>> open(
        dftracer::utils::plugins::InputCursor in,
        const dftracer::utils::plugins::OpArgs&, std::uint64_t,
        const dftracer::utils::plugins::Host&) const {
        return dstd::make_unique<Cursor>(std::move(in));
    }
};

dftu_status merge(dftu_ctx*, void*, void*) { return DFTU_OK; }
void destroy_slice(void*) {}
void destroy(void*) {}

dftu_plugin g_plugin;

}  // namespace

extern "C" {
DFTU_PLUGIN_EXPORT int provider_fixture_gate_ok(void);
DFTU_PLUGIN_EXPORT int provider_fixture_source_destroy_count(void);
DFTU_PLUGIN_EXPORT int provider_fixture_cursor_destroy_count(void);
DFTU_PLUGIN_EXPORT int provider_fixture_failing_next_calls(void);
DFTU_PLUGIN_EXPORT int provider_fixture_failing_cursor_destroy_count(void);
DFTU_PLUGIN_EXPORT int provider_fixture_sdk_alive(void);
DFTU_PLUGIN_EXPORT int provider_fixture_sdk_filters(void);
}

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
DFTU_PLUGIN_EXPORT int provider_fixture_sdk_alive(void) {
    return g_sdk_alive.load();
}
DFTU_PLUGIN_EXPORT int provider_fixture_sdk_filters(void) {
    return g_sdk_filters.load();
}

DFTU_PLUGIN_INFO("provider_fixture", "1.0")

DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value*,
                                               dftu_plugin** out) {
    const auto* providers = static_cast<const dftu_svc_providers*>(
        h->get_service(h->h, DFTU_SVC_PROVIDERS));
    if (!providers || !providers->register_provider)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no provider registry");

    bool ok = true;
    ok = providers->register_provider(ctx, h->h, "provider_fixture.rows",
                                      &ROWS_SOURCE_VT, nullptr) >= 0 &&
         ok;
    ok = providers->register_provider(ctx, h->h, "provider_fixture.failing",
                                      &FAILING_SOURCE_VT, nullptr) >= 0 &&
         ok;
    // Host-reserved namespace: must be refused.
    ok = providers->register_provider(ctx, h->h, "dftu.reserved",
                                      &ROWS_SOURCE_VT, nullptr) < 0 &&
         ok;
    // Unqualified (no "<plugin>." prefix): must be refused.
    ok = providers->register_provider(ctx, h->h, "bareword", &ROWS_SOURCE_VT,
                                      nullptr) < 0 &&
         ok;
    // Duplicate of an already-registered name: must be refused, not silently
    // replaced.
    ok = providers->register_provider(ctx, h->h, "provider_fixture.rows",
                                      &ROWS_SOURCE_VT, nullptr) < 0 &&
         ok;
    g_gate_ok.store(ok ? 1 : 0);

    // The builder only registers here; the fold is this file's own plugin.
    dftu_plugin* unused = nullptr;
    const dftu_status st =
        dftracer::utils::plugins::plugin(ctx, h, nullptr)
            .source<SdkRange>("provider_fixture.sdk",
                              dstd::make_unique<SdkRange>(0, 10))
            .node<DoubleVal>("provider_fixture.double",
                             dstd::make_unique<DoubleVal>())
            .build(&unused);
    if (st < 0) return st;
    unused->destroy(unused->self);

    std::memset(&g_plugin, 0, sizeof(g_plugin));
    g_plugin.struct_size = sizeof(g_plugin);
    g_plugin.make_slice = make_slice;
    g_plugin.on_batch = on_batch;
    g_plugin.merge = merge;
    g_plugin.destroy_slice = destroy_slice;
    g_plugin.destroy = destroy;
    *out = &g_plugin;
    return DFTU_OK;
}

}  // extern "C"
