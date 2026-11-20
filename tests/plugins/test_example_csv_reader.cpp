// SPDX-License-Identifier: MIT
// The examples/plugins/csv_reader.cpp format over a file of more than one
// chunk. It reads 1024 rows per chunk and hands each chunk's rows straight out
// of the shared table, so this proves rows and order across the boundary.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <doctest/doctest.h>

#include <cstdint>

using namespace dftracer::utils;

#ifndef CSV_READER_PLUGIN_PATH
#error "CSV_READER_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef CSV_READER_CHUNKS_CSV
#error "CSV_READER_CHUNKS_CSV must be defined by CMake"
#endif

namespace {

dstd::Result<plugins::host::Plugins> load() {
    return plugins::host::Plugins::Builder()
        .add(CSV_READER_PLUGIN_PATH)
        .build();
}

}  // namespace

TEST_CASE("rows and order hold across the chunk boundary") {
    auto plugins = load();
    REQUIRE(plugins.has_value());

    dftu_lazyframe* frame = nullptr;
    REQUIRE(dftu_lazyframe_scan_file(CSV_READER_CHUNKS_CSV, nullptr, 0,
                                     &frame) == 0);
    REQUIRE(frame);
    const char* names[2] = {"id", "label"};
    dftu_lazyframe* projected = dftu_lazyframe_select(frame, names, 2);
    dftu_lazyframe_free(frame);
    REQUIRE(projected);

    dftu_dataframe* out = dftu_lazyframe_collect(projected, -1);
    REQUIRE(out);
    REQUIRE(dftu_dataframe_num_rows(out) == 1100);

    dftu_series* encoded = dftu_dataframe_column(out, "id");
    REQUIRE(encoded);
    // A projection hands back a view over the chunk's rows, not flat storage.
    dftu_series* ids = dftu_series_materialize(encoded);
    dftu_series_free(encoded);
    REQUIRE(ids);
    const auto* data = static_cast<const std::int64_t*>(dftu_series_data(ids));
    REQUIRE(data);
    for (std::int64_t row = 0; row < 1100; ++row) CHECK(data[row] == row);
    dftu_series_free(ids);

    // Row 1024 is the first row of the second chunk, and its text came with it.
    dftu_series* labels = dftu_dataframe_column(out, "label");
    REQUIRE(labels);
    std::int64_t len = 0;
    const char* label = dftu_series_string_at(labels, 1024, &len);
    REQUIRE(label);
    CHECK(dstd::String(label, static_cast<std::size_t>(len)) == "row1024");
    dftu_series_free(labels);

    dftu_dataframe_free(out);
    dftu_lazyframe_free(projected);
}
