// Rung 2 of the plugin ladder: a plugin can register itself as a named
// LazyFrame source (dftu.svc.providers@0), not just consume rows. Drives the
// real dlopen path through Plugins::Builder against provider_fixture_plugin,
// then reaches the registered providers through the public
// dftu_lazyframe_from_provider entry point (dataframe/abi.h) - the same call
// a non-plugin C caller would make.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/plugins.h>
#include <dlfcn.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <optional>

using dftracer::utils::plugins::Plugins;

#ifndef PROVIDER_FIXTURE_PLUGIN_PATH
#error "PROVIDER_FIXTURE_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

std::optional<Plugins> load_fixture() {
    auto result = Plugins::Builder().add(PROVIDER_FIXTURE_PLUGIN_PATH).build();
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

std::vector<std::int64_t> read_i64_column(const dftu_dataframe* df,
                                          const char* name) {
    dftu_series* col = dftu_dataframe_column(df, name);
    REQUIRE(col);
    REQUIRE(dftu_series_type(col) == DFTU_TYPE_INT64);
    const auto* data = static_cast<const std::int64_t*>(dftu_series_data(col));
    const std::int64_t n = dftu_dataframe_num_rows(df);
    std::vector<std::int64_t> out(data, data + n);
    dftu_series_free(col);
    return out;
}

}  // namespace

TEST_CASE("a registered provider backs a LazyFrame that collects its rows") {
    std::optional<Plugins> plugins = load_fixture();

    dftu_lazyframe* lf = dftu_lazyframe_from_provider("provider_fixture.rows");
    REQUIRE(lf);

    dftu_dataframe* out = dftu_lazyframe_collect(lf, -1);
    REQUIRE(out);
    CHECK(dftu_dataframe_num_rows(out) == 5);
    CHECK(read_i64_column(out, "id") ==
          std::vector<std::int64_t>{1, 2, 3, 4, 5});
    CHECK(read_i64_column(out, "val") ==
          std::vector<std::int64_t>{10, 20, 30, 40, 50});

    dftu_dataframe_free(out);
    dftu_lazyframe_free(lf);
}

TEST_CASE("an op chained on the provider's LazyFrame composes correctly") {
    std::optional<Plugins> plugins = load_fixture();

    dftu_lazyframe* lf = dftu_lazyframe_from_provider("provider_fixture.rows");
    REQUIRE(lf);
    dftu_lazyframe* headed = dftu_lazyframe_head(lf, 2);
    dftu_lazyframe_free(lf);
    REQUIRE(headed);

    dftu_dataframe* out = dftu_lazyframe_collect(headed, -1);
    REQUIRE(out);
    CHECK(dftu_dataframe_num_rows(out) == 2);
    CHECK(read_i64_column(out, "id") == std::vector<std::int64_t>{1, 2});

    dftu_dataframe_free(out);
    dftu_lazyframe_free(headed);
}

TEST_CASE("register_provider refuses a dftu.-prefixed and a duplicate name") {
    std::optional<Plugins> plugins = load_fixture();
    FixtureAccessors fx;
    REQUIRE(fx.handle);
    auto gate_ok = fx.sym<int (*)()>("provider_fixture_gate_ok");
    REQUIRE(gate_ok);
    CHECK(gate_ok() == 1);
}

TEST_CASE("a cursor failing mid-stream surfaces as a collect failure") {
    std::optional<Plugins> plugins = load_fixture();
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
    std::optional<Plugins> plugins = load_fixture();
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
        std::optional<Plugins> plugins = load_fixture();
        dftu_lazyframe* lf =
            dftu_lazyframe_from_provider("provider_fixture.rows");
        CHECK(lf);
        dftu_lazyframe_free(lf);
    }  // ~Plugins unregisters every provider it added.

    CHECK(source_destroys() - before == 1);
    CHECK(dftu_lazyframe_from_provider("provider_fixture.rows") == nullptr);
}
