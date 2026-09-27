#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/field.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/duql/query.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

using dftracer::utils::DFTUtilsException;
using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::Series;
using dftracer::utils::dataframe::field::F;
using dftracer::utils::dataframe::field::FieldExpr;

namespace {

DataFrame make_frame() {
    std::vector<std::int64_t> a{1, 2, 3, 4};
    std::vector<std::int64_t> b{10, 20, 30, 40};
    std::vector<std::int64_t> dur{50, 150, 200, 80};
    DataFrame df;
    df.names = {"a", "b", "dur", "cat"};
    df.columns.push_back(Series::flat_i64(a.data(), 4));
    df.columns.push_back(Series::flat_i64(b.data(), 4));
    df.columns.push_back(Series::flat_i64(dur.data(), 4));
    df.columns.push_back(Series::strings({"io", "cpu", "io", "io"}));
    return df;
}

int bit_at(const Series& mask, int i) {
    const std::uint8_t* bits = mask.data<std::uint8_t>();
    return (bits[i >> 3] >> (i & 7)) & 1;
}

}  // namespace

TEST_SUITE("unified field F") {
    TEST_CASE("comparison serializes to a pushable query") {
        auto q = (F("dur") > 1000).to_duql();
        CHECK(q.to_string() == "dur > 1000");

        auto q2 = ((F("cat") == "io") && (F("dur") >= 100)).to_duql();
        CHECK(q2.references("cat"));
        CHECK(q2.references("dur"));
    }

    TEST_CASE("string-match and membership push down") {
        CHECK((F("name").like("read%")).to_duql().references("name"));
        CHECK((F("cat").in(std::vector<std::string>{"io", "net"}))
                  .to_duql()
                  .references("cat"));
    }

    TEST_CASE("comparison also evaluates in memory to a mask") {
        DataFrame df = make_frame();
        Series mask = (F("dur") > 100).apply(df);
        REQUIRE(mask.type() == dftracer::utils::dataframe::TypeId::Bool);
        REQUIRE(mask.length() == 4);
        CHECK(bit_at(mask, 0) == 0);  // 50
        CHECK(bit_at(mask, 1) == 1);  // 150
        CHECK(bit_at(mask, 2) == 1);  // 200
        CHECK(bit_at(mask, 3) == 0);  // 80
    }

    TEST_CASE("arithmetic value expression evaluates on a frame") {
        DataFrame df = make_frame();
        Series out = (F("a") + F("b")).apply(df);
        REQUIRE(out.length() == 4);
        const std::int64_t* v = out.data<std::int64_t>();
        CHECK(v[0] == 11);
        CHECK(v[1] == 22);
        CHECK(v[2] == 33);
        CHECK(v[3] == 44);
    }

    TEST_CASE("numeric primitive evaluates on a frame") {
        DataFrame df = make_frame();
        Series out = F("b").ilog2().apply(df);
        const std::int64_t* v = out.data<std::int64_t>();
        // ilog2(10)=3, ilog2(20)=4, ilog2(30)=4, ilog2(40)=5.
        CHECK(v[0] == 3);
        CHECK(v[1] == 4);
        CHECK(v[2] == 4);
        CHECK(v[3] == 5);
    }

    TEST_CASE("non-pushable predicate raises on to_duql but applies") {
        DataFrame df = make_frame();
        FieldExpr non_pushable = (F("a") + F("b")) > 3;
        CHECK_THROWS_AS(non_pushable.to_duql(), DFTUtilsException);

        Series mask = non_pushable.apply(df);  // (a+b) > 3 -> all true
        REQUIRE(mask.length() == 4);
        for (int i = 0; i < 4; ++i) CHECK(bit_at(mask, i) == 1);
    }

    TEST_CASE("filter-only predicate raises on apply") {
        DataFrame df = make_frame();
        CHECK_THROWS_AS(F("cat").like("i%").apply(df), DFTUtilsException);
        CHECK_THROWS_AS((F("cat") == "io").apply(df), DFTUtilsException);
    }

    TEST_CASE("missing column raises on apply") {
        DataFrame df = make_frame();
        CHECK_THROWS_AS(F("missing").apply(df), DFTUtilsException);
    }
}
