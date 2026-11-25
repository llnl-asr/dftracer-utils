// SPDX-License-Identifier: MIT
// A CHUNKED column leaves the bridge as one array that its schema describes
// exactly: string_view, binary, fixed_size_binary and dictionary children
// included, for a column, a frame and a stream.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <arrow/arrow_helpers.h>
#include <dataframe/builders_types.h>
#include <dftracer/utils/arrow/stream.h>
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/reshape.h>
#include <dftracer/utils/dataframe/internal/view_builder.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <nanoarrow/nanoarrow.h>
#include <task_result.h>

#include <cstdint>
#include <cstring>
#include <nanoarrow/nanoarrow.hpp>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;
using namespace dftracer::utils::dataframe::test_types;

namespace {

using Strings = dstd::Vector<dstd::String>;
using Opt = dstd::Optional<dstd::String>;

const char* const LONG_A = "hello world long str";
const char* const LONG_B = "another rather long string";

Cells cells_of(const Strings& rows) {
    Cells cells;
    for (const dstd::String& s : rows) cells.push_back(s);
    return cells;
}

Series view_of(const Strings& rows) {
    Series flat = make_string(cells_of(rows));
    ViewBuilder builder;
    builder.append_column(*flat.handle());
    return builder.finish(TypeId::String, false);
}

Series list_of(dstd::Vector<std::int32_t> offsets, Series child) {
    return Series::list(offsets, std::move(child));
}

Series struct_of(dstd::CStringView name, Series field) {
    dstd::Vector<Series> fields;
    fields.push_back(std::move(field));
    return Series::structs({name.c_str()}, std::move(fields));
}

Series chunked_of(Series a, Series b) {
    dstd::Vector<Series> parts;
    parts.push_back(std::move(a));
    parts.push_back(std::move(b));
    return dftu_utils_test::ok(chunked_column(std::move(parts)));
}

Series codes_of(dstd::Vector<std::int32_t> codes) {
    return Series::flat(TypeId::Int32, codes.data(),
                        static_cast<std::int64_t>(codes.size()));
}

/// The array has the buffers and children that its schema says, down to the
/// last child.
void check_described(const ArrowSchema* schema, const ArrowArray* array) {
    nanoarrow::UniqueArrayView view;
    ArrowError error{};
    REQUIRE(ArrowArrayViewInitFromSchema(view.get(), schema, &error) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayViewSetArray(view.get(), array, &error) == NANOARROW_OK);
    CHECK(ArrowArrayViewValidate(view.get(), NANOARROW_VALIDATION_LEVEL_FULL,
                                 &error) == NANOARROW_OK);
}

/// Exports `col`, checks the schema describes the array, and reads it back.
Series round_trip(const Series& col) {
    arrow::Batch batch = exported(col);
    check_described(batch.schema(), batch.array());
    return import_series(batch.schema(), batch.array());
}

/// Dictionary of `values` (the first may be null) over `codes`.
Series dict_of(dstd::Vector<std::int32_t> codes, Cells values) {
    return dftu_utils_test::ok(
        dictionary_of(codes_of(std::move(codes)), make_string(values)));
}

/// A FixedSizeList of `width` over `child`.
Series fixed_list_of(std::int32_t width, Series child) {
    auto* col = new_column(TypeId::FixedSizeList, child.length() / width);
    col->set_fixed_size(width);
    col->set_child(adopt_series(child.release()));
    return Series{col};
}

/// A Map of int32 `offsets` over entries of a string key and `value`.
Series map_of(dstd::Span<const std::int32_t> offsets, Series key,
              Series value) {
    auto* col =
        new_column(TypeId::Map, static_cast<std::int64_t>(offsets.size()) - 1);
    col->offsets = new_buffer(offsets.size() * sizeof(std::int32_t));
    std::memcpy(writable(col->offsets), offsets.data(),
                offsets.size() * sizeof(std::int32_t));
    dstd::Vector<Series> fields;
    fields.push_back(std::move(key));
    fields.push_back(std::move(value));
    col->set_child(adopt_series(
        Series::structs({"key", "value"}, std::move(fields)).release()));
    return Series{col};
}

/// The child schema at `path` of `schema`, children by index.
const ArrowSchema* schema_at(const ArrowSchema* schema,
                             std::initializer_list<int> path) {
    for (int i : path) schema = schema->children[i];
    return schema;
}

struct StreamGuard {
    ArrowArrayStream stream{};
    ~StreamGuard() {
        if (stream.release != nullptr) stream.release(&stream);
    }
};

/// The text under the first column of every batch (a struct field or a list's
/// values), in order.
Strings child_text(dstd::Span<const Series> batches) {
    Strings out;
    for (const Series& b : batches) {
        const Series kids = b.child(0).child(0);
        for (std::int64_t i = 0; i < kids.length(); ++i) {
            const dstd::StringView text = kids.string_at(i);
            out.push_back(dstd::String(text.data(), text.size()));
        }
    }
    return out;
}

/// Every batch of `stream` has the buffers and children of its schema; the rows
/// come back as one Series of the batches joined.
dstd::Vector<Series> read_batches(ArrowArrayStream* stream) {
    nanoarrow::UniqueSchema schema;
    REQUIRE(stream->get_schema(stream, schema.get()) == 0);
    dstd::Vector<Series> out;
    for (;;) {
        nanoarrow::UniqueArray array;
        REQUIRE(stream->get_next(stream, array.get()) == 0);
        if (array->release == nullptr) break;
        check_described(schema.get(), array.get());
        out.push_back(import_series(schema.get(), array.get()));
    }
    return out;
}

}  // namespace

TEST_SUITE("chunked nested export") {
    TEST_CASE("list parts with string_view children export their text") {
        const dstd::Vector<std::int32_t> off_a{0, 1, 2};
        const dstd::Vector<std::int32_t> off_b{0, 1};
        Series col = chunked_of(list_of(off_a, view_of({LONG_A, "short"})),
                                list_of(off_b, view_of({LONG_B})));
        REQUIRE(col.encoding() == Encoding::Chunked);
        Series back = round_trip(col);
        REQUIRE(back.type() == TypeId::List);
        REQUIRE(back.length() == 3);
        Series kids = back.child(0);
        REQUIRE(kids.length() == 3);
        CHECK(kids.string_at(0) == LONG_A);
        CHECK(kids.string_at(1) == "short");
        CHECK(kids.string_at(2) == LONG_B);
    }

    TEST_CASE("list parts that differ in child layout export their text") {
        const dstd::Vector<std::int32_t> off{0, 1};
        Series col =
            chunked_of(list_of(off, view_of({LONG_A})),
                       list_of(off, make_string(Cells{dstd::String(LONG_B)})));
        REQUIRE(col.encoding() == Encoding::Chunked);
        Series back = round_trip(col);
        Series kids = back.child(0);
        REQUIRE(kids.length() == 2);
        CHECK(kids.string_at(0) == LONG_A);
        CHECK(kids.string_at(1) == LONG_B);
    }

    TEST_CASE("list parts with null rows and string_view children") {
        const dstd::Vector<std::int32_t> off_a{0, 1, 1};
        const dstd::Vector<std::int32_t> off_b{0, 1};
        Series a = list_of(off_a, view_of({LONG_A}));
        const std::uint8_t bits = 0b01;
        set_validity(a.handle(), &bits, 2);
        Series col =
            chunked_of(std::move(a), list_of(off_b, view_of({LONG_B})));
        Series back = round_trip(col);
        REQUIRE(back.length() == 3);
        CHECK_FALSE(back.is_null(0));
        CHECK(back.is_null(1));
        CHECK_FALSE(back.is_null(2));
        CHECK(back.child(0).string_at(1) == LONG_B);
    }

    TEST_CASE("struct parts with a string_view field export their text") {
        Series col = chunked_of(struct_of("a", view_of({LONG_A, "short"})),
                                struct_of("a", view_of({LONG_B})));
        REQUIRE(col.encoding() == Encoding::Chunked);
        Series back = round_trip(col);
        REQUIRE(back.type() == TypeId::Struct);
        Series field = back.child(0);
        REQUIRE(field.length() == 3);
        CHECK(field.string_at(0) == LONG_A);
        CHECK(field.string_at(1) == "short");
        CHECK(field.string_at(2) == LONG_B);
    }

    TEST_CASE("string_view parts export their text") {
        Series col = chunked_of(view_of({LONG_A, "short"}), view_of({LONG_B}));
        REQUIRE(col.encoding() == Encoding::Chunked);
        Series back = round_trip(col);
        REQUIRE(back.length() == 3);
        CHECK(back.string_at(0) == LONG_A);
        CHECK(back.string_at(1) == "short");
        CHECK(back.string_at(2) == LONG_B);
    }

    TEST_CASE("dictionary parts export as one dictionary") {
        Series a = dftu_utils_test::ok(dictionary_of(
            codes_of({0, 1, 0}),
            make_large_utf8(Cells{dstd::String("x"), dstd::String("y")})));
        Series b = dftu_utils_test::ok(dictionary_of(
            codes_of({1}),
            make_large_utf8(Cells{dstd::String("z"), dstd::String("x")})));
        Series col = chunked_of(std::move(a), std::move(b));
        REQUIRE(col.encoding() == Encoding::Chunked);
        arrow::Batch batch = exported(col);
        check_described(batch.schema(), batch.array());
        REQUIRE(batch.schema()->dictionary != nullptr);
        CHECK(dstd::StringView{batch.schema()->dictionary->format} == "U");
        Series back = import_series(batch.schema(), batch.array());
        REQUIRE(back.length() == 4);
        CHECK(back.type() == TypeId::LargeString);
        const char* want[] = {"x", "y", "x", "x"};
        for (std::int64_t i = 0; i < 4; ++i)
            CHECK(back.string_at(i) == want[i]);
    }

    TEST_CASE("narrow dictionary parts export as one dictionary of text") {
        Series col =
            chunked_of(dict_of({0, 1}, Cells{dstd::String("x"), Opt{}}),
                       dict_of({0}, Cells{dstd::String("y")}));
        REQUIRE(col.encoding() == Encoding::Chunked);
        arrow::Batch batch = exported(col);
        check_described(batch.schema(), batch.array());
        REQUIRE(batch.schema()->dictionary != nullptr);
        CHECK(dstd::StringView{batch.schema()->dictionary->format} == "u");
        Series back = import_series(batch.schema(), batch.array());
        REQUIRE(back.length() == 3);
        CHECK(back.string_at(0) == "x");
        CHECK(back.is_null(1));
        CHECK(back.string_at(2) == "y");
    }

    TEST_CASE("a list over a dictionary exports the dictionary in its schema") {
        const dstd::Vector<std::int32_t> off{0, 2, 3};
        const dstd::Vector<std::int64_t> off64{0, 2, 3};
        Series lists[] = {
            list_of(off, dict_of({0, 1, 0}, Cells{dstd::String("p"), Opt{}})),
            make_large_list(
                off64, dict_of({0, 1, 0}, Cells{dstd::String("p"), Opt{}})),
            fixed_list_of(1,
                          dict_of({0, 1, 0}, Cells{dstd::String("p"), Opt{}}))};
        for (const Series& col : lists) {
            arrow::Batch batch = exported(col);
            check_described(batch.schema(), batch.array());
            CHECK(schema_at(batch.schema(), {0})->dictionary != nullptr);
            Series back = import_series(batch.schema(), batch.array());
            REQUIRE(back.length() == col.length());
            CHECK(back.child(0).string_at(0) == "p");
            CHECK(back.child(0).is_null(1));
        }
    }

    TEST_CASE("a map over a dictionary exports the dictionary in its schema") {
        const dstd::Vector<std::int32_t> off{0, 1, 2};
        Series col = map_of(
            off, make_string(Cells{dstd::String("k"), dstd::String("l")}),
            dict_of({0, 1}, Cells{dstd::String("p"), Opt{}}));
        arrow::Batch batch = exported(col);
        check_described(batch.schema(), batch.array());
        CHECK(schema_at(batch.schema(), {0, 1})->dictionary != nullptr);
    }

    TEST_CASE("list parts over dictionaries export one dictionary of text") {
        const dstd::Vector<std::int32_t> off_a{0, 2};
        const dstd::Vector<std::int32_t> off_b{0, 2};
        Series col = chunked_of(
            list_of(off_a, dict_of({0, 1}, Cells{dstd::String("p"), Opt{}})),
            list_of(off_b, dict_of({0, 0}, Cells{dstd::String("q")})));
        REQUIRE(col.encoding() == Encoding::Chunked);
        arrow::Batch batch = exported(col);
        check_described(batch.schema(), batch.array());
        const ArrowSchema* item = schema_at(batch.schema(), {0});
        REQUIRE(item->dictionary != nullptr);
        CHECK(dstd::StringView{item->dictionary->format} == "u");
        Series kids = import_series(batch.schema(), batch.array()).child(0);
        REQUIRE(kids.length() == 4);
        CHECK(kids.string_at(0) == "p");
        CHECK(kids.is_null(1));
        CHECK(kids.string_at(2) == "q");
        CHECK(kids.string_at(3) == "q");
    }

    TEST_CASE("struct parts over dictionaries keep the text value type") {
        Series col = chunked_of(
            struct_of("a", dict_of({0, 1}, Cells{dstd::String("p"), Opt{}})),
            struct_of("a", dict_of({0}, Cells{dstd::String("q")})));
        REQUIRE(col.encoding() == Encoding::Chunked);
        arrow::Batch batch = exported(col);
        check_described(batch.schema(), batch.array());
        const ArrowSchema* field = schema_at(batch.schema(), {0});
        REQUIRE(field->dictionary != nullptr);
        CHECK(dstd::StringView{field->dictionary->format} == "u");
        Series back = import_series(batch.schema(), batch.array()).child(0);
        REQUIRE(back.length() == 3);
        CHECK(back.string_at(0) == "p");
        CHECK(back.is_null(1));
        CHECK(back.string_at(2) == "q");
    }

    TEST_CASE("a list whose child is chunked strings exports their text") {
        const dstd::Vector<std::int32_t> off{0, 1, 2};
        Series child = chunked_of(view_of({LONG_A}), view_of({LONG_B}));
        REQUIRE(child.encoding() == Encoding::Chunked);
        Series col = list_of(off, std::move(child));
        Series back = round_trip(col);
        REQUIRE(back.length() == 2);
        Series kids = back.child(0);
        CHECK(kids.string_at(0) == LONG_A);
        CHECK(kids.string_at(1) == LONG_B);
    }

    TEST_CASE("a list whose child is chunked structs exports their fields") {
        const dstd::Vector<std::int32_t> off{0, 1, 2};
        Series child = chunked_of(struct_of("a", view_of({LONG_A})),
                                  struct_of("a", view_of({LONG_B})));
        REQUIRE(child.encoding() == Encoding::Chunked);
        Series col = list_of(off, std::move(child));
        Series back = round_trip(col);
        REQUIRE(back.length() == 2);
        Series field = back.child(0).child(0);
        CHECK(field.string_at(0) == LONG_A);
        CHECK(field.string_at(1) == LONG_B);
    }

    TEST_CASE("a frame of two batches round trips string_view columns") {
        DataFrame frame;
        frame.names = dstd::make_shared_strings({"s", "st"});
        frame.columns.push_back(
            chunked_of(view_of({LONG_A, "short"}), view_of({LONG_B})));
        frame.columns.push_back(
            chunked_of(struct_of("a", view_of({LONG_A, "short"})),
                       struct_of("a", view_of({LONG_B}))));

        arrow::Batch batch = exported(frame);
        check_described(batch.schema(), batch.array());
        DataFrame back = import_frame(batch.schema(), batch.array());
        REQUIRE(back.num_rows() == 3);
        CHECK(back.columns[0].string_at(0) == LONG_A);
        CHECK(back.columns[0].string_at(2) == LONG_B);
        CHECK(back.columns[1].child(0).string_at(0) == LONG_A);
        CHECK(back.columns[1].child(0).string_at(2) == LONG_B);

        ArrowArrayStream stream{};
        REQUIRE(arrow::to_arrow_stream(frame, &stream).has_value());
        nanoarrow::UniqueSchema schema;
        REQUIRE(stream.get_schema(&stream, schema.get()) == 0);
        std::int64_t rows = 0;
        for (;;) {
            nanoarrow::UniqueArray array;
            REQUIRE(stream.get_next(&stream, array.get()) == 0);
            if (array->release == nullptr) break;
            check_described(schema.get(), array.get());
            rows += array->length;
        }
        CHECK(rows == 3);
        stream.release(&stream);
    }

    TEST_CASE("a column stream of nested parts that differ in layout") {
        const dstd::Vector<std::int32_t> off{0, 1};
        Series col =
            chunked_of(list_of(off, view_of({LONG_A})),
                       list_of(off, make_string(Cells{dstd::String(LONG_B)})));
        ArrowArrayStream stream{};
        REQUIRE(arrow::to_arrow_stream(col, &stream).has_value());
        nanoarrow::UniqueSchema schema;
        REQUIRE(stream.get_schema(&stream, schema.get()) == 0);
        for (;;) {
            nanoarrow::UniqueArray array;
            REQUIRE(stream.get_next(&stream, array.get()) == 0);
            if (array->release == nullptr) break;
            check_described(schema.get(), array.get());
        }
        stream.release(&stream);
    }

    TEST_CASE("list parts with binary children export their bytes") {
        const dstd::Vector<std::int32_t> off_a{0, 2};
        const dstd::Vector<std::int32_t> off_b{0, 1};
        Series col = chunked_of(
            list_of(off_a, make_binary(Cells{dstd::String("ab"), Opt{}})),
            list_of(off_b, make_binary(Cells{dstd::String("cd")})));
        REQUIRE(col.encoding() == Encoding::Chunked);
        Series back = round_trip(col);
        Series kids = back.child(0);
        REQUIRE(kids.type() == TypeId::Binary);
        REQUIRE(kids.length() == 3);
        CHECK(kids.string_at(0) == "ab");
        CHECK(kids.is_null(1));
        CHECK(kids.string_at(2) == "cd");
    }

    TEST_CASE("list parts with large_binary children export their bytes") {
        const dstd::Vector<std::int64_t> off{0, 1};
        Series col = chunked_of(
            make_large_list(off, make_large_binary(Cells{dstd::String("ab")})),
            make_large_list(off, make_large_binary(Cells{dstd::String("cd")})));
        Series back = round_trip(col);
        Series kids = back.child(0);
        REQUIRE(kids.type() == TypeId::LargeBinary);
        REQUIRE(kids.length() == 2);
        CHECK(kids.string_at(0) == "ab");
        CHECK(kids.string_at(1) == "cd");
    }

    TEST_CASE("list parts with fixed_size_binary children export their bytes") {
        const dstd::Vector<std::int32_t> off{0, 2};
        Series col = chunked_of(
            list_of(off, make_fixed_size_binary(Strings{"abcd", "efgh"}, 4)),
            list_of(off, make_fixed_size_binary(Strings{"wxyz", "0123"}, 4)));
        REQUIRE(col.encoding() == Encoding::Chunked);
        Series back = round_trip(col);
        Series kids = back.child(0);
        REQUIRE(kids.type() == TypeId::FixedSizeBinary);
        REQUIRE(kids.length() == 4);
        CHECK(std::memcmp(kids.data<char>(), "abcdefghwxyz0123", 16) == 0);
    }

    TEST_CASE("fixed_size_binary parts export their bytes") {
        Series col =
            chunked_of(make_fixed_size_binary(Strings{"abcd", "efgh"}, 4),
                       make_fixed_size_binary(Strings{"wxyz"}, 4));
        Series back = round_trip(col);
        REQUIRE(back.type() == TypeId::FixedSizeBinary);
        REQUIRE(back.length() == 3);
        CHECK(std::memcmp(back.data<char>(), "abcdefghwxyz", 12) == 0);
    }

    TEST_CASE("a frame stream of batches of two child layouts has one schema") {
        const std::int64_t rows = common::constants::dataframe::BATCH_SIZE;
        Strings texts;
        dstd::Vector<std::int32_t> off{0};
        for (std::int64_t i = 0; i < rows; ++i) {
            texts.push_back(dstd::String(LONG_A));
            off.push_back(static_cast<std::int32_t>(i + 1));
        }
        const dstd::Vector<std::int32_t> tail_off{0, 1, 2};
        Series col = chunked_of(
            list_of(off, view_of(texts)),
            list_of(tail_off, make_string(Cells{dstd::String(LONG_B),
                                                dstd::String("short")})));
        REQUIRE(col.encoding() == Encoding::Chunked);
        DataFrame frame;
        frame.names = dstd::make_shared_strings({"l"});
        frame.columns.push_back(std::move(col));
        StreamGuard out;
        REQUIRE(arrow::to_arrow_stream(frame, &out.stream).has_value());
        dstd::Vector<Series> batches = read_batches(&out.stream);
        std::int64_t total = 0;
        for (const Series& b : batches) total += b.length();
        REQUIRE(total == rows + 2);
        const Series& last = batches.back();
        CHECK(last.child(0).child(0).string_at(last.length() - 2) == LONG_B);
        CHECK(last.child(0).child(0).string_at(last.length() - 1) == "short");
    }

    TEST_CASE("a lazy concat of struct layouts streams under one schema") {
        auto views = [] {
            DataFrame frame;
            frame.names = dstd::make_shared_strings({"s"});
            frame.columns.push_back(struct_of("a", view_of({LONG_A, "short"})));
            return frame;
        };
        auto plain = [] {
            DataFrame frame;
            frame.names = dstd::make_shared_strings({"s"});
            frame.columns.push_back(struct_of(
                "a",
                make_string(Cells{dstd::String(LONG_B), dstd::String("x")})));
            return frame;
        };
        for (bool views_first : {true, false}) {
            LazyFrame first =
                LazyFrame::scan(dstd::make_shared<const InMemorySource>(
                    views_first ? views() : plain()));
            LazyFrame second =
                LazyFrame::scan(dstd::make_shared<const InMemorySource>(
                    views_first ? plain() : views()));
            StreamGuard out;
            REQUIRE(arrow::to_arrow_stream(first.concat(std::move(second)),
                                           default_runtime(), &out.stream)
                        .has_value());
            dstd::Vector<Series> batches = read_batches(&out.stream);
            const Strings want = views_first
                                     ? Strings{LONG_A, "short", LONG_B, "x"}
                                     : Strings{LONG_B, "x", LONG_A, "short"};
            CHECK(child_text(batches) == want);
        }
    }

    TEST_CASE("a frame stream of dictionary and plain list children") {
        const std::int64_t rows = common::constants::dataframe::BATCH_SIZE;
        dstd::Vector<std::int32_t> off{0};
        dstd::Vector<std::int32_t> codes;
        for (std::int64_t i = 0; i < rows; ++i) {
            off.push_back(static_cast<std::int32_t>(i + 1));
            codes.push_back(static_cast<std::int32_t>(i % 2));
        }
        const dstd::Vector<std::int32_t> tail_off{0, 1, 2};
        for (bool dictionary_first : {true, false}) {
            Series wide = list_of(
                off,
                dict_of(codes, Cells{dstd::String(LONG_A), dstd::String("p")}));
            Series tail = list_of(
                tail_off,
                make_string(Cells{dstd::String(LONG_B), dstd::String("x")}));
            Series col = dictionary_first
                             ? chunked_of(std::move(wide), std::move(tail))
                             : chunked_of(std::move(tail), std::move(wide));
            REQUIRE(col.encoding() == Encoding::Chunked);
            DataFrame frame;
            frame.names = dstd::make_shared_strings({"l"});
            frame.columns.push_back(std::move(col));
            StreamGuard out;
            REQUIRE(arrow::to_arrow_stream(frame, &out.stream).has_value());
            dstd::Vector<Series> batches = read_batches(&out.stream);
            std::int64_t total = 0;
            for (const Series& b : batches) total += b.length();
            REQUIRE(total == rows + 2);
            const Series& last = batches.back();
            const Series kids = last.child(0).child(0);
            if (dictionary_first) {
                CHECK(kids.string_at(kids.length() - 2) == LONG_B);
                CHECK(kids.string_at(kids.length() - 1) == "x");
            } else {
                CHECK(batches.front().child(0).child(0).string_at(0) == LONG_B);
                CHECK(kids.string_at(kids.length() - 1) == "p");
            }
        }
    }

    TEST_CASE("a frame stream of dictionary and plain top level text") {
        const std::int64_t rows = common::constants::dataframe::BATCH_SIZE;
        dstd::Vector<std::int32_t> codes;
        for (std::int64_t i = 0; i < rows; ++i)
            codes.push_back(static_cast<std::int32_t>(i % 2));
        for (bool dictionary_first : {true, false}) {
            Series wide =
                dict_of(codes, Cells{dstd::String(LONG_A), dstd::String("p")});
            Series tail =
                make_string(Cells{dstd::String(LONG_B), dstd::String("x")});
            dstd::Vector<Series> parts;
            parts.push_back(dictionary_first ? std::move(wide)
                                             : std::move(tail));
            parts.push_back(dictionary_first ? std::move(tail)
                                             : std::move(wide));
            DataFrame frame;
            frame.names = dstd::make_shared_strings({"s"});
            frame.columns.push_back(
                dftu_utils_test::ok(chunked_column(std::move(parts))));
            StreamGuard out;
            REQUIRE(arrow::to_arrow_stream(frame, &out.stream).has_value());
            dstd::Vector<Series> batches = read_batches(&out.stream);
            std::int64_t total = 0;
            for (const Series& b : batches) total += b.length();
            CHECK(total == rows + 2);
        }
    }

    TEST_CASE("a lazy concat of dictionary and plain struct and list fields") {
        auto framed = [](Series column) {
            DataFrame frame;
            frame.names = dstd::make_shared_strings({"c"});
            frame.columns.push_back(std::move(column));
            return frame;
        };
        const dstd::Vector<std::int32_t> off{0, 1, 2};
        auto dictionary = [] {
            return dict_of({0, 1},
                           Cells{dstd::String(LONG_A), dstd::String("p")});
        };
        auto plain = [] {
            return make_string(Cells{dstd::String(LONG_B), dstd::String("x")});
        };
        for (bool as_list : {false, true}) {
            auto shape = [&](Series v) {
                return as_list ? list_of(off, std::move(v))
                               : struct_of("a", std::move(v));
            };
            for (bool dictionary_first : {true, false}) {
                LazyFrame first = LazyFrame::scan(
                    dstd::make_shared<const InMemorySource>(framed(
                        shape(dictionary_first ? dictionary() : plain()))));
                LazyFrame second = LazyFrame::scan(
                    dstd::make_shared<const InMemorySource>(framed(
                        shape(dictionary_first ? plain() : dictionary()))));
                StreamGuard out;
                REQUIRE(arrow::to_arrow_stream(first.concat(std::move(second)),
                                               default_runtime(), &out.stream)
                            .has_value());
                dstd::Vector<Series> batches = read_batches(&out.stream);
                const Strings want = dictionary_first
                                         ? Strings{LONG_A, "p", LONG_B, "x"}
                                         : Strings{LONG_B, "x", LONG_A, "p"};
                CHECK(child_text(batches) == want);
            }
        }
    }

    TEST_CASE("chunks that share one dictionary export it without a copy") {
        Series whole = dict_of(
            {0, 1, 1, 0, 1}, Cells{dstd::String(LONG_A), dstd::String(LONG_B)});
        const dftu_series* values = whole.handle()->child().get();
        Series col = chunked_of(whole.slice(0, 2), whole.slice(2, 3));
        REQUIRE(col.encoding() == Encoding::Chunked);
        arrow::Batch batch = exported(col);
        check_described(batch.schema(), batch.array());
        const ArrowArray* dictionary = batch.array()->dictionary;
        REQUIRE(dictionary != nullptr);
        CHECK(dictionary->length == values->length);
        CHECK(dictionary->buffers[1] == values->offsets.data());
        CHECK(dictionary->buffers[2] == values->data.data());
        Series back = import_series(batch.schema(), batch.array());
        REQUIRE(back.length() == 5);
        const char* want[] = {LONG_A, LONG_B, LONG_B, LONG_A, LONG_B};
        for (std::int64_t i = 0; i < 5; ++i)
            CHECK(back.string_at(i) == want[i]);
    }

    TEST_CASE("a lazy concat of list layouts streams under one schema") {
        const dstd::Vector<std::int32_t> off{0, 1};
        DataFrame views;
        views.names = dstd::make_shared_strings({"l"});
        views.columns.push_back(list_of(off, view_of({LONG_A})));
        DataFrame plain;
        plain.names = dstd::make_shared_strings({"l"});
        plain.columns.push_back(
            list_of(off, make_string(Cells{dstd::String(LONG_B)})));
        LazyFrame first = LazyFrame::scan(
            dstd::make_shared<const InMemorySource>(std::move(views)));
        LazyFrame second = LazyFrame::scan(
            dstd::make_shared<const InMemorySource>(std::move(plain)));
        LazyFrame both = first.concat(std::move(second));
        StreamGuard out;
        REQUIRE(arrow::to_arrow_stream(std::move(both), default_runtime(),
                                       &out.stream)
                    .has_value());
        dstd::Vector<Series> batches = read_batches(&out.stream);
        CHECK(child_text(batches) == Strings{LONG_A, LONG_B});
    }
}
