// SPDX-License-Identifier: MIT
// concat accepts string and large_string columns, alone or mixed: the result is
// a string view column, nulls and order are kept, and a string next to an
// integer still fails.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dataframe/builders_types.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <cstdint>
#include <stdexcept>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;
using namespace dftracer::utils::dataframe::test_types;

namespace {

Cells cells_of(const Series& s) {
    const Series m = s.materialize();
    Cells out;
    for (std::int64_t i = 0; i < m.length(); ++i)
        out.push_back(m.is_null(i) ? dstd::nullopt
                                   : dstd::Optional<dstd::String>(
                                         dstd::String(m.string_at(i))));
    return out;
}

}  // namespace

TEST_SUITE("concat string and large_string") {
    TEST_CASE("two large_string parts concatenate into a string view") {
        Series a = make_large_utf8({"a", "bb"});
        Series b = make_large_utf8({"ccc", "dddd"});
        REQUIRE(a.type() == TypeId::LargeString);
        Series out = dftu_utils_test::ok(dataframe::concat_columns({&a, &b}));
        REQUIRE(out.valid());
        CHECK(out.type() == TypeId::String);
        CHECK(out.encoding() == dftracer::utils::dataframe::Encoding::View);
        CHECK(cells_of(out) == Cells{"a", "bb", "ccc", "dddd"});
    }

    TEST_CASE("a string part and a large_string part, in either order") {
        Series s = make_string({"x", "yy"});
        Series l = make_large_utf8({"zzz"});
        Series sl = dftu_utils_test::ok(dataframe::concat_columns({&s, &l}));
        Series ls = dftu_utils_test::ok(dataframe::concat_columns({&l, &s}));
        CHECK(sl.type() == TypeId::String);
        CHECK(ls.type() == TypeId::String);
        CHECK(cells_of(sl) == Cells{"x", "yy", "zzz"});
        CHECK(cells_of(ls) == Cells{"zzz", "x", "yy"});
    }

    TEST_CASE("nulls are kept at the same rows") {
        Series a = make_large_utf8({"a", dstd::nullopt, "c"});
        Series b = make_string({dstd::nullopt, "e"});
        Series out = dftu_utils_test::ok(dataframe::concat_columns({&a, &b}));
        CHECK(out.null_count() == 2);
        CHECK(cells_of(out) ==
              Cells{"a", dstd::nullopt, "c", dstd::nullopt, "e"});
    }

    TEST_CASE("empty values and an empty part") {
        Series a = make_large_utf8({"", "x"});
        Series e = make_large_utf8({});
        Series out =
            dftu_utils_test::ok(dataframe::concat_columns({&a, &e, &a}));
        CHECK(cells_of(out) == Cells{"", "x", "", "x"});
    }

    TEST_CASE("string parts alone stay string") {
        Series a = make_string({"a"});
        Series b = make_string({"b", dstd::nullopt});
        Series out = dftu_utils_test::ok(dataframe::concat_columns({&a, &b}));
        CHECK(out.type() == TypeId::String);
        CHECK(cells_of(out) == Cells{"a", "b", dstd::nullopt});
    }

    TEST_CASE("a string next to an integer still fails") {
        Series a = make_large_utf8({"a"});
        const dstd::Vector<std::int64_t> v{1};
        Series i = Series::flat_i64(v.data(), 1);
        dftu_utils_test::expect_error(
            dataframe::concat_columns({&a, &i}),
            dftracer::utils::common::ErrorCode::InvalidArgument);
        dftu_utils_test::expect_error(
            dataframe::concat_columns({&i, &a}),
            dftracer::utils::common::ErrorCode::InvalidArgument);
    }

    TEST_CASE("a frame concat keeps every column") {
        DataFrame x, y;
        x.names = dstd::make_shared_strings({"s", "v"});
        y.names = dstd::make_shared_strings({"s", "v"});
        const dstd::Vector<std::int64_t> v0{1, 2}, v1{3};
        x.columns.push_back(make_large_utf8({"a", "b"}));
        x.columns.push_back(Series::flat_i64(v0.data(), 2));
        y.columns.push_back(make_string({"c"}));
        y.columns.push_back(Series::flat_i64(v1.data(), 1));
        DataFrame out = dftu_utils_test::ok(dataframe::concat({&x, &y}));
        REQUIRE(out.num_rows() == 3);
        CHECK(cells_of(out.column("s")) == Cells{"a", "b", "c"});
    }

    TEST_CASE("a lazy concat of a string and a large_string plan collects") {
        DataFrame x, y;
        x.names = dstd::make_shared_strings({"s"});
        y.names = dstd::make_shared_strings({"s"});
        x.columns.push_back(make_string({"a", "b"}));
        y.columns.push_back(make_large_utf8({"c"}));
        LazyFrame lx =
            LazyFrame::scan(dstd::make_shared<InMemorySource>(std::move(x)));
        LazyFrame ly =
            LazyFrame::scan(dstd::make_shared<InMemorySource>(std::move(y)));
        DataFrame out = dftu_utils_test::ok(dftracer::utils::default_runtime()
                                                .submit(lx.concat(ly).collect())
                                                .get());
        CHECK(cells_of(out.column("s")) == Cells{"a", "b", "c"});
    }
}
