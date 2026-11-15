// SPDX-License-Identifier: MIT
// The examples/plugins/range_provider.cpp provider, reached the way any C
// caller reaches a provider: dftu_lazyframe_from_provider. The table is built
// in code, so this proves the example's scan, its projection and its `rows`
// config key, none of which a golden .test file can reach.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <doctest/doctest.h>

#include <cstdint>

using namespace dftracer::utils;

#ifndef RANGE_PROVIDER_PLUGIN_PATH
#error "RANGE_PROVIDER_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

dstd::Result<plugins::host::Plugins> load(const char* config_json) {
    auto builder = plugins::host::Plugins::builder();
    if (config_json == nullptr)
        return builder.add(RANGE_PROVIDER_PLUGIN_PATH).build();
    return builder.add(RANGE_PROVIDER_PLUGIN_PATH, config_json).build();
}

dstd::Vector<std::int64_t> i64_column(const dftu_dataframe* df,
                                      const char* name) {
    dstd::Vector<std::int64_t> out;
    dftu_series* column = dftu_dataframe_column(df, name);
    REQUIRE(column);
    CHECK(dftu_series_type(column) == DFTU_TYPE_INT64);
    const auto* data =
        static_cast<const std::int64_t*>(dftu_series_data(column));
    for (std::int64_t i = 0; i < dftu_series_length(column); ++i)
        out.push_back(data[i]);
    dftu_series_free(column);
    return out;
}

}  // namespace

TEST_CASE("the example provider builds a table of squares") {
    auto plugins = load(nullptr);
    REQUIRE(plugins.has_value());

    dftu_lazyframe* frame =
        dftu_lazyframe_from_provider("range_provider.numbers");
    REQUIRE(frame);
    dftu_dataframe* out = dftu_lazyframe_collect(frame, -1);
    REQUIRE(out);
    CHECK(dftu_dataframe_num_columns(out) == 2);
    CHECK(i64_column(out, "i") ==
          dstd::Vector<std::int64_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
    CHECK(i64_column(out, "square") ==
          dstd::Vector<std::int64_t>{0, 1, 4, 9, 16, 25, 36, 49, 64, 81});

    dftu_dataframe_free(out);
    dftu_lazyframe_free(frame);
}

TEST_CASE("the rows config key sets the table size") {
    auto plugins = load(R"({"rows": 4})");
    REQUIRE(plugins.has_value());

    dftu_lazyframe* frame =
        dftu_lazyframe_from_provider("range_provider.numbers");
    REQUIRE(frame);
    dftu_dataframe* out = dftu_lazyframe_collect(frame, -1);
    REQUIRE(out);
    CHECK(dftu_dataframe_num_rows(out) == 4);
    CHECK(i64_column(out, "square") == dstd::Vector<std::int64_t>{0, 1, 4, 9});

    dftu_dataframe_free(out);
    dftu_lazyframe_free(frame);
}

TEST_CASE("a projection keeps exactly the columns it names, in its order") {
    auto plugins = load(nullptr);
    REQUIRE(plugins.has_value());

    dftu_lazyframe* frame =
        dftu_lazyframe_from_provider("range_provider.numbers");
    REQUIRE(frame);
    const char* names[2] = {"square", "i"};
    dftu_lazyframe* projected = dftu_lazyframe_select(frame, names, 2);
    dftu_lazyframe_free(frame);
    REQUIRE(projected);

    dftu_dataframe* out = dftu_lazyframe_collect(projected, -1);
    REQUIRE(out);
    REQUIRE(dftu_dataframe_num_columns(out) == 2);
    CHECK(dstd::String(dftu_dataframe_column_name(out, 0)) == "square");
    CHECK(dstd::String(dftu_dataframe_column_name(out, 1)) == "i");
    CHECK(i64_column(out, "square") ==
          dstd::Vector<std::int64_t>{0, 1, 4, 9, 16, 25, 36, 49, 64, 81});
    CHECK(i64_column(out, "i") ==
          dstd::Vector<std::int64_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9});

    dftu_dataframe_free(out);
    dftu_lazyframe_free(projected);
}

TEST_CASE("a repeated projection name gives the column twice") {
    auto plugins = load(nullptr);
    REQUIRE(plugins.has_value());

    dftu_lazyframe* frame =
        dftu_lazyframe_from_provider("range_provider.numbers");
    REQUIRE(frame);
    const char* names[2] = {"i", "i"};
    dftu_lazyframe* projected = dftu_lazyframe_select(frame, names, 2);
    dftu_lazyframe_free(frame);
    REQUIRE(projected);

    dftu_dataframe* out = dftu_lazyframe_collect(projected, -1);
    REQUIRE(out);
    REQUIRE(dftu_dataframe_num_columns(out) == 2);
    CHECK(dstd::String(dftu_dataframe_column_name(out, 0)) == "i");
    CHECK(dstd::String(dftu_dataframe_column_name(out, 1)) == "i");
    const dstd::Vector<std::int64_t> expected{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    CHECK(i64_column(out, "i") == expected);

    dftu_dataframe_free(out);
    dftu_lazyframe_free(projected);
}

TEST_CASE("a projection keeps only the column it names") {
    auto plugins = load(nullptr);
    REQUIRE(plugins.has_value());

    dftu_lazyframe* frame =
        dftu_lazyframe_from_provider("range_provider.numbers");
    REQUIRE(frame);
    const char* names[1] = {"square"};
    dftu_lazyframe* projected = dftu_lazyframe_select(frame, names, 1);
    dftu_lazyframe_free(frame);
    REQUIRE(projected);

    dftu_dataframe* out = dftu_lazyframe_collect(projected, -1);
    REQUIRE(out);
    REQUIRE(dftu_dataframe_num_columns(out) == 1);
    CHECK(dstd::String(dftu_dataframe_column_name(out, 0)) == "square");
    CHECK(i64_column(out, "square") ==
          dstd::Vector<std::int64_t>{0, 1, 4, 9, 16, 25, 36, 49, 64, 81});

    dftu_dataframe_free(out);
    dftu_lazyframe_free(projected);
}
