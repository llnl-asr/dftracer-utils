// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <arrow/arrow_helpers.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/kernels/comparison.h>
#include <dftracer/utils/dataframe/kernels/dictionary.h>
#include <dftracer/utils/dataframe/kernels/filter.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <nanoarrow/nanoarrow.h>
#include <span_list.h>

#include <cstring>

// The bridge exports without copying: every buffer of the exported array
// aliases the source column, a selection exports as a dictionary array and a
// column keeps its layout through a round trip.

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

TEST_SUITE("vec_arrow") {
    TEST_CASE("flat int64 round-trips through Arrow zero-copy") {
        dstd::Vector<std::int64_t> a{10, 20, 30};
        Series col = Series::flat_i64(a.data(), 3);

        ArrowSchema schema;
        ArrowArray array;
        test_types::export_series(col, &schema, &array);
        CHECK(std::strcmp(schema.format, "l") == 0);
        REQUIRE(array.length == 3);
        REQUIRE(array.n_buffers == 2);
        // Exported buffer aliases the source (zero copy).
        const std::int64_t* raw =
            static_cast<const std::int64_t*>(array.buffers[1]);
        CHECK(raw[0] == 10);
        CHECK(raw[2] == 30);

        Series back = test_types::import_series(&schema, &array);
        REQUIRE(back.valid());
        CHECK(back.type() == TypeId::Int64);
        REQUIRE(back.length() == 3);
        const std::int64_t* p = back.data<std::int64_t>();
        CHECK(p[0] == 10);
        CHECK(p[1] == 20);
        CHECK(p[2] == 30);

        schema.release(&schema);
    }

    TEST_CASE("selection exports as a dictionary array (no flatten)") {
        dstd::Vector<std::int64_t> v{5, 1, 9, 3, 7};
        Series col = Series::flat_i64(v.data(), 5);
        Series sel = filter_gt(col, 4);  // indices 0,2,4 -> values 5,9,7

        ArrowSchema schema;
        ArrowArray array;
        test_types::export_series(sel, &schema, &array);

        CHECK(std::strcmp(schema.format, "l") == 0);  // int64 indices
        REQUIRE(schema.dictionary != nullptr);
        CHECK(std::strcmp(schema.dictionary->format, "l") == 0);  // int64 base
        REQUIRE(array.dictionary != nullptr);
        CHECK(array.length == 3);
        CHECK(array.dictionary->length == 5);

        const std::int64_t* idx =
            static_cast<const std::int64_t*>(array.buffers[1]);
        const std::int64_t* dict =
            static_cast<const std::int64_t*>(array.dictionary->buffers[1]);
        CHECK(dict[idx[0]] == 5);
        CHECK(dict[idx[1]] == 9);
        CHECK(dict[idx[2]] == 7);

        array.release(&array);
        schema.release(&schema);
    }

    TEST_CASE("string round-trips through Arrow zero-copy") {
        Series s = Series::strings(
            dftu_utils_test::span_of<dstd::StringView>({"foo", "bar", "bazz"}));

        ArrowSchema schema;
        ArrowArray array;
        test_types::export_series(s, &schema, &array);
        CHECK(std::strcmp(schema.format, "u") == 0);  // utf8
        REQUIRE(array.n_buffers == 3);
        REQUIRE(array.length == 3);

        Series back = test_types::import_series(&schema, &array);
        REQUIRE(back.valid());
        CHECK(back.type() == TypeId::String);
        REQUIRE(back.length() == 3);
        CHECK(back.string_at(0) == "foo");
        CHECK(back.string_at(1) == "bar");
        CHECK(back.string_at(2) == "bazz");

        schema.release(&schema);
    }

    TEST_CASE("dictionary string exports as an Arrow dictionary of strings") {
        Series d = dictionary_encode(Series::strings(
            dftu_utils_test::span_of<dstd::StringView>({"x", "y", "x", "z"})));

        ArrowSchema schema;
        ArrowArray array;
        test_types::export_series(d, &schema, &array);
        CHECK(std::strcmp(schema.format, "i") == 0);              // int32 codes
        REQUIRE(schema.dictionary != nullptr);
        CHECK(std::strcmp(schema.dictionary->format, "u") == 0);  // string dict
        REQUIRE(array.dictionary != nullptr);
        CHECK(array.length == 4);
        CHECK(array.dictionary->length == 3);  // x, y, z distinct

        array.release(&array);
        schema.release(&schema);
    }

    TEST_CASE("list<struct> exports as a nested Arrow array") {
        dstd::Vector<double> lo{0.0, 1.0, 5.0};
        dstd::Vector<double> hi{1.0, 2.0, 6.0};
        dstd::Vector<std::int64_t> cnt{10, 20, 30};
        dstd::Vector<Series> fields;
        fields.push_back(Series::flat_f64(lo.data(), 3));
        fields.push_back(Series::flat_f64(hi.data(), 3));
        fields.push_back(Series::flat_i64(cnt.data(), 3));
        Series hist = Series::list(
            dstd::Vector<std::int32_t>{0, 2, 3},
            Series::structs({"lo", "hi", "count"}, std::move(fields)));

        ArrowSchema schema;
        ArrowArray array;
        test_types::export_series(hist, &schema, &array);

        CHECK(std::strcmp(schema.format, "+l") == 0);               // list
        REQUIRE(schema.n_children == 1);
        CHECK(std::strcmp(schema.children[0]->format, "+s") == 0);  // struct
        REQUIRE(schema.children[0]->n_children == 3);
        CHECK(std::strcmp(schema.children[0]->children[0]->name, "lo") == 0);
        CHECK(std::strcmp(schema.children[0]->children[2]->name, "count") == 0);

        REQUIRE(array.length == 2);
        REQUIRE(array.n_children == 1);
        const std::int32_t* off =
            static_cast<const std::int32_t*>(array.buffers[1]);
        CHECK(off[0] == 0);
        CHECK(off[1] == 2);
        CHECK(off[2] == 3);
        ArrowArray* sa = array.children[0];
        REQUIRE(sa->length == 3);  // flattened bins
        REQUIRE(sa->n_children == 3);
        const double* lod =
            static_cast<const double*>(sa->children[0]->buffers[1]);
        const std::int64_t* cntd =
            static_cast<const std::int64_t*>(sa->children[2]->buffers[1]);
        CHECK(lod[2] == doctest::Approx(5.0));
        CHECK(cntd[1] == 20);

        array.release(&array);
        schema.release(&schema);
    }

    TEST_CASE("bool round-trips through Arrow (bit-packed)") {
        dstd::Vector<std::int64_t> v{5, 1, 9, 3, 7};
        Series b = gt(Series::flat_i64(v.data(), 5), 4);

        ArrowSchema schema;
        ArrowArray array;
        test_types::export_series(b, &schema, &array);
        CHECK(std::strcmp(schema.format, "b") == 0);

        Series back = test_types::import_series(&schema, &array);
        REQUIRE(back.valid());
        CHECK(back.type() == TypeId::Bool);
        REQUIRE(back.length() == 5);
        const std::uint8_t* bits = back.data<std::uint8_t>();
        CHECK(((bits[0] >> 0) & 1) == 1);
        CHECK(((bits[0] >> 1) & 1) == 0);
        CHECK(((bits[0] >> 2) & 1) == 1);

        schema.release(&schema);
    }
}
