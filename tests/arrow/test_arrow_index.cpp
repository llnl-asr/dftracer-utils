// SPDX-License-Identifier: MIT
// The row index crosses Arrow: schema metadata `dftracer.utils.index` in the C
// Data Interface, the IPC buffer and the C Stream Interface, and the pandas
// schema metadata maps to it on import.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/arrow/arrow.h>
#include <dftracer/utils/arrow/batch.h>
#include <dftracer/utils/arrow/ipc.h>
#include <dftracer/utils/arrow/stream.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <nanoarrow/nanoarrow.h>
#include <task_result.h>

#include <cstdint>
#include <cstring>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::arrow;
using namespace dftracer::utils::dataframe;
using dftu_utils_test::ok;

namespace {

DataFrame make_frame(dstd::Span<const dstd::StringView> index) {
    const std::int64_t k1[] = {1, 1, 2, 2};
    const std::int64_t k2[] = {10, 20, 10, 20};
    const double v[] = {0.5, 1.5, 2.5, 3.5};
    DataFrame frame;
    frame.names = dstd::make_shared_strings({"k1", "k \"2\"", "v"});
    frame.columns.push_back(Series::flat_i64(k1, 4));
    frame.columns.push_back(Series::flat_i64(k2, 4));
    frame.columns.push_back(Series::flat_f64(v, 4));
    if (index.empty()) return frame;
    return ok(frame.with_index(index));
}

dstd::Vector<dstd::String> index_of(const DataFrame& df) {
    dstd::Vector<dstd::String> out;
    for (const dstd::SharedString& n : df.index_names())
        out.emplace_back(dstd::StringView(n));
    return out;
}

const dstd::StringView TWO[] = {"k1", "k \"2\""};

dstd::String metadata_value(const ArrowSchema* schema, dstd::StringView key) {
    ArrowMetadataReader reader;
    REQUIRE(ArrowMetadataReaderInit(&reader, schema->metadata) == NANOARROW_OK);
    ArrowStringView k, v;
    while (ArrowMetadataReaderRead(&reader, &k, &v) == NANOARROW_OK)
        if (dstd::StringView(k.data, static_cast<std::size_t>(k.size_bytes)) ==
            key)
            return dstd::String(dstd::StringView(
                v.data, static_cast<std::size_t>(v.size_bytes)));
    return {};
}

Batch with_pandas_metadata(const DataFrame& frame, dstd::CStringView json) {
    Batch batch = ok(to_arrow(frame));
    REQUIRE(ArrowSchemaSetMetadata(const_cast<ArrowSchema*>(batch.schema()),
                                   nullptr) == NANOARROW_OK);
    ArrowBuffer meta;
    ArrowMetadataBuilderInit(&meta, nullptr);
    ArrowMetadataBuilderAppend(&meta, ArrowCharView("pandas"),
                               ArrowCharView(json.c_str()));
    REQUIRE(ArrowSchemaSetMetadata(const_cast<ArrowSchema*>(batch.schema()),
                                   reinterpret_cast<const char*>(meta.data)) ==
            NANOARROW_OK);
    ArrowBufferReset(&meta);
    return batch;
}

}  // namespace

TEST_CASE("the C Data Interface carries the index in the schema metadata") {
    DataFrame frame = make_frame(TWO);
    Batch batch = ok(to_arrow(frame));
    CHECK(metadata_value(batch.schema(), "dftracer.utils.index") ==
          "[\"k1\",\"k \\\"2\\\"\"]");
    DataFrame back = ok(dataframe_from_arrow(std::move(batch)));
    CHECK(index_of(back) == index_of(frame));
    CHECK(back.num_columns() == 3);
}

TEST_CASE("an unindexed frame writes no index metadata") {
    Batch batch = ok(to_arrow(make_frame({})));
    CHECK(batch.schema()->metadata == nullptr);
    CHECK(ok(dataframe_from_arrow(std::move(batch))).index_names().empty());
}

TEST_CASE("the IPC buffer round trip keeps the index") {
    DataFrame frame = make_frame(TWO);
    Writer writer = Writer::to_buffer();
    REQUIRE(writer.write(frame));
    REQUIRE(writer.write(frame));
    REQUIRE(writer.finish());
    dstd::Vector<std::uint8_t> bytes = writer.take_bytes();
    Reader reader = ok(Reader::from_bytes(bytes));
    for (int i = 0; i < 2; ++i) {
        auto next = ok(reader.next());
        REQUIRE(next.has_value());
        CHECK(index_of(*next) == index_of(frame));
        CHECK(next->num_rows() == 4);
    }
}

TEST_CASE("the IPC buffer round trip of an unindexed frame has no index") {
    Writer writer = Writer::to_buffer();
    REQUIRE(writer.write(make_frame({})));
    REQUIRE(writer.finish());
    dstd::Vector<std::uint8_t> bytes = writer.take_bytes();
    Reader reader = ok(Reader::from_bytes(bytes));
    auto next = ok(reader.next());
    REQUIRE(next.has_value());
    CHECK(next->index_names().empty());
}

TEST_CASE("the C Stream Interface round trip keeps the index") {
    DataFrame frame = make_frame(TWO);
    ArrowArrayStream stream{};
    REQUIRE(to_arrow_stream(frame, &stream));
    DataFrame back = ok(dataframe_from_arrow_stream(&stream));
    CHECK(index_of(back) == index_of(frame));
    CHECK(back.num_rows() == 4);
}

TEST_CASE("pandas metadata maps its index columns to the index") {
    DataFrame frame = make_frame({});
    SUBCASE("named index columns") {
        DataFrame got = ok(dataframe_from_arrow(with_pandas_metadata(
            frame,
            R"({"index_columns": ["k1", "k \"2\""], "columns": [{"name": "k1", "field_name": "k1", "metadata": {"a": [1, {"b": null}]}}], "pandas_version": "2.2.0"})")));
        CHECK(index_of(got) == index_of(make_frame(TWO)));
    }
    SUBCASE("a RangeIndex is none") {
        DataFrame got = ok(dataframe_from_arrow(with_pandas_metadata(
            frame,
            R"({"index_columns": [{"kind": "range", "name": null, "start": 0, "stop": 4, "step": 1}], "columns": []})")));
        CHECK(got.index_names().empty());
    }
    SUBCASE("a name that is no column is ignored") {
        DataFrame got = ok(dataframe_from_arrow(with_pandas_metadata(
            frame, R"({"index_columns": ["k1", "missing"]})")));
        CHECK(index_of(got) == dstd::Vector<dstd::String>{"k1"});
    }
    SUBCASE("malformed pandas metadata gives no index") {
        DataFrame got = ok(dataframe_from_arrow(
            with_pandas_metadata(frame, R"({"index_columns": [)")));
        CHECK(got.index_names().empty());
    }
}
