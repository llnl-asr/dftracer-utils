// SPDX-License-Identifier: MIT
// The read views of the plugin SDK (dataframe/plugin_frame.h): a ColumnView
// reads a borrowed column with no copy, flat() is zero copy on a FLAT column
// and materializes any other encoding, and a FrameView finds a column by index
// or by name.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/plugin_frame.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

constexpr std::int32_t VIEW_INLINE = 12;

dstd::Vector<dstd::String> words() {
    dstd::Vector<dstd::String> out;
    out.push_back("alpha");
    out.push_back("beta");
    out.push_back("alpha");
    out.push_back("gamma");
    return out;
}

/// Four words with row 2 null, for the encodings that keep a row's nulls.
Series words_with_a_null() {
    dstd::Vector<dstd::StringView> views;
    for (const char* w : {"alpha", "beta", "gamma", "delta"})
        views.emplace_back(w);
    const std::uint8_t validity[1] = {0x0B};
    return Series::strings(
        dstd::Span<const dstd::StringView>(views.data(), views.size()),
        validity);
}

Series chunked_strings() {
    dstd::Vector<dstd::SharedPtr<dftu_series>> parts;
    for (const char* w : {"one", "two"}) {
        dstd::Vector<dstd::String> one;
        one.emplace_back(w);
        const Series part = Series::strings(one);
        parts.push_back(dstd::make_shared<dftu_series>(*part.handle()));
    }
    return Series{make_chunked(std::move(parts))};
}

/// A VIEW String column: row 0 inline, row 1 out of `blob`, row 2 null.
struct ViewStrings {
    dstd::String blob = "a value longer than twelve bytes";
    std::uint8_t views[3 * 16] = {};
    std::uint8_t validity[1] = {0x03};
    const void* buffers[1] = {blob.data()};
    std::int64_t sizes[1] = {static_cast<std::int64_t>(blob.size())};

    static void put(std::uint8_t* view, dstd::StringView s,
                    std::int32_t index) {
        const auto size = static_cast<std::int32_t>(s.size());
        std::memcpy(view, &size, sizeof(size));
        if (size <= VIEW_INLINE) {
            std::memcpy(view + 4, s.data(), s.size());
            return;
        }
        const std::int32_t offset = 0;
        std::memcpy(view + 4, s.data(), 4);
        std::memcpy(view + 8, &index, sizeof(index));
        std::memcpy(view + 12, &offset, sizeof(offset));
    }

    Series make() {
        put(views, "short", 0);
        put(views + 16, blob, 0);
        return Series{dftu_series_new_string_view(DFTU_TYPE_STRING, views, 3,
                                                  validity, buffers, sizes, 1,
                                                  nullptr, nullptr)};
    }
};

/// A frame over one owned copy of `s`, kept by `names` for the frame's life.
OwnedDataFrame frame_of(Column&& column, dstd::StringView name) {
    FrameBuilder builder;
    builder.add(name, std::move(column));
    dstd::Result<OwnedDataFrame> frame = builder.build();
    REQUIRE(frame.has_value());
    return std::move(*frame);
}

Column share(const Series& s) { return Column(dftu_series_share(s.handle())); }

}  // namespace

TEST_CASE("a ColumnView reads a flat column with no copy") {
    dstd::Vector<std::int64_t> values;
    for (std::int64_t i = 0; i < 3; ++i) values.push_back(i * 10);
    dstd::Vector<std::uint8_t> validity{0b101};
    OwnedDataFrame frame = frame_of(
        Column::from_int64(std::move(values), std::move(validity)), "x");
    const FrameView view = frame.view();
    REQUIRE(view.column_count() == 1);
    CHECK(view.row_count() == 3);
    CHECK(view.name(0) == dstd::StringView("x"));

    const ColumnView column = view.column(0);
    CHECK(column.type() == TypeId::Int64);
    CHECK(column.size() == 3);
    CHECK_FALSE(column.is_null(0));
    CHECK(column.is_null(1));
    CHECK_FALSE(column.is_null(2));

    const dstd::Span<const std::int64_t> got = column.values<std::int64_t>();
    REQUIRE(got.size() == 3);
    CHECK(got[0] == 0);
    CHECK(got[1] == 10);
    CHECK(got[2] == 20);
    CHECK(got.data() == static_cast<const std::int64_t*>(dftu_series_data(
                            dftu_dataframe_column_at(frame.get(), 0))));
}

TEST_CASE("flat() on a FLAT column shares the buffers") {
    dstd::Vector<std::int64_t> values;
    for (std::int64_t i = 0; i < 4; ++i) values.push_back(i);
    OwnedDataFrame frame = frame_of(Column::from_int64(std::move(values)), "x");
    const FrameView view = frame.view();

    dstd::Result<Column> flat = view.column(0).flat();
    REQUIRE(flat.has_value());
    CHECK(static_cast<const void*>(dftu_series_data(flat->get())) ==
          static_cast<const void*>(
              dftu_series_data(dftu_dataframe_column_at(frame.get(), 0))));
    CHECK(flat->dtype() == TypeId::Int64);
    CHECK(flat->size() == 4);
}

TEST_CASE("a string column reads a row at any encoding") {
    const Series dictionary = Series::strings(words()).dictionary_encode();
    REQUIRE(dictionary.encoding() == Encoding::Dictionary);
    OwnedDataFrame frame = frame_of(share(dictionary), "s");
    const ColumnView column = frame.view().column(0);

    CHECK(column.type() == TypeId::String);
    CHECK(column.size() == 4);
    CHECK(column.string_at(0) == dstd::StringView("alpha"));
    CHECK(column.string_at(1) == dstd::StringView("beta"));
    CHECK(column.string_at(3) == dstd::StringView("gamma"));

    // flat() materializes it, and the copy reads the same text row by row.
    dstd::Result<Column> flat = column.flat();
    REQUIRE(flat.has_value());
    CHECK(static_cast<const void*>(dftu_series_data(flat->get())) !=
          static_cast<const void*>(
              dftu_series_data(dftu_dataframe_column_at(frame.get(), 0))));
    CHECK(flat->size() == 4);
    OwnedDataFrame copies = frame_of(std::move(*flat), "s");
    const ColumnView plain = copies.view().column(0);
    CHECK(plain.string_at(0) == dstd::StringView("alpha"));
    CHECK(plain.string_at(1) == dstd::StringView("beta"));
    CHECK(plain.string_at(2) == dstd::StringView("alpha"));
    CHECK(plain.string_at(3) == dstd::StringView("gamma"));
}

TEST_CASE("flat() gathers a SELECTION column") {
    dstd::Vector<std::int64_t> values;
    for (std::int64_t i = 0; i < 6; ++i) values.push_back(i);
    dstd::Vector<std::uint8_t> validity{0b110011};
    const Series selected =
        Series::flat_i64(values.data(), 6, validity.data()).drop_nulls();
    REQUIRE(selected.encoding() == Encoding::Selection);

    OwnedDataFrame frame = frame_of(share(selected), "x");
    const ColumnView column = frame.view().column(0);
    CHECK(column.size() == 4);
    // A SELECTION column has no flat buffer of its own.
    CHECK(column.values<std::int64_t>().empty());

    dstd::Result<Column> flat = column.flat();
    REQUIRE(flat.has_value());
    const auto* got =
        static_cast<const std::int64_t*>(dftu_series_data(flat->get()));
    REQUIRE(flat->size() == 4);
    CHECK(got[0] == 0);
    CHECK(got[1] == 1);
    CHECK(got[2] == 4);
    CHECK(got[3] == 5);
}

TEST_CASE("values<T> is empty for another type and for no column") {
    dstd::Vector<std::int64_t> signed_values;
    for (std::int64_t i = 0; i < 3; ++i) signed_values.push_back(i);
    OwnedDataFrame ints =
        frame_of(Column::from_int64(std::move(signed_values)), "i");
    const ColumnView i64 = ints.view().column(0);
    REQUIRE(i64.values<std::int64_t>().size() == 3);
    // Signed read as unsigned, and as a float: an empty span, never a fatal.
    CHECK(i64.values<std::uint64_t>().empty());
    CHECK(i64.values<double>().empty());

    dstd::Vector<std::uint64_t> unsigned_values;
    unsigned_values.push_back(4);
    unsigned_values.push_back(5);
    OwnedDataFrame uints =
        frame_of(Column::from_uint64(std::move(unsigned_values)), "u");
    const ColumnView u64 = uints.view().column(0);
    REQUIRE(u64.values<std::uint64_t>().size() == 2);
    CHECK(u64.values<std::uint64_t>()[1] == 5);
    CHECK(u64.values<std::int64_t>().empty());

    // A Timestamp column stores its rows as int64, so int64 reads them.
    dstd::Vector<std::int64_t> stamps;
    stamps.push_back(10);
    stamps.push_back(20);
    OwnedDataFrame times =
        frame_of(Column::from_timestamp(std::move(stamps)), "t");
    const ColumnView ts = times.view().column(0);
    REQUIRE(ts.type() == TypeId::Timestamp);
    REQUIRE(ts.values<std::int64_t>().size() == 2);
    CHECK(ts.values<std::int64_t>()[1] == 20);

    // A view of no column has no type, no rows, no values and no bytes.
    const FrameView none;
    const ColumnView absent = none.column(0);
    CHECK(absent.type() == TypeId::Unknown);
    CHECK(absent.size() == 0);
    CHECK(absent.values<std::int64_t>().empty());
    CHECK(absent.string_at(0).empty());
    CHECK_FALSE(absent.is_null(0));
}

TEST_CASE("string_at and is_null read every encoding and a null row") {
    // A VIEW column: row 0 inline, row 1 out of a data buffer, row 2 null.
    ViewStrings view_strings;
    const Series view = view_strings.make();
    REQUIRE(view.encoding() == Encoding::View);
    OwnedDataFrame viewed = frame_of(share(view), "s");
    const ColumnView view_column = viewed.view().column(0);
    CHECK(view_column.size() == 3);
    CHECK(view_column.string_at(0) == dstd::StringView("short"));
    CHECK(view_column.string_at(1) == dstd::StringView(view_strings.blob));
    CHECK(view_column.is_null(2));
    CHECK_FALSE(view_column.is_null(0));
    CHECK(view_column.string_at(2).empty());

    // A DICTIONARY column keeps its rows' nulls and reads through its values.
    const Series dictionary = words_with_a_null().dictionary_encode();
    REQUIRE(dictionary.encoding() == Encoding::Dictionary);
    OwnedDataFrame dicted = frame_of(share(dictionary), "s");
    const ColumnView dict_column = dicted.view().column(0);
    CHECK(dict_column.is_null(2));
    CHECK_FALSE(dict_column.is_null(0));
    CHECK_FALSE(dict_column.is_null(3));
    CHECK(dict_column.string_at(0) == dstd::StringView("alpha"));
    CHECK(dict_column.string_at(3) == dstd::StringView("delta"));
    CHECK(dict_column.string_at(2).empty());

    // A SELECTION column resolves each row, and its nulls, through its base.
    const std::uint8_t all[1] = {0x0F};
    const Series mask = Series::flat(TypeId::Bool, all, 4);
    const Series selected = words_with_a_null().filter(mask);
    REQUIRE(selected.encoding() == Encoding::Selection);
    OwnedDataFrame selected_frame = frame_of(share(selected), "s");
    const ColumnView selection = selected_frame.view().column(0);
    CHECK(selection.size() == 4);
    CHECK(selection.is_null(2));
    CHECK_FALSE(selection.is_null(0));
    CHECK(selection.string_at(0) == dstd::StringView("alpha"));
    CHECK(selection.string_at(3) == dstd::StringView("delta"));
    CHECK(selection.string_at(2).empty());

    // A CHUNKED column reads the row of the chunk it falls in.
    const Series chunked = chunked_strings();
    REQUIRE(chunked.encoding() == Encoding::Chunked);
    OwnedDataFrame chunked_frame = frame_of(share(chunked), "s");
    const ColumnView chunk_column = chunked_frame.view().column(0);
    CHECK(chunk_column.size() == 2);
    CHECK(chunk_column.string_at(0) == dstd::StringView("one"));
    CHECK(chunk_column.string_at(1) == dstd::StringView("two"));
    CHECK_FALSE(chunk_column.is_null(0));
    CHECK_FALSE(chunk_column.is_null(1));
}

TEST_CASE("a FrameView finds a column by index and by name") {
    dstd::Vector<std::int64_t> values;
    values.push_back(7);
    dstd::Vector<double> doubles;
    doubles.push_back(1.5);
    FrameBuilder builder;
    builder.add("a", Column::from_int64(std::move(values)));
    builder.add("n", Column::from_double(std::move(doubles)));
    dstd::Result<OwnedDataFrame> built = builder.build();
    REQUIRE(built.has_value());
    const FrameView view = built->view();

    CHECK(view.column_count() == 2);
    CHECK(view.row_count() == 1);
    CHECK(view.name(0) == dstd::StringView("a"));
    CHECK(view.name(1) == dstd::StringView("n"));
    CHECK(view.column(0).type() == TypeId::Int64);
    CHECK(view.column(1).type() == TypeId::Float64);

    // An index out of range, and a null frame, give a view of no column.
    CHECK(dftu_dataframe_column_at(nullptr, 0) == nullptr);
    CHECK(dftu_dataframe_column_at(built->get(), 9) == nullptr);
    CHECK(dftu_dataframe_column_at(built->get(), -1) == nullptr);
    CHECK(view.column(9).type() == TypeId::Unknown);
    CHECK(view.column(9).size() == 0);
    CHECK(view.column(-1).type() == TypeId::Unknown);
    CHECK(view.name(9).empty());
    const FrameView none;
    CHECK(none.column_count() == 0);
    CHECK(none.row_count() == 0);
    CHECK(none.column(0).type() == TypeId::Unknown);
    CHECK(none.name(0).empty());

    const dstd::Optional<ColumnView> by_name = view.column("n");
    REQUIRE(by_name.has_value());
    CHECK(by_name->type() == TypeId::Float64);
    CHECK(by_name->values<double>()[0] == 1.5);
    CHECK_FALSE(view.column("absent").has_value());
}

TEST_CASE("a FrameBuilder refuses an empty frame and a row-count mismatch") {
    FrameBuilder empty;
    dstd::Result<OwnedDataFrame> none = empty.build();
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().condition() == dstd::Condition::InvalidArgument);

    dstd::Vector<std::int64_t> two;
    two.push_back(1);
    two.push_back(2);
    dstd::Vector<std::int64_t> one;
    one.push_back(1);
    FrameBuilder mismatched;
    mismatched.add("a", Column::from_int64(std::move(two)));
    mismatched.add("b", Column::from_int64(std::move(one)));
    dstd::Result<OwnedDataFrame> refused = mismatched.build();
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().condition() == dstd::Condition::InvalidArgument);
}
