// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/buffers.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/validate.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

dftu_series* new_column(TypeId type, std::int64_t n) {
    auto* col = new dftu_series();
    col->type = type;
    col->encoding = Encoding::Flat;
    col->length = n;
    return col;
}

void set_data(dftu_series* col, const void* bytes, std::size_t n) {
    col->data = new_buffer(n);
    std::memcpy(writable(col->data), bytes, n);
}

void set_offsets(dftu_series* col, const void* bytes, std::size_t n) {
    col->offsets = new_buffer(n);
    std::memcpy(writable(col->offsets), bytes, n);
}

void set_validity(dftu_series* col, const std::uint8_t* validity,
                  std::int64_t n) {
    const std::size_t bytes = static_cast<std::size_t>((n + 7) / 8);
    col->validity = new_buffer(bytes);
    std::memcpy(writable(col->validity), validity, bytes);
    std::int64_t nulls = 0;
    for (std::int64_t i = 0; i < n; ++i)
        nulls += ((validity[i >> 3] >> (i & 7)) & 1) == 0;
    col->null_count = nulls;
}

dstd::String message(const dstd::Result<void>& r) {
    REQUIRE_FALSE(r.has_value());
    return dstd::String(r.error().message());
}

bool passes(const dstd::Result<void>& r) {
    if (!r.has_value()) MESSAGE("unexpected error: " << r.error().message());
    return r.has_value();
}

bool names(const dstd::Result<void>& r, dstd::StringView column,
           dstd::StringView rule) {
    if (r.has_value()) {
        MESSAGE("expected an error naming '" << column << "' and '" << rule
                                             << "'");
        return false;
    }
    const dstd::StringView got = r.error().message();
    const bool ok = got.find(column) != dstd::StringView::npos &&
                    got.find(rule) != dstd::StringView::npos;
    if (!ok) MESSAGE("error was: " << got);
    return ok;
}

// A VIEW column over one inline value and one value in a 16 byte buffer.
dftu_series* new_view_column(std::uint8_t* views, std::int64_t n,
                             dstd::Vector<BufferView> blobs) {
    auto* col = new_column(TypeId::String, n);
    col->encoding = Encoding::View;
    col->data = borrow_buffer(views, static_cast<std::size_t>(n) * 16, {});
    col->blobs =
        dstd::make_shared<const dstd::Vector<BufferView>>(std::move(blobs));
    return col;
}

void write_view(std::uint8_t* view, std::int32_t size, std::int32_t index,
                std::int32_t offset) {
    std::memcpy(view, &size, sizeof(size));
    std::memset(view + 4, 'x', 4);
    std::memcpy(view + 8, &index, sizeof(index));
    std::memcpy(view + 12, &offset, sizeof(offset));
}

}  // namespace

TEST_CASE("validate: a valid column of every encoding passes") {
    SUBCASE("flat fixed width, with a null") {
        const std::int64_t values[] = {1, 0, 3};
        const std::uint8_t validity[] = {0x05};
        auto* col = new_column(TypeId::Int64, 3);
        set_data(col, values, sizeof(values));
        set_validity(col, validity, 3);
        CHECK(passes(validate(Series{col})));
    }

    SUBCASE("flat bool is bit packed") {
        const std::uint8_t bits[] = {0x05};
        auto* col = new_column(TypeId::Bool, 3);
        set_data(col, bits, sizeof(bits));
        CHECK(passes(validate(Series{col})));
    }

    SUBCASE("flat string") {
        Series col = Series::strings(dstd::Vector<dstd::String>{
            dstd::String("ab"), dstd::String("cde")});
        CHECK(passes(validate(col)));
    }

    SUBCASE("flat large string, int64 offsets") {
        const std::int64_t offsets[] = {0, 2, 5};
        auto* col = new_column(TypeId::LargeString, 2);
        set_offsets(col, offsets, sizeof(offsets));
        set_data(col, "abcde", 5);
        CHECK(passes(validate(Series{col})));
    }

    SUBCASE("constant") {
        const std::int64_t value = 7;
        auto* col = new_column(TypeId::Int64, 10);
        col->encoding = Encoding::Constant;
        set_data(col, &value, sizeof(value));
        CHECK(passes(validate(Series{col})));
    }

    SUBCASE("view") {
        std::uint8_t views[32] = {};
        write_view(views, 3, 0, 0);
        write_view(views + 16, 20, 0, 0);
        std::uint8_t data[20] = {};
        dstd::Vector<BufferView> blobs;
        blobs.push_back(borrow_buffer(data, sizeof(data), {}));
        CHECK(passes(
            validate(Series{new_view_column(views, 2, std::move(blobs))})));
    }

    SUBCASE("dictionary") {
        Series col = Series::strings(dstd::Vector<dstd::String>{
                                         dstd::String("ab"), dstd::String("cd"),
                                         dstd::String("ab")})
                         .dictionary_encode();
        CHECK(passes(validate(col)));
    }

    SUBCASE("selection") {
        const std::int64_t values[] = {1, 2, 3};
        Series col = Series::flat_i64(values, 3).slice(1, 2);
        CHECK(passes(validate(col)));
    }

    SUBCASE("chunked") {
        const std::int64_t a[] = {1, 2};
        const std::int64_t b[] = {3};
        dstd::Vector<Series> parts;
        parts.push_back(Series::flat_i64(a, 2));
        parts.push_back(Series::flat_i64(b, 1));
        auto joined = chunked_column(std::move(parts));
        REQUIRE(joined.has_value());
        CHECK(passes(validate(*joined)));
    }

    SUBCASE("struct") {
        const std::int64_t values[] = {1, 2};
        dstd::Vector<dstd::String> fields;
        fields.push_back(dstd::String("a"));
        dstd::Vector<Series> columns;
        columns.push_back(Series::flat_i64(values, 2));
        CHECK(passes(
            validate(Series::structs(std::move(fields), std::move(columns)))));
    }

    SUBCASE("list") {
        dstd::Vector<std::int32_t> offsets{0, 1, 3};
        const std::int64_t values[] = {1, 2, 3};
        CHECK(passes(
            validate(Series::list(offsets, Series::flat_i64(values, 3)))));
    }
}

TEST_CASE("validate: a broken column names the column and the rule") {
    SUBCASE("negative length") {
        auto* col = new_column(TypeId::Int64, -1);
        CHECK(names(validate(Series{col}), "unnamed", "is negative"));
    }

    SUBCASE("validity bitmap too short") {
        const std::uint8_t validity[] = {0x01};
        auto* col = new_column(TypeId::Int64, 20);
        col->validity = new_buffer(1);
        std::memcpy(writable(col->validity), validity, 1);
        col->null_count = 19;
        CHECK(names(validate(Series{col}), "validity bitmap", "rows need"));
    }

    SUBCASE("null count does not match the bitmap") {
        const std::int64_t values[] = {1, 2, 3};
        const std::uint8_t validity[] = {0x05};
        auto* col = new_column(TypeId::Int64, 3);
        set_data(col, values, sizeof(values));
        col->validity = new_buffer(1);
        std::memcpy(writable(col->validity), validity, 1);
        col->null_count = 0;
        CHECK(names(validate(Series{col}), "null count", "clears"));
    }

    SUBCASE("flat data buffer is too small") {
        const std::int64_t values[] = {1};
        auto* col = new_column(TypeId::Int64, 3);
        set_data(col, values, sizeof(values));
        CHECK(names(validate(Series{col}), "data buffer", "need 24"));
    }

    SUBCASE("a string column without offsets") {
        auto* col = new_column(TypeId::String, 2);
        set_data(col, "abc", 3);
        CHECK(names(validate(Series{col}), "offset buffer", "string"));
    }

    SUBCASE("a string column with too few offset entries") {
        const std::int32_t offsets[] = {0, 1};
        auto* col = new_column(TypeId::String, 3);
        set_offsets(col, offsets, sizeof(offsets));
        set_data(col, "abc", 3);
        CHECK(names(validate(Series{col}), "offset buffer", "4 entries"));
    }

    SUBCASE("offsets that go backwards") {
        const std::int32_t offsets[] = {0, 3, 2, 3};
        auto* col = new_column(TypeId::String, 3);
        set_offsets(col, offsets, sizeof(offsets));
        set_data(col, "abc", 3);
        CHECK(names(validate(Series{col}), "offset 2", "before it is 3"));
    }

    SUBCASE("an offset past the data bytes") {
        const std::int32_t offsets[] = {0, 2, 9};
        auto* col = new_column(TypeId::String, 2);
        set_offsets(col, offsets, sizeof(offsets));
        set_data(col, "abc", 3);
        CHECK(names(validate(Series{col}), "last offset", "holds 3"));
    }

    SUBCASE("large string offsets are read as int64") {
        const std::int32_t narrow[] = {0, 1, 2};
        auto* col = new_column(TypeId::LargeString, 2);
        set_offsets(col, narrow, 2 * sizeof(std::int64_t));
        set_data(col, "ab", 2);
        CHECK(names(validate(Series{col}), "offset buffer",
                    "3 entries of 8 bytes"));
    }

    SUBCASE("a view with a negative length") {
        std::uint8_t views[16] = {};
        write_view(views, -1, 0, 0);
        CHECK(names(validate(Series{new_view_column(views, 1, {})}), "row 0",
                    "length"));
    }

    SUBCASE("a view naming a buffer the column does not have") {
        std::uint8_t views[16] = {};
        write_view(views, 20, 3, 0);
        CHECK(names(validate(Series{new_view_column(views, 1, {})}), "buffer 3",
                    "it has 0"));
    }

    SUBCASE("a view reading past its buffer") {
        std::uint8_t views[16] = {};
        write_view(views, 40, 0, 0);
        std::uint8_t data[16] = {};
        dstd::Vector<BufferView> blobs;
        blobs.push_back(borrow_buffer(data, sizeof(data), {}));
        CHECK(
            names(validate(Series{new_view_column(views, 1, std::move(blobs))}),
                  "reads 40 bytes", "16 byte buffer"));
    }

    SUBCASE("a view buffer too small for its rows") {
        std::uint8_t views[16] = {};
        auto* col = new_column(TypeId::String, 2);
        col->encoding = Encoding::View;
        col->data = borrow_buffer(views, sizeof(views), {});
        CHECK(names(validate(Series{col}), "view buffer", "32"));
    }

    SUBCASE("a dictionary code outside its values") {
        const std::int32_t codes[] = {0, 5};
        auto* col = new_column(TypeId::String, 2);
        col->encoding = Encoding::Dictionary;
        set_data(col, codes, sizeof(codes));
        Series values = Series::strings(dstd::Vector<dstd::String>{
            dstd::String("abc"), dstd::String("de")});
        col->set_child(adopt_series(values.release()));
        CHECK(names(validate(Series{col}), "code 5", "2 values"));
    }

    SUBCASE("a selection index outside its base") {
        const std::int64_t index[] = {0, 9};
        auto* col = new_column(TypeId::Int64, 2);
        col->encoding = Encoding::Selection;
        set_data(col, index, sizeof(index));
        const std::int64_t values[] = {1, 2};
        Series base = Series::flat_i64(values, 2);
        col->set_child(adopt_series(base.release()));
        CHECK(names(validate(Series{col}), "index 9", "2 base rows"));
    }

    SUBCASE("a chunk start that disagrees with its chunk") {
        const std::int64_t a[] = {1, 2};
        const std::int64_t b[] = {3, 4};
        dstd::Vector<Series> parts;
        parts.push_back(Series::flat_i64(a, 2));
        parts.push_back(Series::flat_i64(b, 2));
        auto joined = chunked_column(std::move(parts));
        REQUIRE(joined.has_value());
        auto* col = joined->handle();
        auto* starts = const_cast<std::int64_t*>(
            reinterpret_cast<const std::int64_t*>(col->data.data()));
        starts[1] = 1;
        CHECK(names(validate(*joined), "chunk 0", "its start says"));
    }

    SUBCASE("chunk lengths that do not sum to the column length") {
        const std::int64_t a[] = {1, 2};
        const std::int64_t b[] = {3, 4};
        dstd::Vector<Series> parts;
        parts.push_back(Series::flat_i64(a, 2));
        parts.push_back(Series::flat_i64(b, 2));
        auto joined = chunked_column(std::move(parts));
        REQUIRE(joined.has_value());
        joined->handle()->length = 5;
        CHECK(names(validate(*joined), "rows in all", "its length is 5"));
    }

    SUBCASE("a struct field of another length") {
        const std::int64_t values[] = {1, 2, 3};
        dstd::Vector<dstd::String> fields;
        fields.push_back(dstd::String("a"));
        dstd::Vector<Series> columns;
        columns.push_back(Series::flat_i64(values, 3));
        Series col = Series::structs(std::move(fields), std::move(columns));
        col.handle()->length = 2;
        CHECK(names(validate(col), "field 'a'", "2 rows"));
    }

    SUBCASE("list offsets past the values column") {
        dstd::Vector<std::int32_t> offsets{0, 1, 3};
        const std::int64_t values[] = {1, 2};
        Series col = Series::list(offsets, Series::flat_i64(values, 2));
        CHECK(names(validate(col), "values", "holds 2"));
    }

    SUBCASE("a nested column without a child") {
        auto* col = new_column(TypeId::List, 2);
        const std::int32_t offsets[] = {0, 1, 2};
        set_offsets(col, offsets, sizeof(offsets));
        CHECK(names(validate(Series{col}), "values column", "list"));
    }

    SUBCASE("a null column") {
        CHECK(names(validate(Series{}), "unnamed", "null"));
    }
}

TEST_CASE("validate: a length no buffer could back is refused") {
    SUBCASE("a flat Int64 column of 1<<61 rows over 16 bytes") {
        const std::int64_t values[] = {1, 2};
        auto* col = new_column(TypeId::Int64, std::int64_t{1} << 61);
        set_data(col, values, sizeof(values));
        CHECK(names(validate(Series{col}), "unnamed", "too large"));
    }

    SUBCASE("a dictionary column of 1<<62 rows") {
        auto* col = new_column(TypeId::String, std::int64_t{1} << 62);
        col->encoding = Encoding::Dictionary;
        col->set_child(adopt_series(
            Series::strings(dstd::Vector<dstd::String>{dstd::String("a")})
                .release()));
        CHECK(names(validate(Series{col}), "unnamed", "too large"));
    }

    SUBCASE("a view column of 1<<60 rows") {
        auto* col = new_column(TypeId::String, std::int64_t{1} << 60);
        col->encoding = Encoding::View;
        CHECK(names(validate(Series{col}), "unnamed", "too large"));
    }

    SUBCASE("a string column of 1<<62 rows") {
        auto* col = new_column(TypeId::String, std::int64_t{1} << 62);
        CHECK(names(validate(Series{col}), "unnamed", "too large"));
    }
}

TEST_CASE("validate: a zero-row column needs no offset buffer") {
    SUBCASE("a String column built with no offsets, the way Arrow allows") {
        Series col{dftu_series_new_string_borrowed(DFTU_TYPE_STRING, nullptr,
                                                   nullptr, 0, 0, nullptr,
                                                   nullptr, nullptr)};
        REQUIRE(col.valid());
        CHECK(passes(validate(col)));
    }

    SUBCASE("a List of no rows over no values") {
        auto* col = new_column(TypeId::List, 0);
        col->set_child(adopt_series(Series::flat_i64(nullptr, 0).release()));
        CHECK(passes(validate(Series{col})));
    }

    SUBCASE("a Map of no rows over no entries") {
        auto* col = new_column(TypeId::Map, 0);
        col->set_child(adopt_series(Series::flat_i64(nullptr, 0).release()));
        CHECK(passes(validate(Series{col})));
    }
}

TEST_CASE("validate: a chunked column with an empty chunk is refused") {
    const std::int64_t a[] = {1, 2};
    const std::int64_t b[] = {3};
    dstd::Vector<Series> parts;
    parts.push_back(Series::flat_i64(a, 2));
    parts.push_back(Series::flat_i64(b, 1));
    auto joined = chunked_column(std::move(parts));
    REQUIRE(joined.has_value());
    joined->handle()->nested[1].series = {};
    CHECK(names(validate(*joined), "chunk 1", "empty"));
}

TEST_CASE("validate: frame names and column lengths") {
    const std::int64_t values[] = {1, 2};

    SUBCASE("a valid frame") {
        DataFrame frame;
        frame.names.push_back(dstd::SharedString("a"));
        frame.names.push_back(dstd::SharedString("b"));
        frame.columns.push_back(Series::flat_i64(values, 2));
        frame.columns.push_back(Series::flat_i64(values, 2));
        CHECK(passes(validate(frame)));
    }

    SUBCASE("fewer names than columns") {
        DataFrame frame;
        frame.names.push_back(dstd::SharedString("a"));
        frame.columns.push_back(Series::flat_i64(values, 2));
        frame.columns.push_back(Series::flat_i64(values, 2));
        CHECK(names(validate(frame), "frame", "1 names for 2 columns"));
    }

    SUBCASE("two columns with one name") {
        DataFrame frame;
        frame.names.push_back(dstd::SharedString("a"));
        frame.names.push_back(dstd::SharedString("a"));
        frame.columns.push_back(Series::flat_i64(values, 2));
        frame.columns.push_back(Series::flat_i64(values, 2));
        CHECK(names(validate(frame), "column 'a'", "this name"));
    }

    SUBCASE("columns of two lengths") {
        DataFrame frame;
        frame.names.push_back(dstd::SharedString("a"));
        frame.names.push_back(dstd::SharedString("b"));
        frame.columns.push_back(Series::flat_i64(values, 2));
        frame.columns.push_back(Series::flat_i64(values, 1));
        CHECK(names(validate(frame), "column 'b'", "the frame has 2"));
    }

    SUBCASE("the error names the column it found first") {
        const std::int32_t offsets[] = {0, 3, 2, 3};
        auto* col = new_column(TypeId::String, 3);
        set_offsets(col, offsets, sizeof(offsets));
        set_data(col, "abc", 3);
        DataFrame frame;
        frame.names.push_back(dstd::SharedString("note"));
        frame.columns.push_back(Series{col});
        CHECK(names(validate(frame), "column 'note'", "offset 2"));
        CHECK(message(validate(frame)).find("note") != dstd::String::npos);
    }
}
