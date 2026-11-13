// SPDX-License-Identifier: MIT
// Rung 2 of the plugin ladder: a plugin can register itself as a named
// LazyFrame source (dftu.svc.providers@0), not just consume rows. Drives the
// real dlopen path through Plugins::Builder against provider_fixture_plugin,
// then reaches the registered providers through the public
// dftu_lazyframe_from_provider entry point (dataframe/abi.h) - the same call
// a non-plugin C caller would make.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/abi_context.h>
#include <dftracer/utils/plugins/host/fold_adapter/ext.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <dftracer/utils/plugins/task_abi.h>
#include <dlfcn.h>
#include <doctest/doctest.h>

#include <cstdint>

using namespace dftracer::utils;

#ifndef PROVIDER_FIXTURE_PLUGIN_PATH
#error "PROVIDER_FIXTURE_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

dstd::Optional<plugins::host::Plugins> load_fixture() {
    auto result = plugins::host::Plugins::Builder()
                      .add(PROVIDER_FIXTURE_PLUGIN_PATH)
                      .build();
    REQUIRE(result.has_value());
    return std::move(result).value();
}

// dlopen on an already-resident path returns a handle to the SAME loaded
// image (its statics are not duplicated), so this is a legitimate way for the
// test to read the fixture's counters without Plugins exposing its internal
// dlopen handle.
struct FixtureAccessors {
    void* handle = dlopen(PROVIDER_FIXTURE_PLUGIN_PATH, RTLD_NOW | RTLD_LOCAL);

    template <class Fn>
    Fn sym(const char* name) const {
        return reinterpret_cast<Fn>(dlsym(handle, name));
    }

    ~FixtureAccessors() {
        if (handle) dlclose(handle);
    }
};

dstd::Vector<std::int64_t> read_i64_column(const dftu_dataframe* df,
                                           const char* name) {
    dftu_series* col = dftu_dataframe_column(df, name);
    REQUIRE(col);
    REQUIRE(dftu_series_type(col) == DFTU_TYPE_INT64);
    dftu_series* flat = dftu_series_materialize(col);
    REQUIRE(flat);
    const auto* data = static_cast<const std::int64_t*>(dftu_series_data(flat));
    const std::int64_t n = dftu_dataframe_num_rows(df);
    dstd::Vector<std::int64_t> out(data, data + n);
    dftu_series_free(flat);
    dftu_series_free(col);
    return out;
}

const char* ASYNC_NAMES[] = {"id"};

struct AsyncCursor {
    int call = 0;
};

coro::CoroTask<void> fill_async(dftu_dataframe** out) {
    const std::int64_t ids[] = {7, 8};
    dftu_series* col = dftu_series_new_flat(DFTU_TYPE_INT64, ids, 2, nullptr);
    *out = dftu_dataframe_new(ASYNC_NAMES, &col, 1);
    co_return;
}

dftu_status async_next(dftu_ctx*, void* self, std::int64_t,
                       dftu_dataframe** out, dftu_task** pending) {
    auto* cur = static_cast<AsyncCursor*>(self);
    if (cur->call++ == 0) {
        *pending = plugins::task_to_abi(fill_async(out));
        return DFTU_OK;
    }
    *out = nullptr;
    return DFTU_END;
}

void async_cursor_destroy(void* self) {
    delete static_cast<AsyncCursor*>(self);
}

const dftu_cursor_vt ASYNC_CURSOR_VT = {.struct_size = sizeof(dftu_cursor_vt),
                                        .next = async_next,
                                        .destroy = async_cursor_destroy};

dftu_status async_schema(dftu_ctx*, void*, const char* const** names,
                         std::int32_t* count) {
    *names = ASYNC_NAMES;
    *count = 1;
    return DFTU_OK;
}

dftu_status async_scan(dftu_ctx*, void*, const dftu_scan_request*,
                       std::int32_t*, void** cursor_self,
                       const dftu_cursor_vt** cursor_vt) {
    *cursor_self = new AsyncCursor();
    *cursor_vt = &ASYNC_CURSOR_VT;
    return DFTU_OK;
}

const dftu_source_vt ASYNC_SOURCE_VT = {.struct_size = sizeof(dftu_source_vt),
                                        .schema = async_schema,
                                        .scan = async_scan};

}  // namespace

TEST_CASE("a provider whose next returns a task is awaited by the host") {
    plugins::AbiContext ctx;
    REQUIRE(plugins::host::detail::register_plugin_provider(
                ctx.get(), "inproc.async", &ASYNC_SOURCE_VT, nullptr,
                nullptr) >= 0);
    dftu_lazyframe* lf = dftu_lazyframe_from_provider("inproc.async");
    REQUIRE(lf);
    dftu_dataframe* out = dftu_lazyframe_collect(lf, -1);
    REQUIRE(out);
    CHECK(read_i64_column(out, "id") == dstd::Vector<std::int64_t>{7, 8});
    dftu_dataframe_free(out);
    dftu_lazyframe_free(lf);
    dftu_provider_unregister("inproc.async");
}

TEST_CASE("a registered provider backs a LazyFrame that collects its rows") {
    dstd::Optional<plugins::host::Plugins> plugins = load_fixture();

    dftu_lazyframe* lf = dftu_lazyframe_from_provider("provider_fixture.rows");
    REQUIRE(lf);

    dftu_dataframe* out = dftu_lazyframe_collect(lf, -1);
    REQUIRE(out);
    CHECK(dftu_dataframe_num_rows(out) == 5);
    CHECK(read_i64_column(out, "id") ==
          dstd::Vector<std::int64_t>{1, 2, 3, 4, 5});
    CHECK(read_i64_column(out, "val") ==
          dstd::Vector<std::int64_t>{10, 20, 30, 40, 50});

    dftu_dataframe_free(out);
    dftu_lazyframe_free(lf);
}

TEST_CASE("an op chained on the provider's LazyFrame composes correctly") {
    dstd::Optional<plugins::host::Plugins> plugins = load_fixture();

    dftu_lazyframe* lf = dftu_lazyframe_from_provider("provider_fixture.rows");
    REQUIRE(lf);
    dftu_lazyframe* headed = dftu_lazyframe_head(lf, 2);
    dftu_lazyframe_free(lf);
    REQUIRE(headed);

    dftu_dataframe* out = dftu_lazyframe_collect(headed, -1);
    REQUIRE(out);
    CHECK(dftu_dataframe_num_rows(out) == 2);
    CHECK(read_i64_column(out, "id") == dstd::Vector<std::int64_t>{1, 2});

    dftu_dataframe_free(out);
    dftu_lazyframe_free(headed);
}

TEST_CASE("register_provider refuses a dftu.-prefixed and a duplicate name") {
    dstd::Optional<plugins::host::Plugins> plugins = load_fixture();
    FixtureAccessors fx;
    REQUIRE(fx.handle);
    auto gate_ok = fx.sym<int (*)()>("provider_fixture_gate_ok");
    REQUIRE(gate_ok);
    CHECK(gate_ok() == 1);
}

TEST_CASE("a cursor failing mid-stream surfaces as a collect failure") {
    dstd::Optional<plugins::host::Plugins> plugins = load_fixture();
    FixtureAccessors fx;
    REQUIRE(fx.handle);
    auto next_calls = fx.sym<int (*)()>("provider_fixture_failing_next_calls");
    REQUIRE(next_calls);
    CHECK(next_calls() == 0);

    dftu_lazyframe* lf =
        dftu_lazyframe_from_provider("provider_fixture.failing");
    REQUIRE(lf);
    dftu_dataframe* out = dftu_lazyframe_collect(lf, -1);
    CHECK(out == nullptr);
    // Exactly one call: the failure is surfaced, not retried in a loop.
    CHECK(next_calls() == 1);

    dftu_lazyframe_free(lf);
}

TEST_CASE("source and cursor destroy are each called exactly once per scan") {
    dstd::Optional<plugins::host::Plugins> plugins = load_fixture();
    FixtureAccessors fx;
    REQUIRE(fx.handle);
    auto cursor_destroys =
        fx.sym<int (*)()>("provider_fixture_cursor_destroy_count");
    REQUIRE(cursor_destroys);
    const int before = cursor_destroys();

    dftu_lazyframe* lf = dftu_lazyframe_from_provider("provider_fixture.rows");
    REQUIRE(lf);
    dftu_dataframe* out = dftu_lazyframe_collect(lf, -1);
    REQUIRE(out);
    dftu_dataframe_free(out);
    dftu_lazyframe_free(lf);

    CHECK(cursor_destroys() - before == 1);
}

TEST_CASE("a provider is gone once the loading plugin set is destroyed") {
    FixtureAccessors fx;
    REQUIRE(fx.handle);
    auto source_destroys =
        fx.sym<int (*)()>("provider_fixture_source_destroy_count");
    REQUIRE(source_destroys);
    const int before = source_destroys();

    {
        dstd::Optional<plugins::host::Plugins> plugins = load_fixture();
        dftu_lazyframe* lf =
            dftu_lazyframe_from_provider("provider_fixture.rows");
        CHECK(lf);
        dftu_lazyframe_free(lf);
    }  // ~Plugins unregisters every provider it added.

    CHECK(source_destroys() - before == 1);
    CHECK(dftu_lazyframe_from_provider("provider_fixture.rows") == nullptr);
}

TEST_CASE("a C++ SDK source absorbs a filter and is freed with the plugin") {
    FixtureAccessors fx;
    REQUIRE(fx.handle);
    auto alive = fx.sym<int (*)()>("provider_fixture_sdk_alive");
    auto filters = fx.sym<int (*)()>("provider_fixture_sdk_filters");
    REQUIRE(alive);
    REQUIRE(filters);
    const int filters_before = filters();

    {
        dstd::Optional<plugins::host::Plugins> plugins = load_fixture();
        dftu_lazyframe* lf =
            dftu_lazyframe_from_provider("provider_fixture.sdk");
        REQUIRE(lf);
        dftu_expr* id = dftu_expr_col(0);
        dftu_scalar six{};
        six.kind = DFTU_SCALAR_TAG_I64;
        six.value.i = 6;
        dftu_expr* pred = dftu_expr_cmp(DFTU_CMP_GT, id, six);
        dftu_lazyframe* filtered = dftu_lazyframe_filter(lf, pred);
        dftu_expr_free(pred);
        dftu_expr_free(id);
        dftu_lazyframe_free(lf);
        REQUIRE(filtered);

        dftu_dataframe* out = dftu_lazyframe_collect(filtered, -1);
        REQUIRE(out);
        CHECK(read_i64_column(out, "id") ==
              dstd::Vector<std::int64_t>{7, 8, 9});
        CHECK(filters() > filters_before);
        dftu_dataframe_free(out);
        dftu_lazyframe_free(filtered);
    }

    CHECK(alive() == 0);
    CHECK(dftu_lazyframe_from_provider("provider_fixture.sdk") == nullptr);
}

TEST_CASE("a C++ SDK node runs over a provider and leaves with the plugin") {
    {
        dstd::Optional<plugins::host::Plugins> plugins = load_fixture();
        dftu_lazyframe* lf =
            dftu_lazyframe_from_provider("provider_fixture.sdk");
        REQUIRE(lf);
        dftu_lazyframe* doubled =
            dftu_lazyframe_op(lf, "provider_fixture.double", nullptr);
        dftu_lazyframe_free(lf);
        REQUIRE(doubled);

        dftu_dataframe* out = dftu_lazyframe_collect(doubled, -1);
        REQUIRE(out);
        CHECK(read_i64_column(out, "id") ==
              dstd::Vector<std::int64_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
        CHECK(read_i64_column(out, "val") ==
              dstd::Vector<std::int64_t>{0, 20, 40, 60, 80, 100, 120, 140, 160,
                                         180});
        dftu_dataframe_free(out);
        dftu_lazyframe_free(doubled);
    }

    dftu_dataframe* one = nullptr;
    {
        const char* names[] = {"id", "val"};
        std::int64_t v[] = {1};
        dftu_series* cols[] = {
            dftu_series_new_flat(DFTU_TYPE_INT64, v, 1, nullptr),
            dftu_series_new_flat(DFTU_TYPE_INT64, v, 1, nullptr)};
        one = dftu_dataframe_new(names, cols, 2);
    }
    dftu_lazyframe* lf = dftu_dataframe_lazy(one);
    CHECK(dftu_lazyframe_op(lf, "provider_fixture.double", nullptr) == nullptr);
    dftu_lazyframe_free(lf);
    dftu_dataframe_free(one);
}
