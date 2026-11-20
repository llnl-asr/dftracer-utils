// SPDX-License-Identifier: MIT
// The examples/plugins/sessionize.cpp node over a table whose `ts` column is a
// TIMESTAMP and holds one null: the node reads both inputs through a
// materialized flat view, accepts a timestamp as an integer, and gives a row
// with a null pid or ts the session -1 without touching a pid's walk.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <doctest/doctest.h>

#include <cstdint>

using namespace dftracer::utils;

#ifndef SESSIONIZE_PLUGIN_PATH
#error "SESSIONIZE_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef SESSIONIZE_INPUT_PLUGIN_PATH
#error "SESSIONIZE_INPUT_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

dstd::Result<plugins::host::Plugins> load() {
    return plugins::host::Plugins::Builder()
        .add(SESSIONIZE_PLUGIN_PATH)
        .add(SESSIONIZE_INPUT_PLUGIN_PATH)
        .build();
}

dstd::Vector<std::int64_t> i64_column(const dftu_dataframe* df,
                                      dstd::CStringView name) {
    dstd::Vector<std::int64_t> out;
    dftu_series* column = dftu_dataframe_column(df, name.c_str());
    REQUIRE(column);
    const auto* data =
        static_cast<const std::int64_t*>(dftu_series_data(column));
    for (std::int64_t i = 0; i < dftu_series_length(column); ++i)
        out.push_back(data[i]);
    dftu_series_free(column);
    return out;
}

}  // namespace

TEST_CASE("the node reads a timestamp column and skips a null row") {
    auto plugins = load();
    REQUIRE(plugins.has_value());

    dftu_lazyframe* source =
        dftu_lazyframe_from_provider("sessionize_input.rows");
    REQUIRE(source);
    dftu_lazyframe* node =
        dftu_lazyframe_op(source, "sessionize.gaps", nullptr);
    dftu_lazyframe_free(source);
    REQUIRE(node);

    dftu_dataframe* out = dftu_lazyframe_collect(node, -1);
    REQUIRE(out);
    REQUIRE(dftu_dataframe_num_columns(out) == 3);
    CHECK(dstd::String(dftu_dataframe_column_name(out, 2)) == "session");

    // pid 1 rows 0 and 2 are 1000 us apart, so one session; row 1 has no ts and
    // takes no part; pid 2 opens its own.
    CHECK(i64_column(out, "session") ==
          dstd::Vector<std::int64_t>{0, -1, 0, 0});
    CHECK(i64_column(out, "pid") == dstd::Vector<std::int64_t>{1, 1, 1, 2});

    dftu_dataframe_free(out);
    dftu_lazyframe_free(node);
}
