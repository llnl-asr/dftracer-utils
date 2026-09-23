#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/config.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/dataframe/arrow.h>

using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::OwnedArrow;
using dftracer::utils::dataframe::Series;

TEST_SUITE("dataframe_arrow_public") {
    TEST_CASE("Series round-trips through the public Arrow bridge") {
        std::vector<std::int64_t> vals = {10, 20, 30, 40};
        Series s = Series::flat_i64(vals.data(),
                                    static_cast<std::int64_t>(vals.size()));

        OwnedArrow a = s.to_arrow();
        CHECK(static_cast<bool>(a));

        Series back = Series::from_arrow(a.schema(), a.array());
        REQUIRE(back.valid());
        REQUIRE(back.length() == 4);
        const std::int64_t* d = back.data<std::int64_t>();
        REQUIRE(d != nullptr);
        for (std::size_t i = 0; i < vals.size(); ++i) CHECK(d[i] == vals[i]);
    }

    TEST_CASE("String Series round-trips through the public Arrow bridge") {
        Series s = Series::strings({"alpha", "beta", "gamma"});

        OwnedArrow a = s.to_arrow();
        Series back = Series::from_arrow(a.schema(), a.array());
        REQUIRE(back.valid());
        REQUIRE(back.length() == 3);
        CHECK(back.string_at(0) == "alpha");
        CHECK(back.string_at(1) == "beta");
        CHECK(back.string_at(2) == "gamma");
    }

    TEST_CASE("DataFrame round-trips schema, columns, and values") {
        std::vector<std::int64_t> ints = {1, 2, 3};
        std::vector<double> reals = {1.5, 2.5, 3.5};

        DataFrame df;
        df.names = {"id", "value"};
        df.columns.push_back(Series::flat_i64(
            ints.data(), static_cast<std::int64_t>(ints.size())));
        df.columns.push_back(Series::flat_f64(
            reals.data(), static_cast<std::int64_t>(reals.size())));

        OwnedArrow a = df.to_arrow();
        CHECK(static_cast<bool>(a));

        DataFrame back = DataFrame::from_arrow(a.schema(), a.array());
        REQUIRE(back.num_columns() == 2);
        REQUIRE(back.num_rows() == 3);
        REQUIRE(back.names.size() == 2);
        CHECK(back.names[0] == "id");
        CHECK(back.names[1] == "value");

        Series id = back.column("id");
        Series value = back.column("value");
        REQUIRE(id.valid());
        REQUIRE(value.valid());
        const std::int64_t* idp = id.data<std::int64_t>();
        const double* vp = value.data<double>();
        REQUIRE(idp != nullptr);
        REQUIRE(vp != nullptr);
        for (std::size_t i = 0; i < ints.size(); ++i) {
            CHECK(idp[i] == ints[i]);
            CHECK(vp[i] == doctest::Approx(reals[i]));
        }
    }

    TEST_CASE("Dictionary column round-trips through the Arrow bridge") {
        Series s = Series::strings({"a", "b", "a", "c", "b", "a"});
        Series dict = s.dictionary_encode();
        REQUIRE(dict.valid());
        REQUIRE(dict.encoding() ==
                dftracer::utils::dataframe::Encoding::Dictionary);

        OwnedArrow a = dict.to_arrow();
        CHECK(static_cast<bool>(a));

        Series back = Series::from_arrow(a.schema(), a.array());
        REQUIRE(back.valid());
        REQUIRE(back.length() == 6);
        CHECK(back.encoding() ==
              dftracer::utils::dataframe::Encoding::Dictionary);

        Series flat = back.materialize();
        REQUIRE(flat.valid());
        REQUIRE(flat.length() == 6);
        const char* expected[] = {"a", "b", "a", "c", "b", "a"};
        for (std::int64_t i = 0; i < 6; ++i)
            CHECK(flat.string_at(i) == expected[i]);
    }

    TEST_CASE("Sliced dictionary array round-trips through the Arrow bridge") {
        Series s = Series::strings({"a", "b", "a", "c", "b", "a"});
        Series dict = s.dictionary_encode();
        REQUIRE(dict.valid());

        OwnedArrow a = dict.to_arrow();
        CHECK(static_cast<bool>(a));

        // Slice off the first 2 rows via the Arrow offset, keeping the same
        // index/dictionary buffers: import must honor the index-buffer offset.
        a.array()->offset = 2;
        a.array()->length = 4;

        Series back = Series::from_arrow(a.schema(), a.array());
        REQUIRE(back.valid());
        REQUIRE(back.length() == 4);

        Series flat = back.materialize();
        REQUIRE(flat.valid());
        REQUIRE(flat.length() == 4);
        const char* expected[] = {"a", "c", "b", "a"};
        for (std::int64_t i = 0; i < 4; ++i)
            CHECK(flat.string_at(i) == expected[i]);
    }

    TEST_CASE("Struct column round-trips through the Arrow bridge") {
        std::vector<std::int64_t> a = {10, 20, 30};
        std::vector<double> b = {1.5, 2.5, 3.5};
        std::vector<Series> fields;
        fields.push_back(Series::flat_i64(a.data(), 3));
        fields.push_back(Series::flat_f64(b.data(), 3));
        Series st = Series::structs({"a", "b"}, std::move(fields));

        DataFrame df;
        df.names = {"s"};
        df.columns.push_back(std::move(st));

        OwnedArrow arw = df.to_arrow();
        DataFrame back = DataFrame::from_arrow(arw.schema(), arw.array());
        REQUIRE(back.num_columns() == 1);
        REQUIRE(back.num_rows() == 3);
        Series s = back.column("s");
        REQUIRE(s.valid());
        REQUIRE(s.num_children() == 2);
        const std::int64_t* ap = s.child(0).data<std::int64_t>();
        const double* bp = s.child(1).data<double>();
        REQUIRE(ap != nullptr);
        REQUIRE(bp != nullptr);
        for (std::size_t i = 0; i < a.size(); ++i) {
            CHECK(ap[i] == a[i]);
            CHECK(bp[i] == doctest::Approx(b[i]));
        }
    }
}
#else
TEST_SUITE("dataframe_arrow_public") {
    TEST_CASE("Arrow disabled: nothing to exercise") { CHECK(true); }
}
#endif
