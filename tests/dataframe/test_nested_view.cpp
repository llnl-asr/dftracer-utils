// SPDX-License-Identifier: MIT
// The FLAT pieces of a nested column: the column itself, its chunks or one
// materialized copy, and the piece of one row reached with no copy.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/nested_view.h>
#include <dftracer/utils/dataframe/internal/reshape.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <cstdint>
#include <cstring>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

Series ints(dstd::Vector<std::int64_t> v) {
    return Series::flat_i64(v.data(), static_cast<std::int64_t>(v.size()));
}

// [[1, 2], [3]] and [[4], []] as one CHUNKED list column.
Series two_chunk_list() {
    const std::int32_t a[] = {0, 2, 3};
    const std::int32_t b[] = {0, 1, 1};
    dstd::Vector<Series> parts;
    parts.push_back(Series::list(a, ints({1, 2, 3})));
    parts.push_back(Series::list(b, ints({4})));
    return dftu_utils_test::ok(chunked_column(std::move(parts)));
}

std::int64_t value_at(const NestedPiece& p, std::int64_t row, std::int64_t k) {
    const ValueRange r = p.values_of(row);
    return Series{new dftu_series(p.child(0))}.data<std::int64_t>()[r.lo + k];
}

}  // namespace

TEST_SUITE("nested view") {
    TEST_CASE("a FLAT column is one borrowed piece") {
        const std::int32_t off[] = {0, 2, 3};
        const Series flat = Series::list(off, ints({1, 2, 3}));
        auto pieces = dftu_utils_test::ok(nested_pieces(flat));
        REQUIRE(pieces.size() == 1);
        CHECK(&pieces[0].column() == flat.handle());
        CHECK(pieces[0].values_of(0).hi == 2);
    }

    TEST_CASE("a CHUNKED column gives its chunks with their first rows") {
        const Series col = two_chunk_list();
        REQUIRE(col.encoding() == Encoding::Chunked);
        auto pieces = dftu_utils_test::ok(nested_pieces(col));
        REQUIRE(pieces.size() == 2);
        CHECK(pieces[1].row_base() == 2);
        std::int64_t local = 0;
        const NestedPiece& p = piece_at(pieces, 2, local);
        CHECK(local == 0);
        CHECK(value_at(p, local, 0) == 4);
        CHECK(&p.column() == col.handle()->nested[1].series.get());
    }

    TEST_CASE("one row is found through chunks, a selection and a dictionary") {
        const Series col = two_chunk_list();
        std::int64_t local = 0;
        auto in_chunk = dftu_utils_test::ok(nested_row(col, 3, local));
        CHECK(local == 1);
        CHECK(in_chunk.values_of(local).lo == in_chunk.values_of(local).hi);

        const std::int32_t off[] = {0, 2, 3};
        const Series base = Series::list(off, ints({1, 2, 3}));
        const std::int32_t codes[] = {1, 0, 1};
        const Series dict = dftu_utils_test::ok(
            dictionary_of(Series::flat(TypeId::Int32, codes, 3), base));
        auto via_code = dftu_utils_test::ok(nested_row(dict, 0, local));
        CHECK(local == 1);
        CHECK(&via_code.column() == dict.handle()->child().get());
        CHECK(value_at(via_code, local, 0) == 3);
    }

    TEST_CASE("a selection row is found in its base") {
        const std::int32_t off[] = {0, 2, 3};
        const Series base = Series::list(off, ints({1, 2, 3}));
        const std::uint8_t keep = 0b10;  // a Bool column holds bits
        const Series picked = base.filter(Series::flat(TypeId::Bool, &keep, 2));
        REQUIRE(picked.encoding() == Encoding::Selection);
        REQUIRE(picked.length() == 1);
        std::int64_t local = 0;
        auto row = dftu_utils_test::ok(nested_row(picked, 0, local));
        CHECK(local == 1);
        CHECK(&row.column() == picked.handle()->child().get());
        CHECK(value_at(row, local, 0) == 3);
    }

    TEST_CASE("a null dictionary row with an unused code has no base row") {
        const std::int32_t off[] = {0, 1};
        const Series base = Series::list(off, ints({22}));
        // Built by hand: dictionary_of would clean the code under the null.
        auto* raw = new dftu_series();
        adopt_type_from(*raw, *base.handle());
        raw->encoding = Encoding::Dictionary;
        raw->length = 2;
        raw->null_count = 1;
        raw->data = new_buffer(2 * sizeof(std::int32_t));
        const std::int32_t codes[] = {999, 0};
        std::memcpy(writable(raw->data), codes, sizeof codes);
        raw->validity = new_buffer(1);
        writable(raw->validity)[0] = 0b10;
        raw->set_child(adopt_series(new dftu_series(*base.handle())));
        const Series dict{raw};
        std::int64_t local = 0;
        CHECK_FALSE(nested_row(dict, 0, local));
        auto row = dftu_utils_test::ok(nested_row(dict, 1, local));
        CHECK(value_at(row, local, 0) == 22);
        const Series flat = dict.materialize();
        REQUIRE(flat.valid());
        CHECK(flat.is_null(0));
        CHECK_FALSE(flat.is_null(1));
    }

    TEST_CASE(
        "a negative dictionary code under a valid row fails to "
        "materialize") {
        const std::int32_t off[] = {0, 1};
        const Series base = Series::list(off, ints({22}));
        auto* raw = new dftu_series();
        adopt_type_from(*raw, *base.handle());
        raw->encoding = Encoding::Dictionary;
        raw->length = 2;
        raw->data = new_buffer(2 * sizeof(std::int32_t));
        const std::int32_t codes[] = {-1, 0};
        std::memcpy(writable(raw->data), codes, sizeof codes);
        raw->set_child(adopt_series(new dftu_series(*base.handle())));
        const Series dict{raw};
        CHECK_FALSE(dict.materialize().valid());
    }

    TEST_CASE("a column that is not nested has no pieces") {
        CHECK_FALSE(nested_pieces(ints({1, 2})));
        std::int64_t local = 0;
        CHECK_FALSE(nested_row(ints({1}), 0, local));
    }
}
