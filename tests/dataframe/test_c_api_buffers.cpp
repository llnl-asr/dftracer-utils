// SPDX-License-Identifier: MIT
// The C ABI a plugin uses to read a column's Arrow-layout buffers (validity,
// chunks, dictionary codes and values, StringView views and data buffers) and
// to hand Arrow memory back with the borrowed constructors, zero copy.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/shared_string.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

constexpr std::int32_t INLINE_MAX = 12;

int releases = 0;

void count_release(void* /*ctx*/) { ++releases; }

// Rows 0 and 2 present, rows 1 and 3 null.
const std::int64_t VALUES[4] = {10, 20, 30, 40};
const std::uint8_t BITS[1] = {0x05};

Series flat_i64() { return Series::flat_i64(VALUES, 4, BITS); }

dstd::Vector<dstd::String> words() {
    return dstd::Vector<dstd::String>{"alpha", "beta", "alpha", "gamma"};
}

// A VIEW column over two buffers of the caller's storage: row 0 inline, row 1
// out of `blob_a`, row 2 out of `blob_b`, row 3 null.
struct ViewFixture {
    dstd::String blob_a = "a value longer than twelve bytes";
    dstd::String blob_b = "another one that is long too";
    std::uint8_t views[4 * 16] = {};
    std::uint8_t validity[1] = {0x07};
    const void* buffers[2] = {blob_a.data(), blob_b.data()};
    std::int64_t sizes[2] = {static_cast<std::int64_t>(blob_a.size()),
                             static_cast<std::int64_t>(blob_b.size())};

    static void put(std::uint8_t* view, const dstd::String& s,
                    std::int32_t index) {
        const auto size = static_cast<std::int32_t>(s.size());
        std::memcpy(view, &size, sizeof(size));
        if (size <= INLINE_MAX) {
            std::memcpy(view + 4, s.data(), s.size());
            return;
        }
        std::memcpy(view + 4, s.data(), 4);
        std::memcpy(view + 8, &index, sizeof(index));
        const std::int32_t offset = 0;
        std::memcpy(view + 12, &offset, sizeof(offset));
    }

    ViewFixture() {
        put(views, "short", 0);
        put(views + 16, blob_a, 0);
        put(views + 32, blob_b, 1);
    }

    dftu_series* make(TypeId type = TypeId::String) {
        return dftu_series_new_string_view(static_cast<dftu_dtype>(type), views,
                                           4, validity, buffers, sizes, 2,
                                           nullptr, nullptr);
    }
};

Series view_column(ViewFixture& fixture) { return Series{fixture.make()}; }

Series chunked_i64() {
    dstd::Vector<dstd::SharedPtr<dftu_series>> chunks;
    for (int k = 0; k < 3; ++k)
        chunks.push_back(dstd::make_shared<dftu_series>(*flat_i64().handle()));
    return Series{make_chunked(std::move(chunks))};
}

}  // namespace

TEST_SUITE("c_api_buffers") {
    TEST_CASE("validity of a flat column, NULL when there are no nulls") {
        Series col = flat_i64();
        std::int64_t bit_offset = -1;
        const std::uint8_t* bits =
            dftu_series_validity(col.handle(), &bit_offset);
        REQUIRE(bits != nullptr);
        CHECK(bit_offset == 0);
        CHECK(bits[0] == BITS[0]);

        Series all_valid = Series::flat_i64(VALUES, 4);
        bit_offset = 7;
        CHECK(dftu_series_validity(all_valid.handle(), &bit_offset) == nullptr);
        CHECK(bit_offset == 0);

        CHECK(dftu_series_validity(nullptr, &bit_offset) == nullptr);
        // A null out pointer is allowed.
        CHECK(dftu_series_validity(col.handle(), nullptr) == bits);
    }

    TEST_CASE("validity is borrowed from a VIEW column and refused elsewhere") {
        ViewFixture fixture;
        Series view = view_column(fixture);
        REQUIRE(view.valid());
        std::int64_t bit_offset = -1;
        const std::uint8_t* bits =
            dftu_series_validity(view.handle(), &bit_offset);
        REQUIRE(bits != nullptr);
        CHECK(bit_offset == 0);
        // Borrowed: the getter hands back the caller's own bitmap.
        CHECK(bits == fixture.validity);

        Series dict = Series::strings(words()).dictionary_encode();
        REQUIRE(dict.encoding() == Encoding::Dictionary);
        CHECK(dftu_series_validity(dict.handle(), &bit_offset) == nullptr);

        Series selected = flat_i64().drop_nulls();
        CHECK(selected.encoding() == Encoding::Selection);
        CHECK(dftu_series_validity(selected.handle(), &bit_offset) == nullptr);

        Series chunks = chunked_i64();
        REQUIRE(chunks.encoding() == Encoding::Chunked);
        CHECK(dftu_series_validity(chunks.handle(), &bit_offset) == nullptr);

        // No op builds a CONSTANT column, so make one by hand.
        dftu_series constant;
        constant.type = TypeId::Int64;
        constant.encoding = Encoding::Constant;
        constant.length = 4;
        CHECK(dftu_series_validity(&constant, &bit_offset) == nullptr);
    }

    TEST_CASE("a slice reports its own bitmap from bit 0, aligned or not") {
        // Rows 0, 2, 4, 6 and 8 to 11 valid, the rest null.
        std::int64_t values[16] = {};
        for (int i = 0; i < 16; ++i) values[i] = i;
        const std::uint8_t bits[2] = {0x55, 0x0F};
        Series parent = Series::flat_i64(values, 16, bits);

        std::int64_t parent_offset = -1;
        const std::uint8_t* parent_bits =
            dftu_series_validity(parent.handle(), &parent_offset);
        REQUIRE(parent_bits != nullptr);

        // Starting on a byte boundary: the bitmap is shared, so row 0 of the
        // slice is bit 0 of the caller's second byte.
        Series aligned = parent.slice(8, 4);
        REQUIRE(aligned.valid());
        std::int64_t bit_offset = -1;
        const std::uint8_t* aligned_bits =
            dftu_series_validity(aligned.handle(), &bit_offset);
        REQUIRE(aligned_bits != nullptr);
        CHECK(bit_offset == 0);
        CHECK(aligned_bits == parent_bits + 1);
        for (std::int64_t i = 0; i < 4; ++i)
            CHECK(((aligned_bits[i >> 3] >> (i & 7)) & 1) ==
                  ((parent_bits[(8 + i) >> 3] >> ((8 + i) & 7)) & 1));
        // Those rows are all valid, yet the column keeps a bitmap: the getter
        // reports the buffer it has, not the null count.
        CHECK(dftu_series_null_count(aligned.handle()) == 0);

        // Starting inside a byte: the bitmap is repacked, still from bit 0.
        Series unaligned = parent.slice(3, 4);
        REQUIRE(unaligned.valid());
        bit_offset = -1;
        const std::uint8_t* unaligned_bits =
            dftu_series_validity(unaligned.handle(), &bit_offset);
        REQUIRE(unaligned_bits != nullptr);
        CHECK(bit_offset == 0);
        for (std::int64_t i = 0; i < 4; ++i)
            CHECK(((unaligned_bits[i >> 3] >> (i & 7)) & 1) ==
                  ((parent_bits[(3 + i) >> 3] >> ((3 + i) & 7)) & 1));
        // Rows 4 and 6 are valid there and rows 3 and 5 are null.
        CHECK(unaligned_bits[0] == 0x0A);

        // A slice of a column that has no bitmap has none either.
        Series plain = Series::flat_i64(values, 16);
        CHECK(dftu_series_validity(plain.slice(8, 4).handle(), &bit_offset) ==
              nullptr);
    }

    TEST_CASE("a chunked column reports its chunks, another encoding one") {
        Series chunks = chunked_i64();
        REQUIRE(chunks.encoding() == Encoding::Chunked);
        CHECK(dftu_series_chunk_count(chunks.handle()) == 3);
        CHECK(dftu_series_chunk_count(nullptr) == 0);

        std::int64_t total = 0;
        for (std::int32_t k = 0; k < 3; ++k) {
            dftu_series* part = dftu_series_chunk(chunks.handle(), k);
            REQUIRE(part != nullptr);
            CHECK(dftu_series_length(part) == 4);
            CHECK(dftu_series_type(part) == DFTU_TYPE_INT64);
            CHECK(dftu_series_is_null(part, 1) == 1);
            total += dftu_series_length(part);
            dftu_series_free(part);
        }
        CHECK(total == dftu_series_length(chunks.handle()));
        CHECK(dftu_series_chunk(chunks.handle(), 3) == nullptr);
        CHECK(dftu_series_chunk(chunks.handle(), -1) == nullptr);
        CHECK(dftu_series_chunk(nullptr, 0) == nullptr);

        // One chunk for any other encoding: the column itself, no copy.
        Series flat = flat_i64();
        CHECK(dftu_series_chunk_count(flat.handle()) == 1);
        dftu_series* same = dftu_series_chunk(flat.handle(), 0);
        REQUIRE(same != nullptr);
        CHECK(dftu_series_data(same) == dftu_series_data(flat.handle()));
        CHECK(dftu_series_length(same) == 4);
        CHECK(dftu_series_is_null(same, 3) == 1);
        dftu_series_free(same);
        CHECK(dftu_series_chunk(flat.handle(), 1) == nullptr);
    }

    TEST_CASE("a dictionary column exposes its codes and values") {
        Series flat = Series::strings(words());
        Series dict = flat.dictionary_encode();
        REQUIRE(dict.encoding() == Encoding::Dictionary);

        dftu_series* codes = dftu_series_dictionary_codes(dict.handle());
        REQUIRE(codes != nullptr);
        CHECK(dftu_series_type(codes) == DFTU_TYPE_INT32);
        CHECK(dftu_series_encoding(codes) == 0);
        CHECK(dftu_series_length(codes) == 4);
        const auto* raw =
            static_cast<const std::int32_t*>(dftu_series_data(codes));
        REQUIRE(raw != nullptr);

        dftu_series* values = dftu_series_dictionary_values(dict.handle());
        REQUIRE(values != nullptr);
        CHECK(dftu_series_type(values) == DFTU_TYPE_STRING);
        for (std::int64_t i = 0; i < 4; ++i) {
            std::int64_t len = 0;
            const char* text = dftu_series_string_at(values, raw[i], &len);
            REQUIRE(text != nullptr);
            CHECK(dstd::StringView(text, static_cast<std::size_t>(len)) ==
                  flat.string_at(i));
        }

        Series other = flat_i64();
        CHECK(dftu_series_dictionary_codes(other.handle()) == nullptr);
        CHECK(dftu_series_dictionary_values(other.handle()) == nullptr);
        CHECK(dftu_series_dictionary_codes(nullptr) == nullptr);
        CHECK(dftu_series_dictionary_values(nullptr) == nullptr);

        dftu_series_free(codes);
        dftu_series_free(values);
    }

    TEST_CASE("a VIEW column hands out its views and data buffers") {
        ViewFixture fixture;
        Series view = view_column(fixture);
        CHECK(dftu_series_view_buffer_count(view.handle()) == 2);
        // Borrowed: the caller's own views and buffers come straight back.
        CHECK(dftu_series_view_data(view.handle()) == fixture.views);
        for (std::int32_t i = 0; i < 2; ++i) {
            std::int64_t size = -1;
            const void* buffer =
                dftu_series_view_buffer(view.handle(), i, &size);
            CHECK(buffer == fixture.buffers[i]);
            CHECK(size == fixture.sizes[i]);
        }
        // The size out-parameter is optional.
        CHECK(dftu_series_view_buffer(view.handle(), 0, nullptr) ==
              fixture.buffers[0]);

        // A bad index is an error, not an empty buffer.
        std::int64_t size = -1;
        CHECK(dftu_series_view_buffer(view.handle(), 2, &size) == nullptr);
        CHECK(size == 0);
        CHECK(dftu_series_view_buffer(view.handle(), -1, &size) == nullptr);
        CHECK(size == 0);

        // Refused for any other encoding and for a NULL column.
        Series flat = flat_i64();
        CHECK(dftu_series_view_buffer_count(flat.handle()) == -1);
        CHECK(dftu_series_view_data(flat.handle()) == nullptr);
        CHECK(dftu_series_view_buffer(flat.handle(), 0, &size) == nullptr);
        CHECK(size == 0);
        Series dict = Series::strings(words()).dictionary_encode();
        CHECK(dftu_series_view_buffer_count(dict.handle()) == -1);
        CHECK(dftu_series_view_data(dict.handle()) == nullptr);
        CHECK(dftu_series_view_buffer_count(nullptr) == -1);
        CHECK(dftu_series_view_data(nullptr) == nullptr);
        CHECK(dftu_series_view_buffer(nullptr, 0, &size) == nullptr);
        CHECK(size == 0);
    }

    TEST_CASE("the borrowed flat constructor borrows its validity") {
        releases = 0;
        {
            dstd::Vector<std::int64_t> vals{1, 2, 3, 4};
            std::uint8_t bits[1] = {0x0d};
            Series borrowed{dftu_series_new_flat_borrowed(
                DFTU_TYPE_INT64, vals.data(), 4, bits, count_release, nullptr)};
            REQUIRE(borrowed.valid());
            std::int64_t bit_offset = -1;
            // No copy: the column's bitmap is the caller's own bytes.
            CHECK(dftu_series_validity(borrowed.handle(), &bit_offset) == bits);
            CHECK(bit_offset == 0);
            CHECK(borrowed.is_null(1));
            CHECK_FALSE(borrowed.is_null(2));
            // A share and a slice hold the same memory: no release yet.
            Series shared = borrowed.share();
            Series head = borrowed.head(2);
            CHECK(releases == 0);
            CHECK(shared.length() == 4);
            CHECK(head.length() == 2);
            CHECK(head.is_null(1));
        }
        CHECK(releases == 1);

        // A refused borrow releases at once.
        releases = 0;
        char bytes[4] = {0};
        CHECK(dftu_series_new_flat_borrowed(DFTU_TYPE_STRING, bytes, 1, nullptr,
                                            count_release, nullptr) == nullptr);
        CHECK(releases == 1);
    }

    TEST_CASE("the borrowed string constructor borrows every buffer") {
        Series plain = Series::strings(words());
        const auto* offsets = dftu_series_offsets(plain.handle());
        const void* data = dftu_series_data(plain.handle());
        REQUIRE(offsets != nullptr);
        REQUIRE(data != nullptr);
        const std::int64_t data_size = offsets[4];

        releases = 0;
        {
            Series borrowed{dftu_series_new_string_borrowed(
                DFTU_TYPE_STRING, offsets, data, data_size, 4, nullptr,
                count_release, nullptr)};
            REQUIRE(borrowed.valid());
            CHECK(dftu_series_data(borrowed.handle()) == data);
            CHECK(dftu_series_offsets(borrowed.handle()) == offsets);
            for (std::int64_t i = 0; i < 4; ++i)
                CHECK(borrowed.string_at(i) == plain.string_at(i));
            Series tail = borrowed.tail(2);
            CHECK(tail.string_at(0) == plain.string_at(2));
            CHECK(releases == 0);
        }
        CHECK(releases == 1);

        // int64 offsets for a LargeString column.
        std::int64_t wide_offsets[3] = {0, 5, 13};
        const char wide_data[] = "large offsets";
        {
            Series wide{dftu_series_new_string_borrowed(
                DFTU_TYPE_LARGE_STRING, wide_offsets, wide_data, 13, 2, nullptr,
                count_release, nullptr)};
            REQUIRE(wide.valid());
            CHECK(dftu_series_offsets64(wide.handle()) == wide_offsets);
            CHECK(dftu_series_offsets(wide.handle()) == nullptr);
            CHECK(wide.string_at(0) == dstd::StringView("large"));
            CHECK(wide.string_at(1) == dstd::StringView(" offsets"));
        }
        CHECK(releases == 2);

        // Bad input returns NULL and runs the release at once.
        struct BadCase {
            dftu_dtype type;
            std::int64_t n;
            std::int64_t data_size;
        };
        const BadCase bad[] = {
            {DFTU_TYPE_INT64, 4, data_size},    // not a variable-width type
            {DFTU_TYPE_STRING, -1, data_size},  // negative rows
            {DFTU_TYPE_STRING, 4, 2},           // offsets past the data
        };
        for (const BadCase& c : bad) {
            const int before = releases;
            CHECK(dftu_series_new_string_borrowed(
                      c.type, offsets, data, c.data_size, c.n, nullptr,
                      count_release, nullptr) == nullptr);
            CHECK(releases == before + 1);
        }
        // Decreasing offsets are refused too.
        const std::int32_t decreasing[5] = {0, 5, 3, 8, 12};
        const int before = releases;
        CHECK(dftu_series_new_string_borrowed(
                  DFTU_TYPE_STRING, decreasing, data, 12, 4, nullptr,
                  count_release, nullptr) == nullptr);
        CHECK(releases == before + 1);
        // A NULL buffer with rows is refused.
        CHECK(dftu_series_new_string_borrowed(
                  DFTU_TYPE_STRING, nullptr, data, data_size, 4, nullptr,
                  count_release, nullptr) == nullptr);
        CHECK(releases == before + 2);
        // A NULL values buffer with bytes promised is refused too, and the
        // release runs at once.
        CHECK(dftu_series_new_string_borrowed(
                  DFTU_TYPE_STRING, offsets, nullptr, data_size, 4, nullptr,
                  count_release, nullptr) == nullptr);
        CHECK(releases == before + 3);
    }

    TEST_CASE("a borrowed string column may have no data buffer") {
        // Arrow leaves the values buffer NULL when every row is empty, so the
        // constructor accepts it when data_size is 0.
        const std::int32_t offsets[5] = {0, 0, 0, 0, 0};
        releases = 0;
        {
            Series empty{dftu_series_new_string_borrowed(
                DFTU_TYPE_STRING, offsets, nullptr, 0, 4, nullptr,
                count_release, nullptr)};
            REQUIRE(empty.valid());
            CHECK(dftu_series_length(empty.handle()) == 4);
            CHECK(dftu_series_offsets(empty.handle()) == offsets);
            CHECK(dftu_series_data(empty.handle()) == nullptr);
            CHECK(dftu_series_null_count(empty.handle()) == 0);
            CHECK(releases == 0);
        }
        CHECK(releases == 1);
    }

    TEST_CASE("a flat column round-trips through its own buffers") {
        Series src = flat_i64();
        const void* data = dftu_series_data(src.handle());
        REQUIRE(data != nullptr);
        std::int64_t bit_offset = -1;
        const std::uint8_t* bits =
            dftu_series_validity(src.handle(), &bit_offset);
        REQUIRE(bits != nullptr);
        const std::int64_t n = dftu_series_length(src.handle());

        releases = 0;
        {
            Series back{dftu_series_new_flat_borrowed(
                static_cast<dftu_dtype>(dftu_series_type(src.handle())), data,
                n, bits, count_release, nullptr)};
            REQUIRE(back.valid());
            // Zero copy of both the values and the nulls.
            CHECK(dftu_series_data(back.handle()) == data);
            CHECK(dftu_series_validity(back.handle(), &bit_offset) == bits);
            CHECK(back.null_count() == src.null_count());
            for (std::int64_t i = 0; i < n; ++i) {
                CHECK(back.is_null(i) == src.is_null(i));
                if (src.is_null(i)) continue;
                CHECK(back.data<std::int64_t>()[i] == VALUES[i]);
            }
            CHECK(releases == 0);
        }
        CHECK(releases == 1);
    }

    TEST_CASE("a flat string column round-trips through its own buffers") {
        Series src = Series::strings(words());
        const auto* offsets = dftu_series_offsets(src.handle());
        const void* data = dftu_series_data(src.handle());
        REQUIRE(offsets != nullptr);
        REQUIRE(data != nullptr);
        const std::int64_t n = dftu_series_length(src.handle());

        releases = 0;
        {
            Series back{dftu_series_new_string_borrowed(
                static_cast<dftu_dtype>(dftu_series_type(src.handle())),
                offsets, data, offsets[n], n, nullptr, count_release, nullptr)};
            REQUIRE(back.valid());
            CHECK(dftu_series_data(back.handle()) == data);
            CHECK(dftu_series_offsets(back.handle()) == offsets);
            for (std::int64_t i = 0; i < n; ++i)
                CHECK(back.string_at(i) == src.string_at(i));
        }
        CHECK(releases == 1);
    }

    TEST_CASE("a VIEW column round-trips through its own buffers") {
        ViewFixture fixture;
        Series src = view_column(fixture);
        const std::int32_t n_buffers =
            dftu_series_view_buffer_count(src.handle());
        REQUIRE(n_buffers == 2);
        const void* views = dftu_series_view_data(src.handle());
        REQUIRE(views != nullptr);
        const void* buffers[2] = {nullptr, nullptr};
        std::int64_t sizes[2] = {0, 0};
        for (std::int32_t i = 0; i < n_buffers; ++i)
            buffers[i] = dftu_series_view_buffer(src.handle(), i, &sizes[i]);
        std::int64_t bit_offset = -1;
        const std::uint8_t* bits =
            dftu_series_validity(src.handle(), &bit_offset);
        REQUIRE(bits != nullptr);

        releases = 0;
        {
            Series back{dftu_series_new_string_view(
                static_cast<dftu_dtype>(dftu_series_type(src.handle())), views,
                4, bits, buffers, sizes, n_buffers, count_release, nullptr)};
            REQUIRE(back.valid());
            CHECK(dftu_series_encoding(back.handle()) ==
                  static_cast<std::int32_t>(Encoding::View));
            for (std::int64_t i = 0; i < 4; ++i) {
                CHECK(back.is_null(i) == src.is_null(i));
                CHECK(back.string_at(i) == src.string_at(i));
            }
            // The rebuilt column points at the very same views and buffers.
            std::int64_t size_again = -1;
            CHECK(dftu_series_view_data(back.handle()) == views);
            CHECK(dftu_series_view_buffer(back.handle(), 0, &size_again) ==
                  buffers[0]);
            CHECK(size_again == sizes[0]);
            CHECK(dftu_series_validity(back.handle(), &bit_offset) == bits);
            CHECK(releases == 0);
        }
        CHECK(releases == 1);
    }
}
