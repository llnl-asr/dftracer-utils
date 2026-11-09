// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/field.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/query.h>
#include <doctest/doctest.h>
#include <memory/fresh_process.h>

#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

DataFrame make_frame() {
    dstd::Vector<std::int64_t> a{1, 2, 3, 4};
    dstd::Vector<std::int64_t> b{10, 20, 30, 40};
    dstd::Vector<std::int64_t> dur{50, 150, 200, 80};
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
        auto q = (field::F("dur") > 1000).to_duql();
        REQUIRE(q.has_value());
        CHECK(q->to_string() == "dur > 1000");

        auto q2 =
            ((field::F("cat") == "io") && (field::F("dur") >= 100)).to_duql();
        REQUIRE(q2.has_value());
        CHECK(q2->references("cat"));
        CHECK(q2->references("dur"));
    }

    TEST_CASE("string-match and membership push down") {
        auto like = (field::F("name").like("read%")).to_duql();
        REQUIRE(like.has_value());
        CHECK(like->references("name"));
        auto in = (field::F("cat").in(dstd::Vector<dstd::String>{"io", "net"}))
                      .to_duql();
        REQUIRE(in.has_value());
        CHECK(in->references("cat"));
    }

    TEST_CASE("comparison also evaluates in memory to a mask") {
        DataFrame df = make_frame();
        auto applied = (field::F("dur") > 100).apply(df);
        REQUIRE(applied.has_value());
        Series mask = std::move(*applied);
        REQUIRE(mask.type() == dftracer::utils::dataframe::TypeId::Bool);
        REQUIRE(mask.length() == 4);
        CHECK(bit_at(mask, 0) == 0);  // 50
        CHECK(bit_at(mask, 1) == 1);  // 150
        CHECK(bit_at(mask, 2) == 1);  // 200
        CHECK(bit_at(mask, 3) == 0);  // 80
    }

    TEST_CASE("arithmetic value expression evaluates on a frame") {
        DataFrame df = make_frame();
        auto applied = (field::F("a") + field::F("b")).apply(df);
        REQUIRE(applied.has_value());
        Series out = std::move(*applied);
        REQUIRE(out.length() == 4);
        const std::int64_t* v = out.data<std::int64_t>();
        CHECK(v[0] == 11);
        CHECK(v[1] == 22);
        CHECK(v[2] == 33);
        CHECK(v[3] == 44);
    }

    TEST_CASE("numeric primitive evaluates on a frame") {
        DataFrame df = make_frame();
        auto applied = field::F("b").ilog2().apply(df);
        REQUIRE(applied.has_value());
        Series out = std::move(*applied);
        const std::int64_t* v = out.data<std::int64_t>();
        // ilog2(10)=3, ilog2(20)=4, ilog2(30)=4, ilog2(40)=5.
        CHECK(v[0] == 3);
        CHECK(v[1] == 4);
        CHECK(v[2] == 4);
        CHECK(v[3] == 5);
    }

    TEST_CASE("non-pushable predicate fails on to_duql but applies") {
        DataFrame df = make_frame();
        field::FieldExpr non_pushable = (field::F("a") + field::F("b")) > 3;
        auto q = non_pushable.to_duql();
        REQUIRE_FALSE(q.has_value());
        CHECK(common::errorcode_of(q.error()) ==
              common::ErrorCode::InvalidArgument);

        auto applied = non_pushable.apply(df);  // (a+b) > 3 -> all true
        REQUIRE(applied.has_value());
        Series mask = std::move(*applied);
        REQUIRE(mask.length() == 4);
        for (int i = 0; i < 4; ++i) CHECK(bit_at(mask, i) == 1);
    }

    TEST_CASE("filter-only predicate fails on apply") {
        DataFrame df = make_frame();
        auto like = field::F("cat").like("i%").apply(df);
        REQUIRE_FALSE(like.has_value());
        CHECK(common::errorcode_of(like.error()) ==
              common::ErrorCode::InvalidArgument);
        auto eq = (field::F("cat") == "io").apply(df);
        REQUIRE_FALSE(eq.has_value());
        CHECK(common::errorcode_of(eq.error()) ==
              common::ErrorCode::InvalidArgument);
    }

    TEST_CASE("missing column fails on apply") {
        DataFrame df = make_frame();
        auto missing = field::F("missing").apply(df);
        REQUIRE_FALSE(missing.has_value());
        CHECK(common::errorcode_of(missing.error()) ==
              common::ErrorCode::InvalidArgument);
    }

    TEST_CASE("an empty field expression ends the process" * doctest::skip()) {
        fresh_process::announce_abort();
        field::FieldExpr empty;
        (void)empty.to_duql();
    }

    TEST_CASE("an empty field expression is fatal") {
        auto result =
            fresh_process::run("an empty field expression ends the process");
        CHECK(result.aborted_with("empty field expression"));
    }

    TEST_CASE("a match on a non-field expression ends the process" *
              doctest::skip()) {
        fresh_process::announce_abort();
        (void)(field::F("a") + field::F("b")).like("x%");
    }

    TEST_CASE("a match on a non-field expression is fatal") {
        auto result = fresh_process::run(
            "a match on a non-field expression ends the process");
        CHECK(result.aborted_with("needs a bare field on the left"));
    }

    TEST_CASE("an aggregate of a non-field expression ends the process" *
              doctest::skip()) {
        fresh_process::announce_abort();
        (void)(field::F("a") + field::F("b")).sum();
    }

    TEST_CASE("an aggregate of a non-field expression is fatal") {
        auto result = fresh_process::run(
            "an aggregate of a non-field expression ends the process");
        CHECK(result.aborted_with("aggregate needs a bare field"));
    }
}
