#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/hash/fnv1a.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/agg_expr.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/field_stat.h>
#include <dftracer/utils/dataframe/kernels/elementwise.h>
#include <dftracer/utils/dataframe/kernels/field_stat.h>
#include <dftracer/utils/dataframe/kernels/kernels.h>
#include <dftracer/utils/dataframe/kernels/prims.h>
#include <dftracer/utils/dataframe/kernels/sort.h>
#include <dftracer/utils/dataframe/kernels/stats.h>
#include <dftracer/utils/dataframe/op.h>
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>
#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/dataframe/arrow_bridge.h>
#include <nanoarrow/nanoarrow.h>

#include <cstring>
#endif

using dftracer::utils::dataframe::add;
using dftracer::utils::dataframe::add_scalar;
using dftracer::utils::dataframe::argsort;
using dftracer::utils::dataframe::cast;
using dftracer::utils::dataframe::count;
using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::dictionary_encode;
using dftracer::utils::dataframe::div;
using dftracer::utils::dataframe::Encoding;
using dftracer::utils::dataframe::eq;
using dftracer::utils::dataframe::filter;
using dftracer::utils::dataframe::filter_gt;
using dftracer::utils::dataframe::ge;
using dftracer::utils::dataframe::group_by;
using dftracer::utils::dataframe::gt;
using dftracer::utils::dataframe::le;
using dftracer::utils::dataframe::logical_and;
using dftracer::utils::dataframe::logical_not;
using dftracer::utils::dataframe::lt;
using dftracer::utils::dataframe::materialize;
using dftracer::utils::dataframe::max;
using dftracer::utils::dataframe::mean;
using dftracer::utils::dataframe::min;
using dftracer::utils::dataframe::mul;
using dftracer::utils::dataframe::mul_scalar;
using dftracer::utils::dataframe::ne;
using dftracer::utils::dataframe::Prim;
using dftracer::utils::dataframe::prim;
using dftracer::utils::dataframe::rename;
using dftracer::utils::dataframe::scalar_value;
using dftracer::utils::dataframe::select;
using dftracer::utils::dataframe::Series;
using dftracer::utils::dataframe::slice;
using dftracer::utils::dataframe::sort_by;
using dftracer::utils::dataframe::str_contains;
using dftracer::utils::dataframe::str_eq;
using dftracer::utils::dataframe::str_starts_with;
using dftracer::utils::dataframe::sub;
using dftracer::utils::dataframe::sub_scalar;
using dftracer::utils::dataframe::sum;
using dftracer::utils::dataframe::take;
using dftracer::utils::dataframe::TypeId;
using dftracer::utils::dataframe::with_column;
namespace dv = dftracer::utils::dataframe;

static bool mask_bit(const Series& m, std::int64_t i) {
    const std::uint8_t* b = m.data<std::uint8_t>();
    return ((b[i >> 3] >> (i & 7)) & 1) != 0;
}

TEST_SUITE("vec") {
    TEST_CASE("flat int64 add") {
        std::vector<std::int64_t> a{1, 2, 3, 4};
        std::vector<std::int64_t> b{10, 20, 30, 40};
        Series ca = Series::flat_i64(a.data(), 4);
        Series cb = Series::flat_i64(b.data(), 4);

        Series sum = add(ca, cb);
        REQUIRE(sum.valid());
        CHECK(sum.type() == TypeId::Int64);
        CHECK(sum.encoding() == Encoding::Flat);
        REQUIRE(sum.length() == 4);
        const std::int64_t* p = sum.data<std::int64_t>();
        CHECK(p[0] == 11);
        CHECK(p[1] == 22);
        CHECK(p[2] == 33);
        CHECK(p[3] == 44);
    }

    TEST_CASE("flat float64 add") {
        std::vector<double> a{1.5, 2.5};
        std::vector<double> b{0.5, 0.5};
        Series sum =
            add(Series::flat_f64(a.data(), 2), Series::flat_f64(b.data(), 2));
        REQUIRE(sum.valid());
        const double* p = sum.data<double>();
        CHECK(p[0] == doctest::Approx(2.0));
        CHECK(p[1] == doctest::Approx(3.0));
    }

    TEST_CASE("int64 sub and mul") {
        std::vector<std::int64_t> a{10, 20, 30};
        std::vector<std::int64_t> b{1, 2, 3};
        Series d =
            sub(Series::flat_i64(a.data(), 3), Series::flat_i64(b.data(), 3));
        REQUIRE(d.valid());
        const std::int64_t* pd = d.data<std::int64_t>();
        CHECK(pd[0] == 9);
        CHECK(pd[2] == 27);

        Series m =
            mul(Series::flat_i64(a.data(), 3), Series::flat_i64(b.data(), 3));
        REQUIRE(m.valid());
        const std::int64_t* pm = m.data<std::int64_t>();
        CHECK(pm[1] == 40);
        CHECK(pm[2] == 90);
    }

    TEST_CASE("mixed-type arithmetic promotes instead of returning null") {
        std::vector<std::uint64_t> ua{1, 2, 3};
        std::vector<double> fb{0.5, 1.5, 2.5};
        Series su = Series::flat(TypeId::Uint64, ua.data(), 3);
        Series sf = Series::flat_f64(fb.data(), 3);

        Series sum = add(su, sf);
        REQUIRE(sum.valid());
        CHECK(sum.type() == TypeId::Float64);
        const double* p = sum.data<double>();
        CHECK(p[0] == doctest::Approx(1.5));
        CHECK(p[1] == doctest::Approx(3.5));
        CHECK(p[2] == doctest::Approx(5.5));

        std::vector<std::int64_t> ia{2, 4, 6};
        Series si = Series::flat_i64(ia.data(), 3);
        Series prod = mul_scalar(si, 1.0);
        REQUIRE(prod.valid());
        CHECK(prod.type() == TypeId::Float64);
        const double* pp = prod.data<double>();
        CHECK(pp[0] == doctest::Approx(2.0));
        CHECK(pp[2] == doctest::Approx(6.0));

        Series uprod = mul_scalar(su, 1.0);
        REQUIRE(uprod.valid());
        CHECK(uprod.type() == TypeId::Float64);
        CHECK(uprod.data<double>()[2] == doctest::Approx(3.0));
    }

    TEST_CASE("float64 div (SIMD) and int64 div zero guard") {
        std::vector<double> a{1.0, 3.0, 9.0};
        std::vector<double> b{2.0, 2.0, 3.0};
        Series q =
            div(Series::flat_f64(a.data(), 3), Series::flat_f64(b.data(), 3));
        REQUIRE(q.valid());
        const double* pq = q.data<double>();
        CHECK(pq[0] == doctest::Approx(0.5));
        CHECK(pq[2] == doctest::Approx(3.0));

        std::vector<std::int64_t> ia{10, 20, 30};
        std::vector<std::int64_t> ib{2, 0, 3};  // divide by zero -> 0
        Series iq =
            div(Series::flat_i64(ia.data(), 3), Series::flat_i64(ib.data(), 3));
        REQUIRE(iq.valid());
        const std::int64_t* piq = iq.data<std::int64_t>();
        CHECK(piq[0] == 5);
        CHECK(piq[1] == 0);
        CHECK(piq[2] == 10);
    }

    TEST_CASE("arithmetic operators (series and scalar, reflected)") {
        std::vector<std::int64_t> a{1, 2, 3, 4};
        std::vector<std::int64_t> b{10, 20, 30, 40};
        Series ca = Series::flat_i64(a.data(), 4);
        Series cb = Series::flat_i64(b.data(), 4);

        Series ss = ca + cb;  // series + series
        REQUIRE(ss.valid());
        CHECK(ss.data<std::int64_t>()[3] == 44);

        Series plus = ca + 10;  // series + scalar
        REQUIRE(plus.valid());
        CHECK(plus.data<std::int64_t>()[0] == 11);
        CHECK(plus.data<std::int64_t>()[3] == 14);

        Series times = ca * 3;
        REQUIRE(times.valid());
        CHECK(times.data<std::int64_t>()[2] == 9);

        Series rmul = 3 * ca;  // reflected, commutative
        REQUIRE(rmul.valid());
        CHECK(rmul.data<std::int64_t>()[2] == 9);

        Series rsub = 10 - ca;  // reflected subtract: 10 - a
        REQUIRE(rsub.valid());
        CHECK(rsub.data<std::int64_t>()[0] == 9);
        CHECK(rsub.data<std::int64_t>()[3] == 6);

        std::vector<double> f{2.0, 4.0, 8.0};
        Series cf = Series::flat_f64(f.data(), 3);
        Series half = cf / 2.0;  // float scalar divide
        REQUIRE(half.valid());
        CHECK(half.data<double>()[2] == doctest::Approx(4.0));
    }

    TEST_CASE("unary primitives (ilog2, popcount, mix64)") {
        std::vector<std::int64_t> x{1000, 2000, 4, 0};
        Series il = prim(Series::flat_i64(x.data(), 4), Prim::Ilog2);
        REQUIRE(il.valid());
        CHECK(il.type() == TypeId::Int64);
        const std::int64_t* pl = il.data<std::int64_t>();
        CHECK(pl[0] == 9);  // floor log2(1000)
        CHECK(pl[2] == 2);  // log2(4)
        CHECK(pl[3] == 0);  // ilog2(0) == 0

        Series pc = prim(Series::flat_i64(x.data(), 4), Prim::Popcount);
        const std::int64_t* pp = pc.data<std::int64_t>();
        CHECK(pp[2] == 1);  // popcount(4)

        // mix64 is deterministic and disperses distinct inputs.
        Series mx = prim(Series::flat_i64(x.data(), 4), Prim::Mix64);
        const std::int64_t* pm = mx.data<std::int64_t>();
        CHECK(pm[0] != pm[1]);
        CHECK(pm[0] != 1000);
    }

    TEST_CASE(
        "floordiv, mod, pow follow Python; ffill, bfill; cum scans keep "
        "nulls; String parses to a number") {
        // a = [7, -7, null, 5], b = [2, 2, 2, 0]
        const std::int64_t av[4] = {7, -7, 0, 5};
        const std::uint8_t valid[1] = {0x0b};
        const std::int64_t bv[4] = {2, 2, 2, 0};
        Series a = Series::flat_i64(av, 4, valid);
        Series b = Series::flat_i64(bv, 4);
        Series q = a.floordiv(b);
        REQUIRE(q.valid());
        CHECK(q.type() == TypeId::Int64);
        CHECK(q.data<std::int64_t>()[0] == 3);
        CHECK(q.data<std::int64_t>()[1] == -4);  // floor toward -inf
        CHECK(q.is_null(2));                     // null in
        CHECK(q.is_null(3));                     // zero divisor
        Series r =
            a.mod(Series{dftu_series_mul_scalar(b.handle(), dv::i64(-1))});
        CHECK(r.data<std::int64_t>()[0] == -1);  // the divisor's sign
        CHECK(r.data<std::int64_t>()[1] == -1);
        Series p = Series{dftu_series_pow_scalar(a.handle(), dv::i64(2), 0)};
        CHECK(p.data<std::int64_t>()[0] == 49);
        CHECK(p.is_null(2));
        Series rp = Series{dftu_series_pow_scalar(a.handle(), dv::i64(2), 1)};
        CHECK(rp.data<std::int64_t>()[0] == 128);
        CHECK(rp.is_null(1));  // 2 ** -7 is not an integer
        const double fv[3] = {7.5, -7.5, 2.0};
        Series f = Series::flat_f64(fv, 3);
        Series fq =
            Series{dftu_series_floordiv_scalar(f.handle(), dv::i64(2), 0)};
        CHECK(fq.type() == TypeId::Float64);
        CHECK(fq.data<double>()[1] == doctest::Approx(-4.0));
        Series fm = Series{dftu_series_mod_scalar(f.handle(), dv::i64(2), 0)};
        CHECK(fm.data<double>()[1] == doctest::Approx(0.5));
        CHECK(dftu_series_floordiv(a.handle(), f.handle()) ==
              nullptr);  // length
        CHECK(Series::strings({"x"}).floordiv(Series::strings({"y"})).valid() ==
              false);

        Series ff = a.ffill();
        CHECK(ff.null_count() == 0);
        CHECK(ff.data<std::int64_t>()[2] == -7);
        Series bf = a.bfill();
        CHECK(bf.data<std::int64_t>()[2] == 5);
        const std::uint8_t lead[1] = {0x0c};  // [null, null, 0, 5]
        Series l = Series::flat_i64(av, 4, lead);
        CHECK(l.ffill().null_count() == 2);
        CHECK(l.bfill().null_count() == 0);
        CHECK(l.bfill().data<std::int64_t>()[0] == 0);
        Series sf = Series::strings({"a", "b"}).ffill();
        CHECK(sf.string_at(1) == "b");

        Series cs = a.cumsum();
        CHECK(cs.is_null(2));
        CHECK(cs.data<std::int64_t>()[3] == 5);
        CHECK(a.cummax().is_null(2));
        CHECK(a.cummin().is_null(2));
        CHECK(a.cum_prod().is_null(2));

        Series parsed =
            Series::strings({"12", " 3 ", "x", "4.5"}).cast(TypeId::Int64);
        REQUIRE(parsed.valid());
        CHECK(parsed.data<std::int64_t>()[0] == 12);
        CHECK(parsed.data<std::int64_t>()[1] == 3);
        CHECK(parsed.is_null(2));
        CHECK(parsed.is_null(3));
        Series parsed_f =
            Series::strings({"12", "4.5", "x"}).cast(TypeId::Float64);
        CHECK(parsed_f.data<double>()[1] == doctest::Approx(4.5));
        CHECK(parsed_f.is_null(2));
        CHECK(Series::strings({"7"})
                  .cast(TypeId::Int32)
                  .data<std::int32_t>()[0] == 7);
    }

    TEST_CASE(
        "the .str batch: case forms, classes, count, rfind, prefix, "
        "suffix, repeat, center, cat, findall, partition, list_join") {
        Series s =
            Series::strings({"hello world", "Foo Bar", "ab1", "", "a-b-a"});
        CHECK(s.str_case(DFTU_STR_CAPITALIZE).string_at(1) == "Foo bar");
        CHECK(s.str_case(DFTU_STR_TITLE_CASE).string_at(0) == "Hello World");
        CHECK(s.str_case(DFTU_STR_SWAPCASE).string_at(1) == "fOO bAR");
        CHECK_FALSE(s.str_case(9).valid());
        Series alpha = s.str_is(DFTU_STR_ALPHA);
        const std::uint8_t* ab = alpha.data<std::uint8_t>();
        CHECK(((ab[0] >> 0) & 1) == 0);  // a space is not a letter
        CHECK(((ab[0] >> 2) & 1) == 0);  // ab1 holds a digit
        CHECK(((ab[0] >> 3) & 1) == 0);  // empty is false
        Series lower = s.str_is(DFTU_STR_LOWER);
        CHECK(((lower.data<std::uint8_t>()[0] >> 0) & 1) == 1);
        CHECK(((lower.data<std::uint8_t>()[0] >> 1) & 1) == 0);
        Series title = s.str_is(DFTU_STR_TITLE);
        CHECK(((title.data<std::uint8_t>()[0] >> 1) & 1) == 1);
        CHECK_FALSE(s.str_is(42).valid());
        CHECK(s.str_count("a").data<std::int64_t>()[4] == 2);
        CHECK(s.str_count("").data<std::int64_t>()[2] == 4);
        CHECK(s.str_rfind("a").data<std::int64_t>()[4] == 4);
        CHECK(s.str_rfind("z").data<std::int64_t>()[0] == -1);
        CHECK(s.str_remove_prefix("he").string_at(0) == "llo world");
        CHECK(s.str_remove_prefix("he").string_at(1) == "Foo Bar");
        CHECK(s.str_remove_suffix("a").string_at(4) == "a-b-");
        CHECK(s.str_repeat(2).string_at(2) == "ab1ab1");
        CHECK(s.str_repeat(0).string_at(2) == "");
        CHECK_FALSE(s.str_repeat(-1).valid());
        CHECK(s.str_center(7, '*').string_at(2) == "**ab1**");
        CHECK(s.str_center(6, '*').string_at(2) == "*ab1**");
        CHECK(s.str_center(2, '*').string_at(0) == "hello world");
        Series cat = s.str_cat(Series::strings({"!", "!", "!", "!", "!"}));
        CHECK(cat.string_at(0) == "hello world!");
        CHECK_FALSE(s.str_cat(Series::strings({"x"})).valid());
        Series found = s.str_findall("[a-z]+");
        REQUIRE(found.valid());
        CHECK(found.type() == TypeId::List);
        CHECK(found.offsets()[1] == 2);  // hello, world
        CHECK(found.offsets()[2] == 4);  // oo, ar
        CHECK(found.offsets()[3] == 5);  // ab
        CHECK(found.offsets()[4] == 5);  // "" adds none
        CHECK(found.child(0).string_at(0) == "hello");
        CHECK_FALSE(s.str_findall("(").valid());
        Series part = s.str_partition("-");
        CHECK(part.offsets()[5] == 15);  // three parts per row
        CHECK(part.child(0).string_at(12) == "a");
        CHECK(part.child(0).string_at(13) == "-");
        CHECK(part.child(0).string_at(14) == "b-a");
        CHECK(part.child(0).string_at(0) ==
              "hello world");  // no sep: (row, "", "")
        CHECK(part.child(0).string_at(2) == "");
        Series rpart = s.str_partition("-", true);
        CHECK(rpart.child(0).string_at(12) == "a-b");
        CHECK(rpart.child(0).string_at(2) == "hello world");  // ("", "", row)
        CHECK(rpart.child(0).string_at(0) == "");
        Series joined = s.str_split("-").list_join("+");
        CHECK(joined.string_at(4) == "a+b+a");
        CHECK(joined.string_at(0) == "hello world");
        CHECK_FALSE(s.list_join("+").valid());
    }

    TEST_CASE("dt_part and dt_round: calendar parts and bucket rounding") {
        // 2023-11-14 22:13:20.123456 UTC (a Tuesday), the epoch, one
        // microsecond before it, 2024-02-29.
        const std::int64_t us[4] = {1'700'000'000'123'456, 0, -1,
                                    1'709'164'800'000'000};
        Series s = Series::flat_i64(us, 4);
        std::vector<Series> keep;
        auto part = [&](dftu_dt_part p) {
            keep.push_back(s.dt_part(p, DFTU_TIME_UNIT_MICRO));
            REQUIRE(keep.back().valid());
            return keep.back().data<std::int64_t>();
        };
        CHECK(part(DFTU_DT_YEAR)[0] == 2023);
        CHECK(part(DFTU_DT_MONTH)[0] == 11);
        CHECK(part(DFTU_DT_DAY)[0] == 14);
        CHECK(part(DFTU_DT_HOUR)[0] == 22);
        CHECK(part(DFTU_DT_MINUTE)[0] == 13);
        CHECK(part(DFTU_DT_SECOND)[0] == 20);
        CHECK(part(DFTU_DT_MICROSECOND)[0] == 123456);
        CHECK(part(DFTU_DT_MILLISECOND)[0] == 123);
        CHECK(part(DFTU_DT_DAY_OF_WEEK)[0] == 1);  // Tuesday, Monday = 0
        CHECK(part(DFTU_DT_DAY_OF_WEEK)[1] == 3);  // the epoch, a Thursday
        CHECK(part(DFTU_DT_DAY_OF_YEAR)[0] == 318);
        CHECK(part(DFTU_DT_QUARTER)[0] == 4);
        CHECK(part(DFTU_DT_IS_LEAP_YEAR)[3] == 1);
        CHECK(part(DFTU_DT_DAYS_IN_MONTH)[3] == 29);
        CHECK(part(DFTU_DT_ISO_WEEK)[0] == 46);
        CHECK(part(DFTU_DT_ISO_YEAR)[1] == 1970);
        CHECK(part(DFTU_DT_EPOCH_DAYS)[2] == -1);  // floors before the epoch
        CHECK(part(DFTU_DT_YEAR)[2] == 1969);
        CHECK(part(DFTU_DT_HOUR)[2] == 23);
        // The same instants read in seconds.
        const std::int64_t secs[1] = {1'700'000'000};
        Series in_s = Series::flat_i64(secs, 1);
        CHECK(in_s.dt_part(DFTU_DT_HOUR, DFTU_TIME_UNIT_SECOND)
                  .data<std::int64_t>()[0] == 22);
        CHECK_FALSE(s.dt_part(99, DFTU_TIME_UNIT_MICRO).valid());
        CHECK_FALSE(Series::strings({"x"}).dt_part(0, 2).valid());

        const std::int64_t hour = 3'600'000'000;
        Series fl = s.dt_round(hour, DFTU_DT_FLOOR);
        CHECK(fl.data<std::int64_t>()[0] == 1'699'999'200'000'000);
        CHECK(fl.data<std::int64_t>()[2] == -hour);
        Series ce = s.dt_round(hour, DFTU_DT_CEIL);
        CHECK(ce.data<std::int64_t>()[0] == 1'699'999'200'000'000 + hour);
        CHECK(ce.data<std::int64_t>()[1] == 0);
        const std::int64_t halves[3] = {2'500'000, 7'500'000, 12'500'000};
        Series ro =
            Series::flat_i64(halves, 3).dt_round(5'000'000, DFTU_DT_ROUND);
        CHECK(ro.data<std::int64_t>()[0] == 0);  // half to even
        CHECK(ro.data<std::int64_t>()[1] == 10'000'000);
        CHECK(ro.data<std::int64_t>()[2] == 10'000'000);
        CHECK_FALSE(s.dt_round(0, DFTU_DT_FLOOR).valid());
        CHECK_FALSE(s.dt_round(hour, 7).valid());
    }

    TEST_CASE("Series method wrappers: rank, rolling, prim") {
        // rank: default AVERAGE tie-break, ascending. Ties (two 2s at rows 1,3)
        // share the average of their ordinal positions.
        std::vector<std::int64_t> r{3, 2, 1, 2};
        Series ranked =
            Series::flat_i64(r.data(), 4).rank(dv::RankMethod::Average);
        REQUIRE(ranked.valid());
        CHECK(ranked.type() == TypeId::Float64);
        const double* pr = ranked.data<double>();
        CHECK(pr[2] == doctest::Approx(1.0));  // value 1 is smallest
        CHECK(pr[0] == doctest::Approx(4.0));  // value 3 is largest
        CHECK(pr[1] == doctest::Approx(2.5));  // tied 2s -> (2+3)/2
        CHECK(pr[3] == doctest::Approx(2.5));

        // Dense ordinal min-tie: 1 -> rank 1, tied 2s -> rank 2, 3 -> rank 3.
        Series dense =
            Series::flat_i64(r.data(), 4).rank(dv::RankMethod::Dense);
        const double* pd = dense.data<double>();
        CHECK(pd[2] == doctest::Approx(1.0));
        CHECK(pd[1] == doctest::Approx(2.0));
        CHECK(pd[0] == doctest::Approx(3.0));

        // Min and Max: the tied 2s take the first and the last of their
        // ordinal positions (2 and 3).
        Series mn = Series::flat_i64(r.data(), 4).rank(dv::RankMethod::Min);
        Series mx = Series::flat_i64(r.data(), 4).rank(dv::RankMethod::Max);
        CHECK(mn.data<double>()[1] == doctest::Approx(2.0));
        CHECK(mn.data<double>()[3] == doctest::Approx(2.0));
        CHECK(mx.data<double>()[1] == doctest::Approx(3.0));
        CHECK(mx.data<double>()[3] == doctest::Approx(3.0));
        CHECK(mx.data<double>()[0] == doctest::Approx(4.0));
        CHECK(mx.data<double>()[2] == doctest::Approx(1.0));

        // rolling: trailing-window mean; first window-1 rows are null.
        std::vector<double> w{1.0, 2.0, 3.0, 4.0};
        Series roll =
            Series::flat_f64(w.data(), 4).rolling(dv::RollingOp::Mean, 2);
        REQUIRE(roll.valid());
        CHECK(roll.type() == TypeId::Float64);
        CHECK(roll.is_null(0));
        const double* pw = roll.data<double>();
        CHECK(pw[1] == doctest::Approx(1.5));  // mean(1,2)
        CHECK(pw[2] == doctest::Approx(2.5));  // mean(2,3)
        CHECK(pw[3] == doctest::Approx(3.5));  // mean(3,4)

        // rolling sum agrees with the free-standing kernel intent.
        Series rsum =
            Series::flat_f64(w.data(), 4).rolling(dv::RollingOp::Sum, 3);
        const double* ps = rsum.data<double>();
        CHECK(rsum.is_null(1));
        CHECK(ps[2] == doctest::Approx(6.0));  // 1+2+3
        CHECK(ps[3] == doctest::Approx(9.0));  // 2+3+4

        // prim: the method mirrors the free-function prim (ilog2 here).
        std::vector<std::int64_t> x{1000, 4, 0};
        Series il = Series::flat_i64(x.data(), 3).prim(dv::PrimOp::Ilog2);
        REQUIRE(il.valid());
        CHECK(il.type() == TypeId::Int64);
        const std::int64_t* pi = il.data<std::int64_t>();
        CHECK(pi[0] == 9);
        CHECK(pi[1] == 2);
        CHECK(pi[2] == 0);
    }

    TEST_CASE("add over other numeric types (int32, uint8, float32)") {
        std::vector<std::int32_t> ai{1, 2, 3};
        std::vector<std::int32_t> bi{100, 100, 100};
        Series si = add(Series::flat(TypeId::Int32, ai.data(), 3),
                        Series::flat(TypeId::Int32, bi.data(), 3));
        REQUIRE(si.valid());
        CHECK(si.type() == TypeId::Int32);
        CHECK(si.data<std::int32_t>()[2] == 103);

        std::vector<std::uint8_t> au{200, 50};
        std::vector<std::uint8_t> bu{55, 5};
        Series su = add(Series::flat(TypeId::Uint8, au.data(), 2),
                        Series::flat(TypeId::Uint8, bu.data(), 2));
        REQUIRE(su.valid());
        CHECK(su.data<std::uint8_t>()[0] == 255);

        std::vector<float> af{1.5f, 2.5f};
        std::vector<float> bf{0.25f, 0.25f};
        Series sf = add(Series::flat(TypeId::Float32, af.data(), 2),
                        Series::flat(TypeId::Float32, bf.data(), 2));
        REQUIRE(sf.valid());
        CHECK(sf.data<float>()[0] == doctest::Approx(1.75f));
    }

    TEST_CASE("compare produces a bit-packed bool column") {
        std::vector<std::int64_t> v{5, 1, 9, 3, 7};
        Series col = Series::flat_i64(v.data(), 5);

        Series b = gt(col, 4);  // 1,0,1,0,1
        REQUIRE(b.valid());
        CHECK(b.type() == TypeId::Bool);
        REQUIRE(b.length() == 5);
        const std::uint8_t* bits = b.data<std::uint8_t>();
        auto bit = [&](int i) { return (bits[i >> 3] >> (i & 7)) & 1; };
        CHECK(bit(0) == 1);
        CHECK(bit(1) == 0);
        CHECK(bit(2) == 1);
        CHECK(bit(3) == 0);
        CHECK(bit(4) == 1);

        Series e = eq(col, 9);  // only index 2
        const std::uint8_t* eb = e.data<std::uint8_t>();
        CHECK(((eb[0] >> 2) & 1) == 1);
        CHECK(((eb[0] >> 0) & 1) == 0);
    }

    TEST_CASE("comparison is exact across integer domains") {
        // Values above 2^63 that a double threshold could not distinguish.
        std::vector<std::uint64_t> u{5, 100, 9000000000000000000ULL};
        Series uc = Series::flat(TypeId::Uint64, u.data(), 3);
        Series m = gt(uc, 8000000000000000000ULL);  // only the last
        REQUIRE(m.valid());
        const std::uint8_t* mb = m.data<std::uint8_t>();
        CHECK(((mb[0] >> 0) & 1) == 0);
        CHECK(((mb[0] >> 1) & 1) == 0);
        CHECK(((mb[0] >> 2) & 1) == 1);
    }

    TEST_CASE(
        "SIMD comparison matches scalar for every op over a large column") {
        // 200 = 3 full 64-lane blocks + an 8-element tail, so both the block
        // packing and the scalar tail run.
        const std::int64_t n = 200;
        std::vector<std::int64_t> vi(n);
        std::vector<double> vd(n);
        for (std::int64_t i = 0; i < n; ++i) {
            vi[i] = (i * 7) % 23 - 11;  // spans negatives and the threshold
            vd[i] = static_cast<double>(vi[i]) + 0.5;
        }
        Series ci = Series::flat_i64(vi.data(), n);
        Series cd = Series::flat(TypeId::Float64, vd.data(), n);
        const std::int64_t thr = 3;

        auto bit = [](const Series& c, std::int64_t i) {
            return (c.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1;
        };
        Series mi[6] = {gt(ci, thr), ge(ci, thr), lt(ci, thr),
                        le(ci, thr), eq(ci, thr), ne(ci, thr)};
        Series md[6] = {gt(cd, 3.5), ge(cd, 3.5), lt(cd, 3.5),
                        le(cd, 3.5), eq(cd, 3.5), ne(cd, 3.5)};
        for (std::int64_t i = 0; i < n; ++i) {
            const std::int64_t x = vi[i];
            CHECK(bit(mi[0], i) == (x > thr));
            CHECK(bit(mi[1], i) == (x >= thr));
            CHECK(bit(mi[2], i) == (x < thr));
            CHECK(bit(mi[3], i) == (x <= thr));
            CHECK(bit(mi[4], i) == (x == thr));
            CHECK(bit(mi[5], i) == (x != thr));
            const double y = vd[i];
            CHECK(bit(md[0], i) == (y > 3.5));
            CHECK(bit(md[2], i) == (y < 3.5));
            CHECK(bit(md[4], i) == (y == 3.5));
        }
    }

    TEST_CASE("scalar-broadcast arithmetic (type-preserving)") {
        std::vector<std::int64_t> v{10, 20, 30};
        Series col = Series::flat_i64(v.data(), 3);

        Series a = add_scalar(col, 5);  // 15,25,35
        REQUIRE(a.valid());
        CHECK(a.data<std::int64_t>()[0] == 15);
        CHECK(a.data<std::int64_t>()[2] == 35);

        Series m = mul_scalar(col, 3);  // 30,60,90
        REQUIRE(m.valid());
        CHECK(m.data<std::int64_t>()[1] == 60);

        std::vector<double> f{1.5, 2.5};
        Series fc = mul_scalar(Series::flat_f64(f.data(), 2), 2.0);
        CHECK(fc.data<double>()[0] == doctest::Approx(3.0));

        // Precision: subtract a base beyond 2^53 that a double scalar could not
        // represent exactly (epoch-nanosecond timestamps).
        std::int64_t base = 1700000000000000000LL;
        std::vector<std::int64_t> ts{base + 10, base + 20};
        Series shifted = sub_scalar(Series::flat_i64(ts.data(), 2), base);
        REQUIRE(shifted.valid());
        CHECK(shifted.data<std::int64_t>()[0] == 10);
        CHECK(shifted.data<std::int64_t>()[1] == 20);
    }

    TEST_CASE("reductions return an exact tagged scalar") {
        // int64 sum stays exact even past 2^53.
        std::int64_t base = 1700000000000000000LL;
        std::vector<std::int64_t> v{base, 10, 20, 30};
        Series col = Series::flat_i64(v.data(), 4);
        CHECK(scalar_value<std::int64_t>(sum(col)) == base + 60);
        CHECK(scalar_value<std::int64_t>(min(col)) == 10);
        CHECK(scalar_value<std::int64_t>(max(col)) == base);
        CHECK(count(col) == 4);

        std::vector<double> f{1.5, 2.5, 4.0};
        Series fc = Series::flat_f64(f.data(), 3);
        CHECK(scalar_value<double>(sum(fc)) == doctest::Approx(8.0));
        CHECK(scalar_value<double>(max(fc)) == doctest::Approx(4.0));

        // Nulls are skipped by sum/count.
        std::vector<std::int64_t> w{9, 9, 9, 9};
        std::uint8_t bitmap = 0b0000'0101;  // rows 0, 2 valid
        Series wc = Series::flat_i64(w.data(), 4, &bitmap);
        CHECK(scalar_value<std::int64_t>(sum(wc)) == 18);
        CHECK(count(wc) == 2);
        CHECK(mean(wc) == doctest::Approx(9.0));  // 18 / 2 valid
    }

    TEST_CASE("SIMD reduce matches scalar over a large column") {
        // 300 is not a multiple of the lane count, so the vectorized body and
        // the scalar tail both run.
        const std::int64_t n = 300;
        std::vector<double> fd(n);
        std::vector<std::int64_t> vi(n);
        double ref_sum = 0.0, ref_min = 1e18, ref_max = -1e18;
        std::int64_t imin = INT64_MAX, imax = INT64_MIN, isum = 0;
        for (std::int64_t i = 0; i < n; ++i) {
            fd[i] = static_cast<double>((i * 37) % 101) - 50.0 + 0.25;
            vi[i] = (i * 91) % 200 - 100;
            ref_sum += fd[i];
            ref_min = fd[i] < ref_min ? fd[i] : ref_min;
            ref_max = fd[i] > ref_max ? fd[i] : ref_max;
            imin = vi[i] < imin ? vi[i] : imin;
            imax = vi[i] > imax ? vi[i] : imax;
            isum += vi[i];
        }
        Series fc = Series::flat_f64(fd.data(), n);
        CHECK(scalar_value<double>(sum(fc)) == doctest::Approx(ref_sum));
        CHECK(scalar_value<double>(min(fc)) == doctest::Approx(ref_min));
        CHECK(scalar_value<double>(max(fc)) == doctest::Approx(ref_max));

        Series ic = Series::flat_i64(vi.data(), n);
        CHECK(scalar_value<std::int64_t>(sum(ic)) == isum);  // i64 sum
        CHECK(scalar_value<std::int64_t>(min(ic)) == imin);
        CHECK(scalar_value<std::int64_t>(max(ic)) == imax);

        // Null-present sum, every 4th row null: exact skip-nulls result.
        std::vector<std::uint8_t> bm(static_cast<std::size_t>((n + 7) / 8), 0);
        std::int64_t isum_valid = 0;
        double fsum_valid = 0.0;
        for (std::int64_t i = 0; i < n; ++i) {
            if (i % 4 != 0) {
                bm[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
                isum_valid += vi[i];
                fsum_valid += fd[i];
            }
        }
        Series icn = Series::flat_i64(vi.data(), n, bm.data());
        Series fcn = Series::flat_f64(fd.data(), n, bm.data());
        CHECK(scalar_value<std::int64_t>(sum(icn)) == isum_valid);
        CHECK(scalar_value<double>(sum(fcn)) == doctest::Approx(fsum_valid));
    }

    TEST_CASE("group_by aggregates values by a string key") {
        Series names =
            Series::strings({"read", "write", "read", "read", "write"});
        std::vector<std::int64_t> durs{10, 5, 20, 30, 15};
        Series dur = Series::flat_i64(durs.data(), 5);

        DataFrame g = group_by(names, dur, DFTU_REDUCE_SUM);
        REQUIRE(g.num_columns() == 2);
        REQUIRE(g.columns[1].valid());
        REQUIRE(g.num_rows() == 2);  // read, write in first-seen order
        CHECK(g.columns[0].string_at(0) == "read");
        CHECK(g.columns[0].string_at(1) == "write");
        CHECK(g.columns[1].type() == TypeId::Int64);
        CHECK(g.columns[1].data<std::int64_t>()[0] == 60);  // 10+20+30
        CHECK(g.columns[1].data<std::int64_t>()[1] == 20);  // 5+15

        DataFrame c = group_by(names, dur, DFTU_REDUCE_COUNT);
        CHECK(c.columns[1].data<std::int64_t>()[0] == 3);

        DataFrame mn = group_by(names, dur, DFTU_REDUCE_MEAN);
        CHECK(mn.columns[1].type() == TypeId::Float64);
        CHECK(mn.columns[1].data<double>()[0] ==
              doctest::Approx(20.0));  // 60/3

        // A dictionary-encoded key gives the same grouping.
        DataFrame gd = group_by(dictionary_encode(names), dur, DFTU_REDUCE_SUM);
        CHECK(gd.columns[1].data<std::int64_t>()[0] == 60);

        // Several aggregates in one pass; columns named in flag order.
        DataFrame multi = group_by(
            names, dur, DFTU_REDUCE_SUM | DFTU_REDUCE_COUNT | DFTU_REDUCE_MEAN);
        REQUIRE(multi.num_columns() == 4);  // key + sum + count + mean
        CHECK(multi.names[0] == "key");
        CHECK(multi.names[1] == "sum");
        CHECK(multi.names[2] == "count");
        CHECK(multi.names[3] == "mean");
        CHECK(multi.columns[1].data<std::int64_t>()[0] == 60);  // sum read
        CHECK(multi.columns[2].data<std::int64_t>()[0] == 3);   // count read
        CHECK(multi.columns[3].data<double>()[0] == doctest::Approx(20.0));
    }

    TEST_CASE("group_by on a numeric key") {
        std::vector<std::int32_t> pid{1, 2, 1, 2, 1};
        std::vector<std::int64_t> val{10, 20, 30, 40, 50};
        DataFrame g =
            group_by(Series::flat(TypeId::Int32, pid.data(), 5),
                     Series::flat_i64(val.data(), 5), DFTU_REDUCE_SUM);
        REQUIRE(g.num_rows() == 2);
        CHECK(g.columns[0].type() == TypeId::Int32);
        CHECK(g.columns[0].data<std::int32_t>()[0] == 1);
        CHECK(g.columns[0].data<std::int32_t>()[1] == 2);
        CHECK(g.columns[1].data<std::int64_t>()[0] == 90);  // 10+30+50
        CHECK(g.columns[1].data<std::int64_t>()[1] == 60);  // 20+40
    }

    TEST_CASE("cast between numeric types") {
        std::vector<std::int64_t> v{1, 256, -5};
        Series c = cast(Series::flat_i64(v.data(), 3), TypeId::Int32);
        REQUIRE(c.valid());
        CHECK(c.type() == TypeId::Int32);
        CHECK(c.data<std::int32_t>()[0] == 1);
        CHECK(c.data<std::int32_t>()[1] == 256);
        CHECK(c.data<std::int32_t>()[2] == -5);

        Series f = cast(Series::flat_i64(v.data(), 3), TypeId::Float64);
        REQUIRE(f.valid());
        CHECK(f.data<double>()[1] == doctest::Approx(256.0));
    }

    TEST_CASE("logical and/not on bool columns") {
        std::vector<std::int64_t> v{5, 1, 9, 3, 7};
        Series col = Series::flat_i64(v.data(), 5);
        Series agt = gt(col, 4);              // 1,0,1,0,1
        Series alt = lt(col, 8);              // 1,1,0,1,1

        Series both = logical_and(agt, alt);  // 1,0,0,0,1
        REQUIRE(both.valid());
        const std::uint8_t* bb = both.data<std::uint8_t>();
        auto bbit = [&](int i) { return (bb[i >> 3] >> (i & 7)) & 1; };
        CHECK(bbit(0) == 1);
        CHECK(bbit(1) == 0);
        CHECK(bbit(2) == 0);
        CHECK(bbit(4) == 1);

        Series n = logical_not(agt);  // 0,1,0,1,0
        const std::uint8_t* nb = n.data<std::uint8_t>();
        auto nbit = [&](int i) { return (nb[i >> 3] >> (i & 7)) & 1; };
        CHECK(nbit(0) == 0);
        CHECK(nbit(1) == 1);
        CHECK(nbit(4) == 0);
    }

    TEST_CASE("string column build and read") {
        Series s = Series::strings({"alpha", "", "gamma"});
        REQUIRE(s.valid());
        CHECK(s.type() == TypeId::String);
        REQUIRE(s.length() == 3);
        CHECK(s.string_at(0) == "alpha");
        CHECK(s.string_at(1) == "");
        CHECK(s.string_at(2) == "gamma");
    }

    TEST_CASE("str_eq on flat and dictionary string columns") {
        Series s = Series::strings({"read", "write", "read", "open"});
        Series m = str_eq(s, "read");  // 1,0,1,0
        REQUIRE(m.valid());
        CHECK(m.type() == TypeId::Bool);
        const std::uint8_t* mb = m.data<std::uint8_t>();
        auto bit = [&](int i) { return (mb[i >> 3] >> (i & 7)) & 1; };
        CHECK(bit(0) == 1);
        CHECK(bit(1) == 0);
        CHECK(bit(2) == 1);
        CHECK(bit(3) == 0);

        // Dictionary path compares codes; same logical result.
        Series dm = str_eq(dictionary_encode(s), "read");
        REQUIRE(dm.valid());
        const std::uint8_t* db = dm.data<std::uint8_t>();
        auto dbit = [&](int i) { return (db[i >> 3] >> (i & 7)) & 1; };
        CHECK(dbit(0) == 1);
        CHECK(dbit(2) == 1);
        CHECK(dbit(1) == 0);

        // A value absent from the dictionary matches nothing.
        Series none = str_eq(dictionary_encode(s), "missing");
        REQUIRE(none.valid());
        CHECK((none.data<std::uint8_t>()[0] & 0x0f) == 0);
    }

    TEST_CASE("str_contains and str_starts_with (with dict pushdown)") {
        Series p = Series::strings({"/data/a.h5", "/tmp/b", "/data/c.h5"});
        Series has = str_contains(p, "/data/");  // 1,0,1
        REQUIRE(has.valid());
        const std::uint8_t* hb = has.data<std::uint8_t>();
        CHECK((hb[0] & 0b111) == 0b101);

        Series pref = str_starts_with(dictionary_encode(p), "/data/");  // 1,0,1
        REQUIRE(pref.valid());
        const std::uint8_t* pb = pref.data<std::uint8_t>();
        CHECK((pb[0] & 0b111) == 0b101);
    }

    TEST_CASE("SIMD substring search matches the scalar find (contains/find)") {
        // Rows long enough to cross a vector block, plus a null, plus needles
        // at varied offsets (start, middle, end, absent, empty).
        Series s = Series::strings(
            {"the quick brown fox jumps over the lazy dog",  // 0
             "brownies for everyone in the room today okay", "no match here",
             "xxbrownxx", "", "brown"});
        auto bit = [](const Series& m, std::int64_t i) {
            return (m.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1;
        };
        Series has = s.str_contains("brown");
        REQUIRE(has.valid());
        CHECK(bit(has, 0) == 1);
        CHECK(bit(has, 1) == 1);
        CHECK(bit(has, 2) == 0);
        CHECK(bit(has, 3) == 1);
        CHECK(bit(has, 4) == 0);
        CHECK(bit(has, 5) == 1);
        // Empty needle contains everywhere (std::string_view::find semantics).
        Series empty = s.str_contains("");
        for (std::int64_t i = 0; i < s.length(); ++i) CHECK(bit(empty, i) == 1);

        Series find = s.str_find("brown");
        const std::int64_t* f = find.data<std::int64_t>();
        CHECK(f[0] == 10);
        CHECK(f[1] == 0);
        CHECK(f[2] == -1);
        CHECK(f[3] == 2);
        CHECK(f[4] == -1);
        CHECK(f[5] == 0);
        // Empty needle: first index 0 everywhere (matches scalar s.find("")).
        Series find_empty = s.str_find("");
        const std::int64_t* fe = find_empty.data<std::int64_t>();
        for (std::int64_t i = 0; i < s.length(); ++i) CHECK(fe[i] == 0);
    }

    TEST_CASE("str_like affix routes, general glob, and escapes") {
        auto bit = [](const Series& m, std::int64_t i) {
            return (m.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1;
        };
        Series s = Series::strings({"apple", "apricot", "banana", "grape",
                                    "pineapple", "50%_off", "a"});

        // exact (no wildcards, with an escaped literal)
        Series ex = s.str_like("apple");
        CHECK(bit(ex, 0) == 1);
        CHECK(bit(ex, 4) == 0);

        // "text%" -> starts_with
        Series pre = s.str_like("ap%");
        CHECK(bit(pre, 0) == 1);  // apple
        CHECK(bit(pre, 1) == 1);  // apricot
        CHECK(bit(pre, 2) == 0);  // banana
        CHECK(bit(pre, 6) == 0);  // "a" has no "ap" prefix

        // "%text" -> ends_with
        Series suf = s.str_like("%apple");
        CHECK(bit(suf, 0) == 1);  // apple
        CHECK(bit(suf, 4) == 1);  // pineapple
        CHECK(bit(suf, 2) == 0);

        // "%text%" -> contains (SIMD substring)
        Series mid = s.str_like("%an%");
        CHECK(bit(mid, 2) == 1);  // banana
        CHECK(bit(mid, 0) == 0);

        // "_" matches exactly one char
        Series one = s.str_like("a_ple");
        CHECK(bit(one, 0) == 1);  // apple
        CHECK(bit(one, 6) == 0);  // "a" too short

        // general glob: leading %, interior _, literal
        Series gen = s.str_like("%p_le");
        CHECK(bit(gen, 0) == 1);  // apple: ...p p l e -> %p_le
        CHECK(bit(gen, 4) == 1);  // pineapple ends ...pple
        CHECK(bit(gen, 2) == 0);  // banana

        // escaped literal '%' and '_': matches the literal "50%_off"
        Series esc = s.str_like("50\\%\\_off");
        CHECK(bit(esc, 5) == 1);
        CHECK(bit(esc, 0) == 0);

        // a bare "%" matches everything (including empty via the run)
        Series all = s.str_like("%");
        for (std::int64_t i = 0; i < s.length(); ++i) CHECK(bit(all, i) == 1);

        // null rows stay null in the mask
        Series withnull = Series::strings({"apple", "x"}).str_like("apple");
        REQUIRE(withnull.valid());
        CHECK(bit(withnull, 0) == 1);
        CHECK(bit(withnull, 1) == 0);
    }

    TEST_CASE("string column ops") {
        auto mbit = [](const Series& m, std::int64_t i) {
            return (m.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1;
        };

        Series s = Series::strings({"Read", "reader", "WRITE", "op"});

        // predicates
        Series ends =
            s.str_ends_with("e");  // Read? no, reader no, WRITE no, op no
        REQUIRE(ends.valid());
        CHECK(mbit(ends, 0) == 0);
        Series ends2 =
            Series::strings({"cat.h5", "dog.txt", "x.h5"}).str_ends_with(".h5");
        CHECK(mbit(ends2, 0) == 1);
        CHECK(mbit(ends2, 1) == 0);
        CHECK(mbit(ends2, 2) == 1);

        Series rgx = Series::strings({"abc123", "abc", "abc123x"})
                         .str_matches("[a-z]+[0-9]+");
        REQUIRE(rgx.valid());
        CHECK(mbit(rgx, 0) == 1);  // full match
        CHECK(mbit(rgx, 1) == 0);  // no digits
        CHECK(mbit(rgx, 2) == 0);  // trailing letter breaks the full match

        // numeric: len_bytes exercises the SIMD path with > lane-count rows
        Series lens =
            Series::strings({"a", "bb", "ccc", "", "de", "fghi", "j", "kk"})
                .str_len_bytes();
        REQUIRE(lens.valid());
        CHECK(lens.type() == TypeId::Int64);
        const std::int64_t* lp = lens.data<std::int64_t>();
        CHECK(lp[0] == 1);
        CHECK(lp[1] == 2);
        CHECK(lp[2] == 3);
        CHECK(lp[3] == 0);
        CHECK(lp[5] == 4);
        CHECK(lp[7] == 2);

        Series chars = Series::strings({"abc", "\xC3\xA9\x63"})
                           .str_len_chars();        // "abc","ec"
        CHECK(chars.data<std::int64_t>()[0] == 3);
        CHECK(chars.data<std::int64_t>()[1] == 2);  // 3 bytes, 2 codepoints

        // Long mixed ASCII + multibyte string exercises the vectorized counter
        // body: 40 ASCII 'a' + 10 x 2-byte 'e-acute' = 80 bytes, 50 codepoints.
        std::string longs(40, 'a');
        for (int k = 0; k < 10; ++k) longs += "\xC3\xA9";
        Series lc = Series::strings({longs}).str_len_chars();
        CHECK(lc.data<std::int64_t>()[0] == 50);

        Series find = Series::strings({"hello/world", "nope"}).str_find("/");
        CHECK(find.data<std::int64_t>()[0] == 5);
        CHECK(find.data<std::int64_t>()[1] == -1);

        Series hashed = Series::strings({"POSIX", "read"}).fnv1a();
        CHECK(hashed.type() == TypeId::Uint64);
        CHECK(hashed.data<std::uint64_t>()[0] ==
              dftracer::utils::hash::fnv1a_hash(std::string_view{"POSIX"}));
        CHECK(hashed.data<std::uint64_t>()[1] ==
              dftracer::utils::hash::fnv1a_hash(std::string_view{"read"}));

        Series parsed = Series::strings({"00000000000000ff", "00000000deadbeef",
                                         "deadbeef"})
                            .hex64_parse();
        CHECK(parsed.type() == TypeId::Uint64);
        CHECK(parsed.data<std::uint64_t>()[0] == 0xffull);
        CHECK(parsed.data<std::uint64_t>()[1] == 0xdeadbeefull);
        CHECK(parsed.is_null(2));  // not the 16-digit form

        Series formatted = parsed.hex64_format();
        CHECK(formatted.type() == TypeId::String);
        CHECK(formatted.string_at(0) == "00000000000000ff");
        CHECK(formatted.string_at(1) == "00000000deadbeef");
        CHECK(formatted.is_null(2));
        CHECK(formatted.hex64_parse().data<std::uint64_t>()[1] ==
              0xdeadbeefull);
        CHECK(Series::strings({"f07c4ebf132e3799"})
                  .hex64_parse()
                  .hex64_format()
                  .string_at(0) == "f07c4ebf132e3799");

        // transforms
        Series lo = s.to_lowercase();
        CHECK(lo.string_at(0) == "read");
        CHECK(lo.string_at(2) == "write");
        Series up = s.to_uppercase();
        CHECK(up.string_at(1) == "READER");

        Series trimmed =
            Series::strings({"  hi  ", "\tx\n", "none"}).str_strip();
        CHECK(trimmed.string_at(0) == "hi");
        CHECK(trimmed.string_at(1) == "x");
        Series ltr = Series::strings({"  hi  "}).str_lstrip();
        CHECK(ltr.string_at(0) == "hi  ");
        Series rtr = Series::strings({"  hi  "}).str_rstrip();
        CHECK(rtr.string_at(0) == "  hi");

        Series rep = Series::strings({"a.b.c"}).str_replace(".", "-");
        CHECK(rep.string_at(0) == "a-b.c");
        Series repa = Series::strings({"a.b.c"}).str_replace_all(".", "-");
        CHECK(repa.string_at(0) == "a-b-c");

        Series sl = Series::strings({"hello"}).str_slice(1, 3);
        CHECK(sl.string_at(0) == "ell");
        Series sln =
            Series::strings({"hello"}).str_slice(-2, -1);  // from end to end
        CHECK(sln.string_at(0) == "lo");

        Series ps = Series::strings({"7", "abcd"}).str_pad_start(3, '*');
        CHECK(ps.string_at(0) == "**7");
        CHECK(ps.string_at(1) == "abcd");  // already >= width
        Series pe = Series::strings({"7"}).str_pad_end(3, '*');
        CHECK(pe.string_at(0) == "7**");
        Series zf = Series::strings({"42", "-5"}).str_zfill(4);
        CHECK(zf.string_at(0) == "0042");
        CHECK(zf.string_at(1) == "-005");  // sign preserved

        // list: split
        Series sp = Series::strings({"a/b/c", "x"}).str_split("/");
        REQUIRE(sp.valid());
        CHECK(sp.type() == TypeId::List);
        const std::int32_t* lo2 = sp.offsets();
        CHECK(lo2[0] == 0);
        CHECK(lo2[1] == 3);  // row 0: 3 parts
        CHECK(lo2[2] == 4);  // row 1: 1 part
        Series vals = sp.child(0);
        CHECK(vals.string_at(0) == "a");
        CHECK(vals.string_at(1) == "b");
        CHECK(vals.string_at(2) == "c");
        CHECK(vals.string_at(3) == "x");

        // list: len and get (negative index from the end, short row null)
        Series ln = sp.list_len();
        REQUIRE(ln.valid());
        CHECK(ln.type() == TypeId::Int64);
        CHECK(ln.data<std::int64_t>()[0] == 3);
        CHECK(ln.data<std::int64_t>()[1] == 1);
        Series g1 = sp.list_get(1);
        CHECK(g1.string_at(0) == "b");
        CHECK(g1.is_null(1));
        Series gl = sp.list_get(-1);
        CHECK(gl.string_at(0) == "c");
        CHECK(gl.string_at(1) == "x");

        // regex capture group, null where there is no match
        Series ex =
            Series::strings({"a-b", "no", "c-d"}).str_extract("(\\w)-(\\w)", 2);
        REQUIRE(ex.valid());
        CHECK(ex.string_at(0) == "b");
        CHECK(ex.is_null(1));
        CHECK(ex.string_at(2) == "d");
        CHECK(ex.null_count() == 1);
        Series ex0 = Series::strings({"a-b"}).str_extract("(\\w)-(\\w)", 0);
        CHECK(ex0.string_at(0) == "a-b");
    }

    TEST_CASE("nested list<struct> column (histogram shape)") {
        std::vector<double> lo{0.0, 1.0, 5.0};
        std::vector<double> hi{1.0, 2.0, 6.0};
        std::vector<std::int64_t> cnt{10, 20, 30};
        std::vector<Series> fields;
        fields.push_back(Series::flat_f64(lo.data(), 3));
        fields.push_back(Series::flat_f64(hi.data(), 3));
        fields.push_back(Series::flat_i64(cnt.data(), 3));
        Series st = Series::structs({"lo", "hi", "count"}, std::move(fields));
        REQUIRE(st.valid());
        CHECK(st.type() == TypeId::Struct);
        REQUIRE(st.length() == 3);

        std::vector<std::int32_t> offsets{0, 2,
                                          3};  // row0: bins 0-1, row1: bin 2
        Series hist = Series::list(offsets, std::move(st));
        REQUIRE(hist.valid());
        CHECK(hist.type() == TypeId::List);
        REQUIRE(hist.length() == 2);  // two rows (groups)
    }

    TEST_CASE("take gathers rows of any column type") {
        // Flat reorder with repeats.
        std::vector<std::int64_t> v{10, 20, 30};
        Series g = take(Series::flat_i64(v.data(), 3), {2, 0, 0});
        REQUIRE(g.valid());
        REQUIRE(g.length() == 3);
        CHECK(g.data<std::int64_t>()[0] == 30);
        CHECK(g.data<std::int64_t>()[1] == 10);
        CHECK(g.data<std::int64_t>()[2] == 10);

        // Validity is gathered: pick a valid then a null row.
        std::vector<std::int64_t> w{1, 2, 3, 4};
        std::uint8_t bitmap = 0b0000'0101;  // rows 0, 2 valid
        Series gn = take(Series::flat_i64(w.data(), 4, &bitmap), {2, 1});
        REQUIRE(gn.valid());
        REQUIRE(gn.length() == 2);
        CHECK(gn.data<std::int64_t>()[0] == 3);
        CHECK(gn.null_count() == 1);  // gathered row 1 was invalid

        // Strings.
        Series gs = take(Series::strings({"a", "b", "c"}), {1, 1, 0});
        REQUIRE(gs.valid());
        CHECK(gs.string_at(0) == "b");
        CHECK(gs.string_at(2) == "a");
    }

    TEST_CASE("agg engine: fused multi-agg + mergeable partials") {
        namespace ag = dftracer::utils::dataframe;
        Series key = Series::strings({"io", "cpu", "io", "cpu", "io"});
        std::vector<std::int64_t> durv{10, 5, 20, 15, 30};
        Series dur = Series::flat_i64(durv.data(), 5);
        std::vector<const Series*> vals{&dur};
        std::vector<ag::AggSpec> specs{{ag::AggOp::Count, -1, "n"},
                                       {ag::AggOp::Sum, 0, "sum_dur"},
                                       {ag::AggOp::Mean, 0, "mean_dur"},
                                       {ag::AggOp::Max, 0, "max_dur"}};

        // One-shot fused group-by: all four aggregates in a single pass.
        DataFrame one = ag::group_agg(key, vals, specs, "cat");
        REQUIRE(one.num_rows() == 2);
        REQUIRE(one.names.size() == 5);
        REQUIRE(one.names == std::vector<std::string>{"cat", "n", "sum_dur",
                                                      "mean_dur", "max_dur"});
        // first-seen order: io then cpu; columns are key + specs in order.
        CHECK(one.columns[0].string_at(0) == "io");
        CHECK(one.columns[1].data<std::int64_t>()[0] == 3);   // io count
        CHECK(one.columns[2].data<std::int64_t>()[0] == 60);  // io sum 10+20+30
        CHECK(one.columns[3].data<double>()[0] == doctest::Approx(20.0));
        CHECK(one.columns[4].data<std::int64_t>()[0] == 30);  // io max
        CHECK(one.columns[2].data<std::int64_t>()[1] == 20);  // cpu 5+15

        // Mergeable partials: split the rows, accumulate two states, merge -
        // must equal the one-shot result (this is the spill/distributed path).
        Series k1 = Series::strings({"io", "cpu"});
        std::vector<std::int64_t> d1{10, 5};
        Series c1 = Series::flat_i64(d1.data(), 2);
        Series k2 = Series::strings({"io", "cpu", "io"});
        std::vector<std::int64_t> d2{20, 15, 30};
        Series c2 = Series::flat_i64(d2.data(), 3);

        auto s1 = ag::agg_new(specs);
        auto s2 = ag::agg_new(specs);
        std::vector<const Series*> v1{&c1}, v2{&c2};
        ag::agg_accumulate(*s1, k1, v1);
        ag::agg_accumulate(*s2, k2, v2);
        ag::agg_merge(*s1, *s2);
        DataFrame merged = ag::agg_finalize(*s1, "cat");
        REQUIRE(merged.num_rows() == 2);
        CHECK(merged.columns[1].data<std::int64_t>()[0] == 3);
        CHECK(merged.columns[2].data<std::int64_t>()[0] == 60);
        CHECK(merged.columns[3].data<double>()[0] == doctest::Approx(20.0));
        CHECK(merged.columns[4].data<std::int64_t>()[0] == 30);

        // Serialize the merged partial, ship it (round-trip), finalize - must
        // match (the distributed-partial path).
        std::string blob = ag::agg_serialize(*s1);
        auto s3 = ag::agg_deserialize(blob);
        DataFrame rt = ag::agg_finalize(*s3, "cat");
        REQUIRE(rt.num_rows() == 2);
        CHECK(rt.columns[0].string_at(0) == "io");
        CHECK(rt.columns[1].data<std::int64_t>()[0] == 3);
        CHECK(rt.columns[2].data<std::int64_t>()[0] == 60);
        CHECK(rt.columns[3].data<double>()[0] == doctest::Approx(20.0));
    }

    TEST_CASE("agg engine: native multi-key group_by (composite key, typed)") {
        namespace ag = dftracer::utils::dataframe;
        // Two key columns of different types: String "cat" and Int64 "pid".
        // (cat, pid) = (io, 1), (cpu, 1), (io, 2), (cpu, 1), (io, 1)
        Series cat = Series::strings({"io", "cpu", "io", "cpu", "io"});
        std::vector<std::int64_t> pidv{1, 1, 2, 1, 1};
        Series pid = Series::flat_i64(pidv.data(), 5);
        std::vector<std::int64_t> durv{10, 5, 20, 15, 30};
        Series dur = Series::flat_i64(durv.data(), 5);
        std::vector<const Series*> keys{&cat, &pid};
        std::vector<const Series*> vals{&dur};
        std::vector<ag::AggSpec> specs{{ag::AggOp::Count, -1, "n"},
                                       {ag::AggOp::Sum, 0, "sum_dur"}};

        DataFrame g = ag::group_agg(keys, vals, specs, {"cat", "pid"});
        REQUIRE(g.num_rows() == 3);  // (io,1) (cpu,1) (io,2) - first-seen order
        REQUIRE(g.names ==
                std::vector<std::string>{"cat", "pid", "n", "sum_dur"});
        CHECK(g.columns[0].type() == TypeId::String);
        CHECK(g.columns[1].type() == TypeId::Int64);

        auto find = [&](const std::string& c, std::int64_t p) -> std::int64_t {
            for (std::int64_t i = 0; i < g.num_rows(); ++i)
                if (g.columns[0].string_at(i) == c &&
                    g.columns[1].data<std::int64_t>()[i] == p)
                    return i;
            FAIL("group not found");
            return -1;
        };
        const std::int64_t io1 = find("io", 1), cpu1 = find("cpu", 1),
                           io2 = find("io", 2);
        CHECK(g.columns[2].data<std::int64_t>()[io1] == 2);    // rows 0,4
        CHECK(g.columns[3].data<std::int64_t>()[io1] == 40);   // 10+30
        CHECK(g.columns[2].data<std::int64_t>()[cpu1] == 2);
        CHECK(g.columns[3].data<std::int64_t>()[cpu1] == 20);  // 5+15
        CHECK(g.columns[2].data<std::int64_t>()[io2] == 1);
        CHECK(g.columns[3].data<std::int64_t>()[io2] == 20);

        // Mergeable partials + serialize round-trip, split across the same
        // composite key.
        Series ck1 = Series::strings({"io", "cpu"});
        std::vector<std::int64_t> pk1{1, 1};
        Series pid1 = Series::flat_i64(pk1.data(), 2);
        std::vector<std::int64_t> d1{10, 5};
        Series c1 = Series::flat_i64(d1.data(), 2);
        Series ck2 = Series::strings({"io", "cpu", "io"});
        std::vector<std::int64_t> pk2{2, 1, 1};
        Series pid2 = Series::flat_i64(pk2.data(), 3);
        std::vector<std::int64_t> d2{20, 15, 30};
        Series c2 = Series::flat_i64(d2.data(), 3);

        auto s1 = ag::agg_new(specs);
        auto s2 = ag::agg_new(specs);
        std::vector<const Series*> keys1{&ck1, &pid1}, keys2{&ck2, &pid2};
        std::vector<const Series*> v1{&c1}, v2{&c2};
        ag::agg_accumulate(*s1, keys1, v1);
        ag::agg_accumulate(*s2, keys2, v2);
        std::string blob = ag::agg_serialize(*s2);
        auto s2b = ag::agg_deserialize(blob);
        ag::agg_merge(*s1, *s2b);
        DataFrame merged =
            ag::agg_finalize(*s1, std::vector<std::string>{"cat", "pid"});
        REQUIRE(merged.num_rows() == 3);
        const std::int64_t mio1 = [&] {
            for (std::int64_t i = 0; i < merged.num_rows(); ++i)
                if (merged.columns[0].string_at(i) == "io" &&
                    merged.columns[1].data<std::int64_t>()[i] == 1)
                    return i;
            return std::int64_t(-1);
        }();
        REQUIRE(mio1 >= 0);
        CHECK(merged.columns[2].data<std::int64_t>()[mio1] == 2);
        CHECK(merged.columns[3].data<std::int64_t>()[mio1] == 40);
    }

    TEST_CASE("agg engine: moment aggregates match the stats kernels") {
        namespace ag = dftracer::utils::dataframe;
        // Two groups; enough spread per group for skew/kurt to be meaningful.
        Series key = Series::strings({"a", "b", "a", "b", "a", "b", "a", "b"});
        std::vector<double> xv{2, 100, 4, 50, 8, 25, 16, 200};
        Series x = Series::flat_f64(xv.data(), 8);
        std::vector<const Series*> vals{&x};
        std::vector<ag::AggSpec> specs{{ag::AggOp::Var, 0, "var"},
                                       {ag::AggOp::Std, 0, "std"},
                                       {ag::AggOp::Skew, 0, "skew"},
                                       {ag::AggOp::Kurt, 0, "kurt"}};
        DataFrame g = ag::group_agg(key, vals, specs, "k");
        REQUIRE(g.num_rows() == 2);
        REQUIRE(g.names ==
                std::vector<std::string>{"k", "var", "std", "skew", "kurt"});
        // Reference: the same group's values through the kernels directly.
        std::vector<double> ga{2, 4, 8, 16};  // group "a", first-seen row 0
        Series ca = Series::flat_f64(ga.data(), 4);
        CHECK(g.columns[1].data<double>()[0] ==
              doctest::Approx(dv::variance(ca, true)));
        CHECK(g.columns[2].data<double>()[0] ==
              doctest::Approx(dv::stddev(ca, true)));
        CHECK(g.columns[3].data<double>()[0] ==
              doctest::Approx(dv::skewness(ca)));
        CHECK(g.columns[4].data<double>()[0] ==
              doctest::Approx(dv::kurtosis(ca)));

        // Mergeable across a split: partials must equal the one-shot moments.
        Series k1 = Series::strings({"a", "b", "a", "b"});
        std::vector<double> d1{2, 100, 4, 50};
        Series c1 = Series::flat_f64(d1.data(), 4);
        Series k2 = Series::strings({"a", "b", "a", "b"});
        std::vector<double> d2{8, 25, 16, 200};
        Series c2 = Series::flat_f64(d2.data(), 4);
        auto s1 = ag::agg_new(specs);
        auto s2 = ag::agg_new(specs);
        std::vector<const Series*> v1{&c1}, v2{&c2};
        ag::agg_accumulate(*s1, k1, v1);
        ag::agg_accumulate(*s2, k2, v2);
        ag::agg_merge(*s1, *s2);
        std::string blob = ag::agg_serialize(*s1);
        DataFrame m = ag::agg_finalize(*ag::agg_deserialize(blob), "k");
        CHECK(m.columns[1].data<double>()[0] ==
              doctest::Approx(g.columns[1].data<double>()[0]));
        CHECK(m.columns[4].data<double>()[0] ==
              doctest::Approx(g.columns[4].data<double>()[0]));
    }

    TEST_CASE("eval_many: CSE spans outputs, pruner skips unused inputs") {
        namespace ag = dftracer::utils::dataframe;
        std::vector<std::int64_t> a{1, 2, 3, 4};
        std::vector<std::int64_t> b{10, 20, 30, 40};
        std::vector<std::int64_t> unused{7, 7, 7, 7};
        Series ca = Series::flat_i64(a.data(), 4);
        Series cb = Series::flat_i64(b.data(), 4);
        Series cu = Series::flat_i64(unused.data(), 4);
        std::vector<const Series*> in{&ca, &cb,
                                      &cu};  // col 2 is never referenced

        ag::Expr sum_ab = ag::expr_binary(ag::BinaryOp::Add, ag::expr_col(0),
                                          ag::expr_col(1));
        ag::Expr twice = ag::expr_binary(ag::BinaryOp::Mul, sum_ab,
                                         ag::expr_lit(std::int64_t{2}));
        // Two outputs sharing `a+b`; the shared node is computed once (CSE).
        std::vector<ag::Series> outs = ag::eval_many({sum_ab, twice}, in);
        REQUIRE(outs.size() == 2);
        REQUIRE(outs[0].length() == 4);
        CHECK(outs[0].data<std::int64_t>()[0] == 11);
        CHECK(outs[0].data<std::int64_t>()[3] == 44);
        CHECK(outs[1].data<std::int64_t>()[0] == 22);
        CHECK(outs[1].data<std::int64_t>()[3] == 88);
    }

    TEST_CASE("group_agg_expr: aggregates over expressions with shared CSE") {
        namespace ag = dftracer::utils::dataframe;
        Series key = Series::strings({"x", "y", "x", "y", "x"});
        std::vector<std::int64_t> a{1, 2, 3, 4, 5};
        std::vector<std::int64_t> b{10, 20, 30, 40, 50};
        Series ca = Series::flat_i64(a.data(), 5);
        Series cb = Series::flat_i64(b.data(), 5);
        std::vector<const Series*> in{&key, &ca, &cb};

        ag::Expr keyx = ag::expr_col(0);
        ag::Expr ab = ag::expr_binary(ag::BinaryOp::Add, ag::expr_col(1),
                                      ag::expr_col(2));
        // Ergonomic builders; sum(a+b) and mean(a+b) share `ab` -> one
        // FieldStat via node-dedup + CSE.
        std::vector<ag::AggExprSpec> specs{ag::agg_count("n"),
                                           ag::agg_sum(ab).as("sum_ab"),
                                           ag::agg_mean(ab).as("mean_ab")};
        DataFrame out = ag::group_agg_expr(keyx, specs, in, "k");
        REQUIRE(out.num_rows() == 2);
        REQUIRE(out.names ==
                std::vector<std::string>{"k", "n", "sum_ab", "mean_ab"});
        // group "x": rows 0,2,4 -> (a+b) = 11,33,55; sum=99, mean=33, n=3.
        CHECK(out.columns[0].string_at(0) == "x");
        CHECK(out.columns[1].data<std::int64_t>()[0] == 3);
        CHECK(out.columns[2].data<std::int64_t>()[0] == 99);
        CHECK(out.columns[3].data<double>()[0] == doctest::Approx(33.0));
        // group "y": rows 1,3 -> 22,44; sum=66, mean=33, n=2.
        CHECK(out.columns[1].data<std::int64_t>()[1] == 2);
        CHECK(out.columns[2].data<std::int64_t>()[1] == 66);
    }

    TEST_CASE(
        "group_agg_expr C ABI: builders + dftu_dataframe_group_agg_expr") {
        Series key = Series::strings({"x", "y", "x"});
        std::vector<std::int64_t> a{1, 2, 3};
        std::vector<std::int64_t> b{10, 20, 30};
        Series ca = Series::flat_i64(a.data(), 3);
        Series cb = Series::flat_i64(b.data(), 3);
        const dftu_series* in[3] = {key.handle(), ca.handle(), cb.handle()};

        dftu_expr* kexpr = dftu_expr_col(0);  // bare string key column
        dftu_expr* c1 = dftu_expr_col(1);
        dftu_expr* c2 = dftu_expr_col(2);
        dftu_expr* ab = dftu_expr_binary(0 /*add*/, c1, c2);
        dftu_agg_spec specs[2] = {dftu_agg_count("n"),
                                  dftu_agg_sum(ab, "sum_ab")};

        dftu_series* outk = nullptr;
        dftu_series* outv[2] = {nullptr, nullptr};
        int32_t w =
            dftu_dataframe_group_agg_expr(kexpr, specs, 2, in, 3, &outk, outv);
        REQUIRE(w == 2);
        Series rk{outk};
        Series rn{outv[0]};
        Series rs{outv[1]};
        // x: rows 0,2 -> a+b = 11,33; sum=44, n=2.
        CHECK(rk.string_at(0) == "x");
        CHECK(rn.data<std::int64_t>()[0] == 2);
        CHECK(rs.data<std::int64_t>()[0] == 44);

        dftu_expr_free(kexpr);
        dftu_expr_free(c1);
        dftu_expr_free(c2);
        dftu_expr_free(ab);
    }

    TEST_CASE("field_stat_reduce: SIMD batch reduction matches scalar") {
        namespace ag = dftracer::utils::dataframe;
        // Large enough to cross several SIMD lanes plus a scalar tail.
        const std::int64_t n = 4099;
        std::vector<double> fv(static_cast<std::size_t>(n));
        std::vector<std::int64_t> iv(static_cast<std::size_t>(n));
        for (std::int64_t k = 0; k < n; ++k) {
            fv[static_cast<std::size_t>(k)] =
                static_cast<double>((k * 7) % 13) - 3.0 +
                0.25 * static_cast<double>(k % 5);
            iv[static_cast<std::size_t>(k)] = (k * 1000003) % 900000 - 450000;
        }
        // Float64: SIMD reduce vs scalar add must agree on every atom.
        Series fc = Series::flat_f64(fv.data(), n);
        ag::FieldStat simd = ag::field_stat_reduce(fc);
        ag::FieldStat ref;
        for (double x : fv) ref.add(x);
        CHECK(simd.n == ref.n);
        CHECK(simd.sum == doctest::Approx(ref.sum));
        CHECK(simd.sumsq == doctest::Approx(ref.sumsq));
        CHECK(simd.min == doctest::Approx(ref.min));
        CHECK(simd.max == doctest::Approx(ref.max));
        CHECK(simd.variance(true) == doctest::Approx(ref.variance(true)));
        CHECK(simd.skewness() == doctest::Approx(ref.skewness()));
        CHECK(simd.kurtosis() == doctest::Approx(ref.kurtosis()));

        // Int64: exact integer sum/min/max preserved through the SIMD path.
        Series ic = Series::flat_i64(iv.data(), n);
        ag::FieldStat isimd = ag::field_stat_reduce(ic);
        ag::FieldStat iref;
        for (std::int64_t x : iv) iref.add(x);
        CHECK(isimd.n == iref.n);
        CHECK(isimd.domain == ag::FieldStatDomain::I64);
        CHECK(isimd.esum == iref.esum);
        CHECK(isimd.emin == iref.emin);
        CHECK(isimd.emax == iref.emax);
        CHECK(isimd.sumsq == doctest::Approx(iref.sumsq));

        // A sub-range reduces only that slice.
        ag::FieldStat part = ag::field_stat_reduce(fc, 10, 20);
        ag::FieldStat pref;
        for (std::int64_t k = 10; k < 20; ++k)
            pref.add(fv[static_cast<std::size_t>(k)]);
        CHECK(part.n == 10);
        CHECK(part.sum == doctest::Approx(pref.sum));
    }

    TEST_CASE("agg engine: chunked group_agg matches over a large input") {
        namespace ag = dftracer::utils::dataframe;
        const std::int64_t n = 200000;  // > AGG_GRAIN, exercises chunk + merge
        std::vector<std::string> ks(static_cast<std::size_t>(n));
        std::vector<std::int64_t> dv(static_cast<std::size_t>(n));
        std::int64_t sum_a = 0, sum_b = 0, cnt_a = 0, cnt_b = 0;
        for (std::int64_t i = 0; i < n; ++i) {
            bool a = (i % 2) == 0;
            ks[static_cast<std::size_t>(i)] = a ? "a" : "b";
            dv[static_cast<std::size_t>(i)] = i;
            if (a) {
                sum_a += i;
                cnt_a++;
            } else {
                sum_b += i;
                cnt_b++;
            }
        }
        Series key = Series::strings(ks);
        Series dur = Series::flat_i64(dv.data(), n);
        std::vector<const Series*> vals{&dur};
        DataFrame g = ag::group_agg(
            key, vals, {{ag::AggOp::Count, -1, "n"}, {ag::AggOp::Sum, 0, "s"}},
            "cat");
        REQUIRE(g.num_rows() == 2);
        // first-seen order: a then b
        CHECK(g.columns[0].string_at(0) == "a");
        CHECK(g.columns[1].data<std::int64_t>()[0] == cnt_a);
        CHECK(g.columns[2].data<std::int64_t>()[0] == sum_a);
        CHECK(g.columns[1].data<std::int64_t>()[1] == cnt_b);
        CHECK(g.columns[2].data<std::int64_t>()[1] == sum_b);
    }

    TEST_CASE("expr engine: compile + fused eval (types, CSE, prim, compare)") {
        namespace ex = dftracer::utils::dataframe;
        std::vector<std::int64_t> av{1, 2, 3, 4};
        std::vector<std::int64_t> bv{10, 20, 30, 40};
        Series a = Series::flat_i64(av.data(), 4);
        Series b = Series::flat_i64(bv.data(), 4);
        std::vector<const Series*> in{&a, &b};

        // (a + b) * 2  -> int arithmetic, scalar op
        ex::Expr e =
            ex::expr_binary(ex::BinaryOp::Mul,
                            ex::expr_binary(ex::BinaryOp::Add, ex::expr_col(0),
                                            ex::expr_col(1)),
                            ex::expr_lit(std::int64_t{2}));
        Series r = ex::eval(e, in);
        CHECK(r.type() == TypeId::Int64);
        CHECK(r.data<std::int64_t>()[0] == 22);
        CHECK(r.data<std::int64_t>()[3] == 88);

        // a / 2  promotes to float
        Series rf = ex::eval(ex::expr_binary(ex::BinaryOp::Div, ex::expr_col(0),
                                             ex::expr_lit(std::int64_t{2})),
                             in);
        CHECK(rf.type() == TypeId::Float64);
        CHECK(rf.data<double>()[3] == doctest::Approx(2.0));

        // CSE: (a+b) reused - the compiled program shares one add slot, so the
        // result of (a+b)+(a+b) is 2*(a+b).
        ex::Expr sum = ex::expr_binary(ex::BinaryOp::Add, ex::expr_col(0),
                                       ex::expr_col(1));
        Series cse = ex::eval(ex::expr_binary(ex::BinaryOp::Add, sum, sum), in);
        CHECK(cse.data<std::int64_t>()[0] == 22);  // (1+10)*2

        // comparison -> bool
        Series mask = ex::eval(
            ex::expr_cmp(
                ex::CmpOp::Gt, ex::expr_col(1),
                dftracer::utils::dataframe::to_scalar<std::int64_t>(25)),
            in);
        CHECK(mask.type() == TypeId::Bool);
        const std::uint8_t* mb = mask.data<std::uint8_t>();
        CHECK(((mb[0] >> 0) & 1) == 0);  // 10 > 25 false
        CHECK(((mb[0] >> 2) & 1) == 1);  // 30 > 25 true
    }

    TEST_CASE("column slice is a zero-copy view into the parent buffer") {
        std::vector<std::int64_t> v{10, 11, 12, 13, 14, 15, 16, 17};
        Series c = Series::flat_i64(v.data(), 8);
        Series s = c.slice(2, 3);  // rows 12,13,14
        REQUIRE(s.valid());
        REQUIRE(s.length() == 3);
        CHECK(s.data<std::int64_t>()[0] == 12);
        CHECK(s.data<std::int64_t>()[2] == 14);
        // shares the parent's memory: the slice points 2 elements in.
        CHECK(s.data<std::int64_t>() == c.data<std::int64_t>() + 2);
        Series doubled = dv::add(s, s);
        CHECK(doubled.data<std::int64_t>()[0] == 24);
    }

    TEST_CASE("batch take/filter/slice apply across every column") {
        DataFrame b;
        b.names = {"name", "count"};
        b.columns.push_back(Series::strings({"a", "b", "c", "d"}));
        std::vector<std::int64_t> cnt{10, 20, 30, 40};
        b.columns.push_back(Series::flat_i64(cnt.data(), 4));

        DataFrame t = take(b, {3, 1});
        REQUIRE(t.num_rows() == 2);
        CHECK(t.columns[0].string_at(0) == "d");
        CHECK(t.columns[1].data<std::int64_t>()[1] == 20);

        DataFrame s = slice(b, 1, 2);  // rows b, c
        REQUIRE(s.num_rows() == 2);
        CHECK(s.columns[0].string_at(0) == "b");
        CHECK(s.columns[1].data<std::int64_t>()[1] == 30);

        Series mask = gt(b.columns[1], std::int64_t(15));  // count > 15
        DataFrame f = filter(b, mask);
        REQUIRE(f.num_rows() == 3);
        CHECK(f.columns[0].string_at(0) == "b");
        CHECK(f.columns[1].data<std::int64_t>()[2] == 40);
    }

    TEST_CASE(
        "frame ops: select/rename/with_column zero-copy, argsort/sort_by") {
        DataFrame b;
        b.names = {"name", "count"};
        b.columns.push_back(Series::strings({"a", "b", "c"}));
        std::vector<std::int64_t> cnt{30, 10, 20};
        b.columns.push_back(Series::flat_i64(cnt.data(), 3));

        // select projects, sharing the underlying buffers (no copy).
        DataFrame proj = select(b, {"count"});
        REQUIRE(proj.num_columns() == 1);
        CHECK(proj.names[0] == "count");
        CHECK(proj.columns[0].data<std::int64_t>() ==
              b.columns[1].data<std::int64_t>());  // same buffer pointer

        DataFrame rn = rename(b, {"key", "n"});
        CHECK(rn.names[0] == "key");
        CHECK(rn.names[1] == "n");
        CHECK(rn.num_rows() == 3);

        std::vector<std::int64_t> extra{1, 2, 3};
        DataFrame wc = with_column(b, "id", Series::flat_i64(extra.data(), 3));
        REQUIRE(wc.num_columns() == 3);
        CHECK(wc.names[2] == "id");

        // argsort of {30,10,20} ascending -> [1,2,0]; sort_by orders every col.
        Series ord = argsort(b.columns[1], false);
        REQUIRE(ord.length() == 3);
        CHECK(ord.data<std::int64_t>()[0] == 1);
        CHECK(ord.data<std::int64_t>()[1] == 2);
        CHECK(ord.data<std::int64_t>()[2] == 0);

        DataFrame asc = sort_by(b, "count", false);
        CHECK(asc.columns[1].data<std::int64_t>()[0] == 10);
        CHECK(asc.columns[0].string_at(0) == "b");
        DataFrame desc = sort_by(b, "count", true);
        CHECK(desc.columns[1].data<std::int64_t>()[0] == 30);
        CHECK(desc.columns[0].string_at(0) == "a");
    }

    TEST_CASE("argsort SIMD fast path matches scalar across types") {
        // Float with negatives exercises the order-preserving bit transform.
        std::vector<double> f{3.5, -1.0, -2.5, 0.0, 2.5};
        Series fo = argsort(Series::flat_f64(f.data(), 5), false);
        const std::int64_t* p = fo.data<std::int64_t>();
        CHECK(p[0] == 2);  // -2.5
        CHECK(p[1] == 1);  // -1.0
        CHECK(p[2] == 3);  // 0.0
        CHECK(p[3] == 4);  // 2.5
        CHECK(p[4] == 0);  // 3.5

        // Signed int32, descending.
        std::vector<std::int32_t> vi{-5, 10, 3, 10, -5};
        Series io = argsort(Series::flat(TypeId::Int32, vi.data(), 5), true);
        const std::int64_t* q = io.data<std::int64_t>();
        CHECK(q[0] == 1);  // 10 (first)
        CHECK(q[1] == 3);  // 10 (ties keep ascending index -> stable)
        CHECK(q[2] == 2);  // 3

        // Equal keys stay stable ascending (SIMD packs the index in low bits).
        std::vector<std::int64_t> eq{7, 7, 7};
        Series eo = argsort(Series::flat_i64(eq.data(), 3), false);
        CHECK(eo.data<std::int64_t>()[0] == 0);
        CHECK(eo.data<std::int64_t>()[1] == 1);
        CHECK(eo.data<std::int64_t>()[2] == 2);

        // String falls back to the scalar path.
        Series so =
            argsort(Series::strings({"banana", "apple", "cherry"}), false);
        const std::int64_t* s = so.data<std::int64_t>();
        CHECK(s[0] == 1);  // apple
        CHECK(s[1] == 0);  // banana
        CHECK(s[2] == 2);  // cherry
    }

    // The degenerate ends of every ordering op. highway's vector sort is
    // skipped on some targets (every SVE one), and its heap fallback loops
    // forever on an empty range and runs past the end when it is asked for as
    // many keys as it was given, so each of these hung or corrupted there
    // while returning at once everywhere else.
    TEST_CASE("ordering ops at their degenerate ends") {
        std::vector<std::int64_t> vals{5, 3, 9, 1, 7};
        for (std::int64_t n = 0; n <= 5; ++n) {
            const Series v = Series::flat_i64(n ? vals.data() : nullptr, n);
            REQUIRE(v.length() == n);
            CHECK(argsort(v, false).length() == n);
            CHECK(argsort(v, true).length() == n);
            CHECK(v.sort().length() == n);
            CHECK(dv::unique(v).length() <= n);
            CHECK(dv::nunique(v) <= n);
            for (std::int64_t k = 0; k <= n + 1; ++k) {
                const std::int64_t want = std::min(k, n);
                CHECK(dv::topk_indices(v, k, true).length() == want);
                CHECK(dv::topk_indices(v, k, false).length() == want);
            }
        }
        // The values, not just the lengths: the largest two of the five.
        const Series five = Series::flat_i64(vals.data(), 5);
        const Series top2 = dv::topk_indices(five, 2, true);
        REQUIRE(top2.length() == 2);
        CHECK(vals[static_cast<std::size_t>(top2.data<std::int64_t>()[0])] ==
              9);
        CHECK(vals[static_cast<std::size_t>(top2.data<std::int64_t>()[1])] ==
              7);
    }

    TEST_CASE("stats: rank / rolling / cummax / cummin / moments") {
        // rank (average ties): sorted 1,1,2,3 -> ranks by original index.
        std::vector<std::int64_t> rv{3, 1, 2, 1};
        Series rk = dv::rank(Series::flat_i64(rv.data(), 4),
                             dv::RankMethod::Average, false);
        REQUIRE(rk.type() == TypeId::Float64);
        const double* rp = rk.data<double>();
        CHECK(rp[0] == doctest::Approx(4.0));
        CHECK(rp[1] == doctest::Approx(1.5));
        CHECK(rp[2] == doctest::Approx(3.0));
        CHECK(rp[3] == doctest::Approx(1.5));

        Series rd = dv::rank(Series::flat_i64(rv.data(), 4),
                             dv::RankMethod::Dense, false);
        CHECK(rd.data<double>()[0] ==
              doctest::Approx(3.0));  // 3 is 3rd distinct

        // rolling window 3 over 1..5.
        std::vector<std::int64_t> wv{1, 2, 3, 4, 5};
        Series rs =
            dv::rolling(Series::flat_i64(wv.data(), 5), 3, dv::RollingOp::Sum);
        REQUIRE(rs.length() == 5);
        CHECK(rs.is_null(0));
        CHECK(rs.is_null(1));
        CHECK(rs.data<double>()[2] == doctest::Approx(6.0));
        CHECK(rs.data<double>()[4] == doctest::Approx(12.0));
        Series rmax =
            dv::rolling(Series::flat_i64(wv.data(), 5), 3, dv::RollingOp::Max);
        CHECK(rmax.data<double>()[4] == doctest::Approx(5.0));
        Series rmean =
            dv::rolling(Series::flat_i64(wv.data(), 5), 3, dv::RollingOp::Mean);
        CHECK(rmean.data<double>()[3] == doctest::Approx(3.0));

        // cummax / cummin.
        std::vector<std::int64_t> cv{3, 1, 4, 1, 5};
        Series cmx = dv::cummax(Series::flat_i64(cv.data(), 5));
        CHECK(cmx.data<std::int64_t>()[1] == 3);
        CHECK(cmx.data<std::int64_t>()[4] == 5);
        Series cmn = dv::cummin(Series::flat_i64(cv.data(), 5));
        CHECK(cmn.data<std::int64_t>()[4] == 1);

        // SIMD moments: a symmetric set has ~zero skew; kurtosis is finite.
        std::vector<double> sym{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        Series sc = Series::flat_f64(sym.data(), 10);
        CHECK(dv::skewness(sc) == doctest::Approx(0.0).epsilon(1e-9));
        CHECK(dv::variance(sc, false) == doctest::Approx(8.25));  // population
    }

    TEST_CASE("stats: quantile/median/variance/skew/nunique/unique") {
        std::vector<std::int64_t> v{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        Series c = Series::flat_i64(v.data(), 10);
        CHECK(dv::median(c) == doctest::Approx(5.5));
        CHECK(dv::quantile(c, 0.0) == doctest::Approx(1.0));
        CHECK(dv::quantile(c, 1.0) == doctest::Approx(10.0));
        CHECK(dv::quantile(c, 0.25) == doctest::Approx(3.25));
        // sample variance of 1..10 is 9.1666...
        CHECK(dv::variance(c, true) == doctest::Approx(9.16666).epsilon(0.001));
        CHECK(dv::stddev(c, true) == doctest::Approx(3.02765).epsilon(0.001));

        std::vector<std::int64_t> d{3, 1, 3, 2, 1, 3};
        Series dc = Series::flat_i64(d.data(), 6);
        CHECK(dv::nunique(dc) == 3);
        Series u = dv::unique(dc);
        REQUIRE(u.length() == 3);
        CHECK(u.data<std::int64_t>()[0] == 1);
        CHECK(u.data<std::int64_t>()[1] == 2);
        CHECK(u.data<std::int64_t>()[2] == 3);
    }

    TEST_CASE("D1 windowed stats: rolling_var/std/median/quantile") {
        std::vector<std::int64_t> wv{1, 2, 3, 4, 5};
        Series src = Series::flat_i64(wv.data(), 5);
        // window 3 over 1..5: windows {1,2,3},{2,3,4},{3,4,5} each var 1.0.
        Series rv = dv::rolling_var(Series::flat_i64(wv.data(), 5), 3);
        REQUIRE(rv.type() == TypeId::Float64);
        CHECK(rv.is_null(0));
        CHECK(rv.is_null(1));
        CHECK(rv.data<double>()[2] == doctest::Approx(1.0));
        CHECK(rv.data<double>()[4] == doctest::Approx(1.0));
        Series rs = dv::rolling_std(Series::flat_i64(wv.data(), 5), 3);
        CHECK(rs.data<double>()[2] == doctest::Approx(1.0));
        Series rm = dv::rolling_median(Series::flat_i64(wv.data(), 5), 3);
        CHECK(rm.is_null(1));
        CHECK(rm.data<double>()[2] == doctest::Approx(2.0));
        CHECK(rm.data<double>()[4] == doctest::Approx(4.0));
        Series rq =
            dv::rolling_quantile(Series::flat_i64(wv.data(), 5), 3, 1.0);
        CHECK(rq.data<double>()[2] == doctest::Approx(3.0));  // window max
        (void)src;
    }

    TEST_CASE("D1 ewm_mean / ewm_std") {
        std::vector<double> x{1, 2, 3, 4};
        Series em = dv::ewm_mean(Series::flat_f64(x.data(), 4), 0.5);
        REQUIRE(em.type() == TypeId::Float64);
        // pandas adjust=True: y1 = (2 + .5*1) / 1.5; y3 = (4 + .5*3 + .25*2
        // + .125*1) / 1.875 = 3.2666...
        CHECK(em.data<double>()[0] == doctest::Approx(1.0));
        CHECK(em.data<double>()[1] == doctest::Approx(5.0 / 3.0));
        CHECK(em.data<double>()[3] == doctest::Approx(3.2666666667));
        // A null row repeats the previous value and decays the weights.
        const double hx[3] = {1.0, 0.0, 3.0};
        const std::uint8_t hv[1] = {0x05};
        Series holes = dv::ewm_mean(Series::flat_f64(hx, 3, hv), 0.5);
        CHECK(holes.data<double>()[1] == doctest::Approx(1.0));
        CHECK(holes.data<double>()[2] == doctest::Approx(2.6));
        CHECK_FALSE(holes.is_null(1));
        Series es = dv::ewm_std(Series::flat_f64(x.data(), 4), 0.5);
        CHECK(es.is_null(0));  // sample std of one point is undefined
        CHECK(es.data<double>()[1] == doctest::Approx(std::sqrt(0.5)));
    }

    TEST_CASE("D1 cut / qcut bucketing") {
        std::vector<double> v{5, 15, 25, 35};
        std::vector<double> edges{10, 20, 30};
        Series binned = dv::cut(Series::flat_f64(v.data(), 4),
                                Series::flat_f64(edges.data(), 3));
        REQUIRE(binned.type() == TypeId::Int32);
        // count of edges <= x: 5->0, 15->1, 25->2, 35->3.
        CHECK(binned.data<std::int32_t>()[0] == 0);
        CHECK(binned.data<std::int32_t>()[1] == 1);
        CHECK(binned.data<std::int32_t>()[2] == 2);
        CHECK(binned.data<std::int32_t>()[3] == 3);

        std::vector<std::int64_t> q{1, 2, 3, 4, 5, 6, 7, 8};
        Series qb = dv::qcut(Series::flat_i64(q.data(), 8), 4);  // quartiles
        REQUIRE(qb.type() == TypeId::Int32);
        CHECK(qb.data<std::int32_t>()[0] == 0);  // smallest in first bucket
        CHECK(qb.data<std::int32_t>()[7] == 3);  // largest in last bucket
    }

    TEST_CASE("D1 search_sorted / interpolate") {
        std::vector<std::int64_t> sorted{1, 3, 5, 7};
        std::vector<std::int64_t> queries{0, 3, 4, 8};
        Series idx = dv::search_sorted(Series::flat_i64(sorted.data(), 4),
                                       Series::flat_i64(queries.data(), 4));
        REQUIRE(idx.type() == TypeId::Int64);
        CHECK(idx.data<std::int64_t>()[0] == 0);  // 0 before 1
        CHECK(idx.data<std::int64_t>()[1] == 1);  // 3 lower-bound at index 1
        CHECK(idx.data<std::int64_t>()[2] == 2);  // 4 between 3 and 5
        CHECK(idx.data<std::int64_t>()[3] == 4);  // 8 past the end

        // interior nulls interpolate; leading/trailing nulls stay null.
        std::vector<double> iv{0, 0, 10, 0, 0, 40, 0};
        std::uint8_t vbits = 0b0100100;  // valid at index 2 and 5 only
        Series in =
            Series::flat(TypeId::Float64, iv.data(), 7, &vbits).interpolate();
        REQUIRE(in.type() == TypeId::Float64);
        CHECK(in.is_null(0));
        CHECK(in.is_null(1));
        CHECK(in.data<double>()[2] == doctest::Approx(10.0));
        CHECK(in.data<double>()[3] == doctest::Approx(20.0));  // 10 + 30*1/3
        CHECK(in.data<double>()[4] == doctest::Approx(30.0));  // 10 + 30*2/3
        CHECK(in.data<double>()[5] == doctest::Approx(40.0));
        CHECK(in.is_null(6));
    }

    TEST_CASE("D1 is_between (SIMD) and dot (SIMD)") {
        std::vector<std::int64_t> v{1, 5, 10, 15, 20};
        Series mask =
            Series::flat_i64(v.data(), 5).is_between<std::int64_t>(5, 15);
        REQUIRE(mask.type() == TypeId::Bool);
        const std::uint8_t* bits = mask.data<std::uint8_t>();
        auto bit = [&](int i) { return (bits[i >> 3] >> (i & 7)) & 1; };
        CHECK(bit(0) == 0);  // 1
        CHECK(bit(1) == 1);  // 5
        CHECK(bit(2) == 1);  // 10
        CHECK(bit(3) == 1);  // 15
        CHECK(bit(4) == 0);  // 20

        std::vector<double> a{1, 2, 3, 4};
        std::vector<double> b{10, 20, 30, 40};
        auto d =
            Series::flat_f64(a.data(), 4).dot(Series::flat_f64(b.data(), 4));
        CHECK(d.f64() ==
              doctest::Approx(1 * 10 + 2 * 20 + 3 * 30 + 4 * 40));  // 300

        // Narrow (16-bit) type now takes the SIMD between / filter_gt path
        // instead of the scalar widened-double fallback. Exercise >64 rows.
        std::vector<std::int16_t> w(100);
        for (std::size_t i = 0; i < w.size(); ++i)
            w[i] = static_cast<std::int16_t>(i);
        Series c16 = Series::flat(TypeId::Int16, w.data(), 100);
        Series bm = c16.is_between<std::int16_t>(10, 20);
        const std::uint8_t* bb = bm.data<std::uint8_t>();
        auto b16 = [&](int i) { return (bb[i >> 3] >> (i & 7)) & 1; };
        CHECK(b16(9) == 0);
        CHECK(b16(10) == 1);
        CHECK(b16(20) == 1);
        CHECK(b16(21) == 0);
        CHECK(filter_gt(c16, static_cast<std::int16_t>(89)).length() == 10);
    }

    TEST_CASE("batch group_by re-aggregates a materialized batch") {
        DataFrame b;
        b.names = {"cat", "dur"};
        b.columns.push_back(Series::strings({"io", "io", "cpu", "io", "cpu"}));
        std::vector<std::int64_t> dur{10, 20, 5, 30, 15};
        b.columns.push_back(Series::flat_i64(dur.data(), 5));

        DataFrame g = dv::group_by(
            b, "cat",
            {{dv::Agg::Count, "", "count"}, {dv::Agg::Sum, "dur", "sum_dur"}});
        REQUIRE(g.num_rows() == 2);
        REQUIRE(g.num_columns() == 3);
        CHECK(g.names[0] == "cat");
        CHECK(g.names[1] == "count");
        CHECK(g.names[2] == "sum_dur");
        // First-seen key order: io then cpu.
        CHECK(g.columns[0].string_at(0) == "io");
        CHECK(g.columns[1].data<std::int64_t>()[0] == 3);   // io count
        CHECK(g.columns[2].data<std::int64_t>()[0] == 60);  // io sum 10+20+30
        CHECK(g.columns[0].string_at(1) == "cpu");
        CHECK(g.columns[2].data<std::int64_t>()[1] == 20);  // cpu sum 5+15
    }

    TEST_CASE("hash_partition co-locates equal keys and preserves rows") {
        DataFrame b;
        b.names = {"cat", "dur"};
        b.columns.push_back(
            Series::strings({"io", "cpu", "io", "net", "cpu", "io"}));
        std::vector<std::int64_t> dur{1, 2, 3, 4, 5, 6};
        b.columns.push_back(Series::flat_i64(dur.data(), 6));

        std::vector<DataFrame> parts = dv::hash_partition(b, {"cat"}, 4);
        REQUIRE(parts.size() == 4);

        std::int64_t total = 0;
        std::map<std::string, std::size_t> home;
        bool colocated = true;
        for (std::size_t p = 0; p < parts.size(); ++p) {
            total += parts[p].num_rows();
            for (std::int64_t r = 0; r < parts[p].num_rows(); ++r) {
                std::string k(parts[p].columns[0].string_at(r));
                auto it = home.find(k);
                if (it == home.end())
                    home[k] = p;
                else if (it->second != p)
                    colocated = false;
            }
        }
        CHECK(total == 6);        // every row lands in exactly one part
        CHECK(colocated);         // rows with equal keys share a part
        CHECK(home.size() == 3);  // io, cpu, net

        // partition_id is the per-row part each row of hash_partition landed
        // in, and it is a registry op.
        Series ids = dv::partition_id(b, {"cat"}, 4);
        REQUIRE(ids.type() == TypeId::Int32);
        REQUIRE(ids.length() == 6);
        const std::int32_t* id = ids.data<std::int32_t>();
        for (std::int64_t r = 0; r < 6; ++r) {
            std::string k(b.columns[0].string_at(r));
            CHECK(static_cast<std::size_t>(id[r]) == home[k]);
        }
        CHECK(id[0] == id[2]);
        CHECK(id[1] == id[4]);
        CHECK_THROWS_AS(dv::partition_id(b, {}, 4), std::invalid_argument);
        CHECK_THROWS_AS(dv::partition_id(b, {"cat"}, 0), std::invalid_argument);
        CHECK_THROWS_AS(dv::partition_id(b, {"nope"}, 4), std::out_of_range);
        const char* keys[1] = {"cat"};
        const dftu_dataframe* h = dftu_dataframe_new(
            std::vector<const char*>{"cat", "dur"}.data(),
            std::vector<dftu_series*>{b.columns[0].share().release(),
                                      b.columns[1].share().release()}
                .data(),
            2);
        REQUIRE(h);
        const dftu_op_desc* op = dftu_op_find("dftu.frame.partition_id");
        REQUIRE(op);
        dv::OpArgs arg;
        arg.frame(0, h).strlist(1, keys, 1).i64(2, 4);
        dftu_series* via_op = dftu_op_run(op, nullptr, 0, arg);
        REQUIRE(via_op);
        CHECK(dftu_series_length(via_op) == 6);
        CHECK(std::memcmp(dftu_series_data(via_op), id, 6 * sizeof(int32_t)) ==
              0);
        dftu_series_free(via_op);
        dftu_dataframe_free(const_cast<dftu_dataframe*>(h));
    }

    TEST_CASE("topk: k largest/smallest via partial sort") {
        DataFrame b;
        b.names = {"name", "dur"};
        b.columns.push_back(Series::strings({"a", "b", "c", "d", "e"}));
        std::vector<std::int64_t> dur{50, 10, 90, 30, 70};
        b.columns.push_back(Series::flat_i64(dur.data(), 5));

        DataFrame top2 = dv::topk(b, "dur", 2, true);  // largest
        REQUIRE(top2.num_rows() == 2);
        CHECK(top2.columns[1].data<std::int64_t>()[0] == 90);
        CHECK(top2.columns[1].data<std::int64_t>()[1] == 70);
        CHECK(top2.columns[0].string_at(0) == "c");

        DataFrame bot2 = dv::topk(b, "dur", 2, false);  // smallest
        CHECK(bot2.columns[1].data<std::int64_t>()[0] == 10);
        CHECK(bot2.columns[1].data<std::int64_t>()[1] == 30);
    }

    TEST_CASE("concat vertically merges batches (numeric + string)") {
        DataFrame a;
        a.names = {"k", "n"};
        a.columns.push_back(Series::strings({"x", "y"}));
        std::vector<std::int64_t> an{1, 2};
        a.columns.push_back(Series::flat_i64(an.data(), 2));
        DataFrame b;
        b.names = {"k", "n"};
        b.columns.push_back(Series::strings({"z"}));
        std::vector<std::int64_t> bn{3};
        b.columns.push_back(Series::flat_i64(bn.data(), 1));

        DataFrame m = dv::concat({&a, &b});
        REQUIRE(m.num_rows() == 3);
        CHECK(m.columns[0].string_at(2) == "z");
        CHECK(m.columns[1].data<std::int64_t>()[2] == 3);
    }

    TEST_CASE("concat diagonal unions columns, null-fills, promotes numeric") {
        // Part a: {k(str), n(i64)}; part b: {k(str), r(f64)} - disjoint n/r,
        // plus a shared numeric column m that is i64 in a and f64 in b.
        DataFrame a;
        a.names = {"k", "n", "m"};
        a.columns.push_back(Series::strings({"x", "y"}));
        std::vector<std::int64_t> an{1, 2};
        a.columns.push_back(Series::flat_i64(an.data(), 2));
        std::vector<std::int64_t> am{10, 20};
        a.columns.push_back(Series::flat_i64(am.data(), 2));
        DataFrame b;
        b.names = {"k", "r", "m"};
        b.columns.push_back(Series::strings({"z"}));
        std::vector<double> br{2.5};
        b.columns.push_back(Series::flat_f64(br.data(), 1));
        std::vector<double> bm{30.5};
        b.columns.push_back(Series::flat_f64(bm.data(), 1));

        DataFrame m = dv::concat({&a, &b}, dv::ConcatHow::Diagonal);
        REQUIRE(m.num_rows() == 3);
        // Column order = first appearance: k, n, m, r.
        REQUIRE(m.names == std::vector<std::string>{"k", "n", "m", "r"});

        const std::int64_t ki = m.column_index("k");
        const std::int64_t ni = m.column_index("n");
        const std::int64_t mi = m.column_index("m");
        const std::int64_t ri = m.column_index("r");

        // k present in both.
        CHECK(m.columns[ki].string_at(2) == "z");
        // n only in a -> b's row is null.
        CHECK(m.columns[ni].data<std::int64_t>()[0] == 1);
        CHECK(m.columns[ni].is_null(2));
        // r only in b -> a's rows are null.
        CHECK(m.columns[ri].is_null(0));
        CHECK(m.columns[ri].data<double>()[2] == doctest::Approx(2.5));
        // m promoted i64+f64 -> Float64 for all rows.
        CHECK(m.columns[mi].type() ==
              dftracer::utils::dataframe::TypeId::Float64);
        CHECK(m.columns[mi].data<double>()[0] == doctest::Approx(10));
        CHECK(m.columns[mi].data<double>()[2] == doctest::Approx(30.5));
    }

    TEST_CASE("concat diagonal unifies a String/numeric clash on String") {
        // A scalar column that is String in one part and numeric in another
        // (build_row_frame infers an arg column's type per batch) unifies on
        // String with numbers stringified, instead of aborting the concat.
        DataFrame a;
        a.names = {"v"};
        a.columns.push_back(Series::strings({"s"}));
        DataFrame b;
        b.names = {"v"};
        std::vector<std::int64_t> bn{1};
        b.columns.push_back(Series::flat_i64(bn.data(), 1));
        DataFrame m = dv::concat({&a, &b}, dv::ConcatHow::Diagonal);
        REQUIRE(m.columns.size() == 1);
        CHECK(m.columns[0].type() ==
              dftracer::utils::dataframe::TypeId::String);
        REQUIRE(m.num_rows() == 2);
        CHECK(m.columns[0].string_at(0) == "s");
        CHECK(m.columns[0].string_at(1) == "1");
    }

    TEST_CASE("concat diagonal throws on a nested/scalar type clash") {
        // A List column against a scalar has no meaningful unification.
        DataFrame a;
        a.names = {"v"};
        std::vector<std::int32_t> offs{0, 1};
        std::vector<std::int64_t> child{7};
        a.columns.push_back(
            Series::list(offs, Series::flat_i64(child.data(), 1)));
        DataFrame b;
        b.names = {"v"};
        std::vector<std::int64_t> bn{1};
        b.columns.push_back(Series::flat_i64(bn.data(), 1));
        CHECK_THROWS_AS(dv::concat({&a, &b}, dv::ConcatHow::Diagonal),
                        std::invalid_argument);
    }

    TEST_CASE("elementwise: abs/clip/round/fillna/cumsum") {
        std::vector<std::int64_t> v{-3, 5, -7, 2};
        Series ab = dv::abs(Series::flat_i64(v.data(), 4));
        CHECK(ab.data<std::int64_t>()[0] == 3);
        CHECK(ab.data<std::int64_t>()[2] == 7);

        Series cl = dv::clip(Series::flat_i64(v.data(), 4),
                             dv::to_scalar<std::int64_t>(-2),
                             dv::to_scalar<std::int64_t>(3));
        CHECK(cl.data<std::int64_t>()[0] == -2);  // -3 -> -2
        CHECK(cl.data<std::int64_t>()[1] == 3);   // 5 -> 3

        std::vector<double> f{1.4, 2.5, -1.6};
        Series rd = dv::round(Series::flat_f64(f.data(), 3));
        CHECK(rd.data<double>()[0] == doctest::Approx(1.0));
        CHECK(rd.data<double>()[2] == doctest::Approx(-2.0));

        std::vector<std::int64_t> cs{1, 2, 3, 4};
        Series cm = dv::cumsum(Series::flat_i64(cs.data(), 4));
        CHECK(cm.data<std::int64_t>()[3] == 10);
    }

    TEST_CASE("take with a negative index gathers a null (join OUTER fill)") {
        std::vector<std::int64_t> v{10, 20, 30};
        Series g = take(Series::flat_i64(v.data(), 3), {1, -1, 0});
        REQUIRE(g.length() == 3);
        CHECK(g.data<std::int64_t>()[0] == 20);
        CHECK(g.data<std::int64_t>()[2] == 10);
        CHECK(g.null_count() == 1);  // row 1 (index -1) is null

        Series gs = take(Series::strings({"a", "b"}), {-1, 1, 0});
        REQUIRE(gs.length() == 3);
        CHECK(gs.null_count() == 1);
        CHECK(gs.string_at(1) == "b");
        CHECK(gs.string_at(2) == "a");
    }

    TEST_CASE("take gathers nested list<struct> rows") {
        std::vector<double> lo{0.0, 1.0, 5.0};
        std::vector<double> hi{1.0, 2.0, 6.0};
        std::vector<std::int64_t> cnt{10, 20, 30};
        std::vector<Series> fields;
        fields.push_back(Series::flat_f64(lo.data(), 3));
        fields.push_back(Series::flat_f64(hi.data(), 3));
        fields.push_back(Series::flat_i64(cnt.data(), 3));
        Series st = Series::structs({"lo", "hi", "count"}, std::move(fields));
        std::vector<std::int32_t> offsets{0, 2,
                                          3};  // row0: 2 bins, row1: 1 bin
        Series hist = Series::list(offsets, std::move(st));

        // Gather row 1 (1 bin) then row 0 (2 bins).
        Series g = take(hist, {1, 0});
        REQUIRE(g.valid());
        CHECK(g.type() == TypeId::List);
        REQUIRE(g.length() == 2);
    }

    TEST_CASE("dictionary_encode dedups strings and materializes back") {
        Series d =
            dictionary_encode(Series::strings({"a", "b", "a", "a", "b"}));
        REQUIRE(d.valid());
        CHECK(d.encoding() == Encoding::Dictionary);
        REQUIRE(d.length() == 5);

        Series flat = materialize(d);
        REQUIRE(flat.valid());
        CHECK(flat.type() == TypeId::String);
        REQUIRE(flat.length() == 5);
        CHECK(flat.string_at(0) == "a");
        CHECK(flat.string_at(1) == "b");
        CHECK(flat.string_at(2) == "a");
        CHECK(flat.string_at(4) == "b");
    }

    TEST_CASE("filter_gt yields a selection, materialize gathers it") {
        std::vector<std::int64_t> v{5, 1, 9, 3, 7};
        Series col = Series::flat_i64(v.data(), 5);

        Series sel = filter_gt(col, 4);
        REQUIRE(sel.valid());
        CHECK(sel.encoding() == Encoding::Selection);
        REQUIRE(sel.length() == 3);  // 5, 9, 7

        Series flat = materialize(sel);
        REQUIRE(flat.valid());
        CHECK(flat.encoding() == Encoding::Flat);
        REQUIRE(flat.length() == 3);
        const std::int64_t* p = flat.data<std::int64_t>();
        CHECK(p[0] == 5);
        CHECK(p[1] == 9);
        CHECK(p[2] == 7);
    }

    TEST_CASE("filter_gt SIMD compaction matches scalar over a large column") {
        std::vector<std::int64_t> v(100);
        for (int i = 0; i < 100; ++i) v[static_cast<std::size_t>(i)] = i;
        Series sel = filter_gt(Series::flat_i64(v.data(), 100), 49);  // 50..99
        REQUIRE(sel.valid());
        REQUIRE(sel.length() == 50);
        Series flat = materialize(sel);
        const std::int64_t* p = flat.data<std::int64_t>();
        CHECK(p[0] == 50);
        CHECK(p[49] == 99);

        std::vector<double> f(64);
        for (int i = 0; i < 64; ++i) f[static_cast<std::size_t>(i)] = i * 1.0;
        Series fs = filter_gt(Series::flat_f64(f.data(), 64), 31.5);  // 32..63
        REQUIRE(fs.length() == 32);
        Series fflat = materialize(fs);
        const double* fp = fflat.data<double>();
        CHECK(fp[0] == doctest::Approx(32.0));
        CHECK(fp[31] == doctest::Approx(63.0));
    }

    TEST_CASE("filter by a mask composes with comparison") {
        std::vector<std::int64_t> v{5, 1, 9, 3, 7};
        Series col = Series::flat_i64(v.data(), 5);

        Series mask = gt(col, 4);        // 1,0,1,0,1 -> rows 0,2,4
        Series sel = filter(col, mask);  // SELECTION over col
        REQUIRE(sel.valid());
        CHECK(sel.encoding() == Encoding::Selection);
        REQUIRE(sel.length() == 3);

        Series flat = materialize(sel);
        const std::int64_t* p = flat.data<std::int64_t>();
        CHECK(p[0] == 5);
        CHECK(p[1] == 9);
        CHECK(p[2] == 7);
    }

    TEST_CASE("filter_gt skips nulls") {
        std::vector<std::int64_t> v{9, 9, 9, 9};
        // Bitmap: rows 0 and 2 valid, rows 1 and 3 null.
        std::uint8_t bitmap = 0b0000'0101;
        Series col = Series::flat_i64(v.data(), 4, &bitmap);
        REQUIRE(col.null_count() == 2);

        Series sel = filter_gt(col, 4);
        REQUIRE(sel.valid());
        CHECK(sel.length() == 2);  // only the two valid nines
    }

    TEST_CASE("A1 reducers: product/all/any/arg_min/arg_max/mode") {
        std::vector<std::int64_t> v{2, 3, 4};
        Series c = Series::flat_i64(v.data(), 3);
        CHECK(scalar_value<std::int64_t>(c.product()) == 24);

        std::vector<std::int64_t> w{5, 1, 9, 1, 9};
        Series cw = Series::flat_i64(w.data(), 5);
        CHECK(cw.arg_min() == 1);  // first 1
        CHECK(cw.arg_max() == 2);  // first 9
        CHECK(scalar_value<std::int64_t>(cw.mode()) ==
              1);                  // 1 hits count 2 first

        // Bool reductions via a comparison mask.
        Series allpos = dv::gt(cw, std::int64_t(0));   // all true
        CHECK(allpos.all());
        CHECK(allpos.any());
        Series somepos = dv::gt(cw, std::int64_t(8));  // 0,0,1,0,1
        CHECK_FALSE(somepos.all());
        CHECK(somepos.any());
    }

    TEST_CASE(
        "value_counts returns value/count DataFrame, most-frequent first") {
        std::vector<std::int64_t> v{5, 1, 5, 5, 1, 9};
        Series c =
            Series::flat_i64(v.data(), static_cast<std::int64_t>(v.size()));
        DataFrame vc = c.value_counts();

        REQUIRE(vc.num_columns() == 2);
        CHECK(vc.column_index("value") == 0);
        CHECK(vc.column_index("count") == 1);
        REQUIRE(vc.num_rows() == 3);  // distinct values 5, 1, 9

        Series values = vc.column("value");
        Series counts = vc.column("count");
        REQUIRE(values.type() == dftracer::utils::dataframe::TypeId::Int64);
        REQUIRE(counts.type() == dftracer::utils::dataframe::TypeId::Int64);

        // Most-frequent first: 5 (x3), then 1 (x2), then 9 (x1).
        const std::int64_t* val = values.data<std::int64_t>();
        const std::int64_t* cnt = counts.data<std::int64_t>();
        CHECK(val[0] == 5);
        CHECK(cnt[0] == 3);
        CHECK(cnt[1] == 2);
        CHECK(cnt[2] == 1);
    }

    TEST_CASE("Scalar ergonomic typed accessors") {
        std::vector<double> f{1.0, 5.0, 2.0, 4.0};
        Series fc = Series::flat_f64(f.data(), 4);
        CHECK(fc.max().f64() == doctest::Approx(5.0));
        CHECK(fc.sum().f64() == doctest::Approx(12.0));
        CHECK(fc.count() == 4);

        std::vector<std::int64_t> iv{10, 20, 30};
        Series ic = Series::flat_i64(iv.data(), 3);
        CHECK(ic.max().i64() == 30);
        CHECK(ic.sum().i64() == 60);
        CHECK(ic.max().u64() == 30u);
        CHECK(ic.max().as<double>() == doctest::Approx(30.0));
        CHECK(ic.max().tag() == DFTU_SCALAR_TAG_I64);
        CHECK(ic.min().boolean() == true);

        dftu_scalar raw = ic.max();           // implicit Scalar -> dftu_scalar
        CHECK(raw.value.i == 30);
        Series filled = ic.fillna(ic.min());  // Scalar as scalar-taking arg
        CHECK(filled.valid());

        std::vector<std::string> names{"a", "b", "b"};
        Series sc = Series::strings(names);
        CHECK(sc.mode().str().empty());
    }

    TEST_CASE("A1 rounding + sign/negate elementwise") {
        std::vector<double> f{1.2, -1.7, 2.5, -2.5};
        Series c = Series::flat_f64(f.data(), 4);
        Series ce = dv::ceil(c);
        CHECK(ce.data<double>()[0] == doctest::Approx(2.0));
        CHECK(ce.data<double>()[1] == doctest::Approx(-1.0));
        Series fl = dv::floor(c);
        CHECK(fl.data<double>()[0] == doctest::Approx(1.0));
        CHECK(fl.data<double>()[3] == doctest::Approx(-3.0));
        Series tr = dv::trunc(c);
        CHECK(tr.data<double>()[2] == doctest::Approx(2.0));
        CHECK(tr.data<double>()[3] == doctest::Approx(-2.0));

        std::vector<std::int64_t> s{-4, 0, 7};
        Series cs = Series::flat_i64(s.data(), 3);
        Series sg = dv::sign(cs);
        CHECK(sg.data<std::int64_t>()[0] == -1);
        CHECK(sg.data<std::int64_t>()[1] == 0);
        CHECK(sg.data<std::int64_t>()[2] == 1);
        Series ng = dv::negate(cs);
        CHECK(ng.data<std::int64_t>()[0] == 4);
        CHECK(ng.data<std::int64_t>()[2] == -7);
    }

    TEST_CASE("A1 diff/pct_change/scans/sqrt/exp/log") {
        std::vector<std::int64_t> v{10, 13, 9, 12};
        Series c = Series::flat_i64(v.data(), 4);
        Series d = dv::diff(c);
        REQUIRE(d.length() == 4);
        CHECK(d.is_null(0));
        CHECK(d.data<std::int64_t>()[1] == 3);
        CHECK(d.data<std::int64_t>()[2] == -4);

        Series pc = dv::pct_change(c);
        CHECK(pc.type() == TypeId::Float64);
        CHECK(pc.is_null(0));
        CHECK(pc.data<double>()[1] == doctest::Approx(0.3));

        std::vector<std::int64_t> p{1, 2, 3, 4};
        Series cp = Series::flat_i64(p.data(), 4);
        Series cprod = dv::cum_prod(cp);
        CHECK(cprod.data<std::int64_t>()[3] == 24);
        Series ccnt = dv::cum_count(cp);
        CHECK(ccnt.type() == TypeId::Int64);
        CHECK(ccnt.data<std::int64_t>()[3] == 4);

        std::vector<double> q{4.0, 9.0, 16.0};
        Series cq = Series::flat_f64(q.data(), 3);
        Series sq = dv::sqrt(cq);
        CHECK(sq.type() == TypeId::Float64);
        CHECK(sq.data<double>()[1] == doctest::Approx(3.0));
        Series lg = dv::log(dv::exp(cq));  // log(exp(x)) == x
        CHECK(lg.data<double>()[2] == doctest::Approx(16.0));

        // Exercise the vectorized null-free diff / pct_change paths (>64 rows).
        std::vector<std::int64_t> big(100);
        for (std::size_t i = 0; i < big.size(); ++i)
            big[i] = static_cast<std::int64_t>(i * 3);
        Series bd = dv::diff(Series::flat_i64(big.data(), 100));
        CHECK(bd.is_null(0));
        CHECK(bd.data<std::int64_t>()[1] == 3);
        CHECK(bd.data<std::int64_t>()[99] == 3);

        std::vector<double> bf(100);
        for (std::size_t i = 0; i < bf.size(); ++i)
            bf[i] = static_cast<double>(i + 1);
        Series bp = dv::pct_change(Series::flat_f64(bf.data(), 100));
        CHECK(bp.is_null(0));
        CHECK(bp.data<double>()[1] == doctest::Approx(1.0));  // (2-1)/1
        CHECK(bp.data<double>()[99] ==
              doctest::Approx(1.0 / 99));                     // (100-99)/99
    }

    TEST_CASE("A2 prefix scans (SIMD) match a scalar reference") {
        // 250 rows: multiple full vector blocks (carry propagation) plus a
        // scalar tail. Compare every scan against an independent scalar fold.
        const std::int64_t n = 250;
        std::vector<std::int64_t> vi(n);
        std::vector<double> vf(n);
        for (std::int64_t i = 0; i < n; ++i) {
            vi[i] = (i * 7) % 13 - 6;  // small, mixed sign; sums stay exact
            vf[i] = static_cast<double>((i * 5) % 11) - 5.0 + 0.5;
        }
        Series ci = Series::flat_i64(vi.data(), n);
        Series cf = Series::flat_f64(vf.data(), n);

        Series csum = dv::cumsum(ci), cmax = dv::cummax(ci),
               cmin = dv::cummin(ci);
        std::int64_t racc = 0, rmax = INT64_MIN, rmin = INT64_MAX;
        for (std::int64_t i = 0; i < n; ++i) {
            racc += vi[i];
            rmax = vi[i] > rmax ? vi[i] : rmax;
            rmin = vi[i] < rmin ? vi[i] : rmin;
            CHECK(csum.data<std::int64_t>()[i] == racc);
            CHECK(cmax.data<std::int64_t>()[i] == rmax);
            CHECK(cmin.data<std::int64_t>()[i] == rmin);
        }

        // Integer cumulative product stays small (values in [-6,6], reset via
        // zeros keeps it bounded); compare exactly.
        std::vector<std::int64_t> vp(n);
        for (std::int64_t i = 0; i < n; ++i) vp[i] = (i % 5 == 0) ? 0 : (i % 3);
        Series cprod = dv::cum_prod(Series::flat_i64(vp.data(), n));
        std::int64_t rp = 1;
        for (std::int64_t i = 0; i < n; ++i) {
            rp *= vp[i];
            CHECK(cprod.data<std::int64_t>()[i] == rp);
        }

        // Float cumsum reassociates, so compare with a tolerance.
        Series fsum = dv::cumsum(cf);
        double facc = 0.0;
        for (std::int64_t i = 0; i < n; ++i) {
            facc += vf[i];
            CHECK(fsum.data<double>()[i] == doctest::Approx(facc));
        }
    }

    TEST_CASE("A2 fillna over the vectorized masked-blend path") {
        // >64 rows with scattered nulls: the SIMD blend must match the fill.
        std::vector<std::int64_t> v(100);
        std::vector<std::uint8_t> bm((100 + 7) / 8, 0);
        for (std::size_t i = 0; i < v.size(); ++i) {
            v[i] = static_cast<std::int64_t>(i);
            if (i % 3 != 0)  // rows not divisible by 3 are valid
                bm[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        }
        Series c = Series::flat_i64(v.data(), 100, bm.data());
        Series f = c.fillna(dv::to_scalar<std::int64_t>(-7));
        CHECK(f.null_count() == 0);
        for (std::int64_t i = 0; i < 100; ++i)
            CHECK(f.data<std::int64_t>()[i] == (i % 3 == 0 ? -7 : i));
    }

    TEST_CASE("A2 predicates: is_nan/is_finite/is_infinite") {
        const double inf = std::numeric_limits<double>::infinity();
        const double nan = std::numeric_limits<double>::quiet_NaN();
        std::vector<double> f{1.0, nan, inf, -inf, 2.5};
        Series c = Series::flat_f64(f.data(), 5);

        Series nn = c.is_nan();
        CHECK(nn.type() == TypeId::Bool);
        CHECK(mask_bit(nn, 1));
        CHECK_FALSE(mask_bit(nn, 0));
        CHECK_FALSE(mask_bit(nn, 2));

        Series fin = c.is_finite();
        CHECK(mask_bit(fin, 0));
        CHECK(mask_bit(fin, 4));
        CHECK_FALSE(mask_bit(fin, 1));
        CHECK_FALSE(mask_bit(fin, 2));

        Series infm = c.is_infinite();
        CHECK(mask_bit(infm, 2));
        CHECK(mask_bit(infm, 3));
        CHECK_FALSE(mask_bit(infm, 0));

        // Integer column: never NaN/Inf, always finite.
        std::vector<std::int64_t> iv{1, 2, 3};
        Series ic = Series::flat_i64(iv.data(), 3);
        Series ifin = ic.is_finite();
        for (std::int64_t i = 0; i < 3; ++i) CHECK(mask_bit(ifin, i));
        Series inan = ic.is_nan();
        for (std::int64_t i = 0; i < 3; ++i) CHECK_FALSE(mask_bit(inan, i));
    }

    TEST_CASE("A2 is_unique / is_duplicated") {
        std::vector<std::int64_t> v{3, 1, 3, 2, 1};
        Series c = Series::flat_i64(v.data(), 5);
        Series uq = c.is_unique();
        std::vector<bool> exp_uq{false, false, false, true, false};
        std::vector<bool> exp_dp{true, true, true, false, true};
        for (std::int64_t i = 0; i < 5; ++i) {
            CHECK(mask_bit(uq, i) == exp_uq[static_cast<std::size_t>(i)]);
            CHECK(mask_bit(c.is_duplicated(), i) ==
                  exp_dp[static_cast<std::size_t>(i)]);
        }
    }

    TEST_CASE("A2 is_sorted") {
        std::vector<std::int64_t> asc{1, 2, 2, 3};
        Series ca = Series::flat_i64(asc.data(), 4);
        CHECK(ca.is_sorted());
        CHECK_FALSE(ca.is_sorted(true));

        std::vector<std::int64_t> desc{5, 4, 4, 1};
        Series cd = Series::flat_i64(desc.data(), 4);
        CHECK(cd.is_sorted(true));
        CHECK_FALSE(cd.is_sorted());

        std::vector<std::int64_t> one{7};
        CHECK(Series::flat_i64(one.data(), 1).is_sorted());

        // Exercise the vectorized loop (>64 rows) plus a late inversion in the
        // scalar tail.
        std::vector<std::int64_t> big(200);
        for (std::size_t i = 0; i < big.size(); ++i)
            big[i] = static_cast<std::int64_t>(i);
        CHECK(Series::flat_i64(big.data(), 200).is_sorted());
        big[199] = 0;
        CHECK_FALSE(Series::flat_i64(big.data(), 200).is_sorted());

        // Float: a NaN is never an inversion, matching the scalar rule.
        std::vector<double> f(100);
        for (std::size_t i = 0; i < f.size(); ++i)
            f[i] = static_cast<double>(i);
        f[50] = std::numeric_limits<double>::quiet_NaN();
        CHECK(Series::flat_f64(f.data(), 100).is_sorted());
        f[10] = 999.0;  // real inversion before the NaN
        CHECK_FALSE(Series::flat_f64(f.data(), 100).is_sorted());
    }

    TEST_CASE("A2 drop_nulls") {
        std::vector<std::int64_t> v{9, 8, 7, 6};
        std::uint8_t bitmap = 0b0000'0101;  // rows 0, 2 valid
        Series c = Series::flat_i64(v.data(), 4, &bitmap);
        REQUIRE(c.null_count() == 2);
        Series kept = materialize(c.drop_nulls());
        REQUIRE(kept.length() == 2);
        CHECK(kept.data<std::int64_t>()[0] == 9);
        CHECK(kept.data<std::int64_t>()[1] == 7);
    }

    TEST_CASE("A2 is_in") {
        std::vector<std::int64_t> v{1, 2, 3, 4, 5};
        Series c = Series::flat_i64(v.data(), 5);
        std::vector<std::int64_t> vals{2, 4, 6};
        Series cv = Series::flat_i64(vals.data(), 3);
        Series m = c.is_in(cv);
        std::vector<bool> exp{false, true, false, true, false};
        for (std::int64_t i = 0; i < 5; ++i)
            CHECK(mask_bit(m, i) == exp[static_cast<std::size_t>(i)]);

        // FLAT Float64 + small needle set takes the SIMD broadcast path; check
        // parity against a scalar reference over >64 rows.
        const std::int64_t n = 200;
        std::vector<double> fv(n);
        for (std::int64_t i = 0; i < n; ++i) fv[i] = static_cast<double>(i % 7);
        std::vector<double> needles{1.0, 4.0, 6.0};
        Series fm = Series::flat_f64(fv.data(), n)
                        .is_in(Series::flat_f64(needles.data(), 3));
        for (std::int64_t i = 0; i < n; ++i) {
            const double x = fv[static_cast<std::size_t>(i)];
            const bool want = (x == 1.0 || x == 4.0 || x == 6.0);
            CHECK(mask_bit(fm, i) == want);
        }
    }

    TEST_CASE("A2 sort / head / tail / reverse") {
        std::vector<std::int64_t> v{3, 1, 2};
        Series c = Series::flat_i64(v.data(), 3);

        Series s = c.sort();
        CHECK(s.data<std::int64_t>()[0] == 1);
        CHECK(s.data<std::int64_t>()[2] == 3);
        Series sd = c.sort(true);
        CHECK(sd.data<std::int64_t>()[0] == 3);
        CHECK(sd.data<std::int64_t>()[2] == 1);

        Series h = c.head(2);
        REQUIRE(h.length() == 2);
        CHECK(h.data<std::int64_t>()[0] == 3);
        CHECK(h.data<std::int64_t>()[1] == 1);
        Series t = c.tail(2);
        REQUIRE(t.length() == 2);
        CHECK(t.data<std::int64_t>()[0] == 1);
        CHECK(t.data<std::int64_t>()[1] == 2);
        CHECK(c.head(10).length() == 3);  // clamp

        Series r = c.reverse();
        CHECK(r.data<std::int64_t>()[0] == 2);
        CHECK(r.data<std::int64_t>()[2] == 3);
    }

    TEST_CASE("A2 shift") {
        std::vector<std::int64_t> v{10, 20, 30, 40};
        Series c = Series::flat_i64(v.data(), 4);

        Series lag = c.shift(1);
        REQUIRE(lag.length() == 4);
        CHECK(lag.is_null(0));
        CHECK(lag.data<std::int64_t>()[1] == 10);
        CHECK(lag.data<std::int64_t>()[3] == 30);

        Series lead = c.shift(-1);
        CHECK(lead.data<std::int64_t>()[0] == 20);
        CHECK(lead.data<std::int64_t>()[2] == 40);
        CHECK(lead.is_null(3));
    }

    TEST_CASE("A2 top_k / bottom_k") {
        std::vector<std::int64_t> v{50, 10, 90, 30, 70};
        Series c = Series::flat_i64(v.data(), 5);
        Series tk = c.top_k(2);
        REQUIRE(tk.length() == 2);
        CHECK(tk.data<std::int64_t>()[0] == 90);
        CHECK(tk.data<std::int64_t>()[1] == 70);
        Series bk = c.bottom_k(2);
        REQUIRE(bk.length() == 2);
        CHECK(bk.data<std::int64_t>()[0] == 10);
        CHECK(bk.data<std::int64_t>()[1] == 30);
    }

    TEST_CASE("A2 sample is deterministic") {
        std::vector<std::int64_t> v(10);
        for (std::int64_t i = 0; i < 10; ++i)
            v[static_cast<std::size_t>(i)] = i;
        Series c = Series::flat_i64(v.data(), 10);

        Series s1 = c.sample(4, 42);
        Series s2 = c.sample(4, 42);
        REQUIRE(s1.length() == 4);
        REQUIRE(s2.length() == 4);
        for (std::int64_t i = 0; i < 4; ++i)
            CHECK(s1.data<std::int64_t>()[i] == s2.data<std::int64_t>()[i]);
        // Ascending row order preserved (v[i] == i), so values increase.
        for (std::int64_t i = 1; i < 4; ++i)
            CHECK(s1.data<std::int64_t>()[i] > s1.data<std::int64_t>()[i - 1]);
        CHECK(c.sample(100, 1).length() == 10);  // clamp
    }

    TEST_CASE("from_borrowed shares the buffer and releases exactly once") {
        std::vector<std::int64_t> v{10, 20, 30, 40};
        static int releases = 0;
        releases = 0;
        {
            Series s = Series::from_borrowed(
                TypeId::Int64, v.data(), static_cast<std::int64_t>(v.size()),
                nullptr, [](void*) { ++releases; }, nullptr);
            REQUIRE(s.valid());
            REQUIRE(s.length() == 4);
            // Zero-copy: the column points straight at the caller's buffer.
            CHECK(s.data<std::int64_t>() == v.data());
            CHECK(s.data<std::int64_t>()[2] == 30);
            CHECK(releases == 0);  // still borrowed
        }
        CHECK(releases == 1);      // released once on destruction
    }

    TEST_CASE("from_borrowed takes ownership of a move-in owner") {
        // The owner backs the borrowed buffer; destroying the Series frees it.
        struct Owner {
            std::vector<double> data;
            int* counter;
            Owner(std::vector<double> d, int* c)
                : data(std::move(d)), counter(c) {}
            Owner(Owner&& o) noexcept
                : data(std::move(o.data)), counter(o.counter) {
                o.counter = nullptr;  // only the live owner counts its death
            }
            Owner& operator=(Owner&&) = delete;
            ~Owner() {
                if (counter) ++*counter;
            }
        };
        int destroyed = 0;
        std::vector<double> src{1.5, 2.5, 3.5};
        const double* base = src.data();
        {
            Owner owner{std::move(src), &destroyed};
            const double* data = owner.data.data();
            Series s = Series::from_borrowed(
                TypeId::Float64, data,
                static_cast<std::int64_t>(owner.data.size()), std::move(owner));
            REQUIRE(s.valid());
            CHECK(s.data<double>() == base);  // no copy of the moved buffer
            CHECK(s.data<double>()[1] == 2.5);
            CHECK(destroyed == 0);
        }
        CHECK(destroyed == 1);  // the held owner is destroyed exactly once
    }

    TEST_CASE("from_borrowed rejects a variable-width type") {
        static int releases = 0;
        releases = 0;
        char bytes[4] = {0};
        Series s = Series::from_borrowed(
            TypeId::String, bytes, 1, nullptr, [](void*) { ++releases; },
            nullptr);
        CHECK_FALSE(s.valid());
        CHECK(releases == 1);  // rejection still runs release once
    }
}

#ifdef DFTRACER_UTILS_ENABLE_ARROW
using dftracer::utils::dataframe::from_arrow;
using dftracer::utils::dataframe::to_arrow;

TEST_SUITE("vec_arrow") {
    TEST_CASE("flat int64 round-trips through Arrow zero-copy") {
        std::vector<std::int64_t> a{10, 20, 30};
        Series col = Series::flat_i64(a.data(), 3);

        ArrowSchema schema;
        ArrowArray array;
        to_arrow(col, &schema, &array);
        CHECK(std::strcmp(schema.format, "l") == 0);
        REQUIRE(array.length == 3);
        REQUIRE(array.n_buffers == 2);
        // Exported buffer aliases the source (zero copy).
        const std::int64_t* raw =
            static_cast<const std::int64_t*>(array.buffers[1]);
        CHECK(raw[0] == 10);
        CHECK(raw[2] == 30);

        Series back = from_arrow(&schema, &array);
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
        std::vector<std::int64_t> v{5, 1, 9, 3, 7};
        Series col = Series::flat_i64(v.data(), 5);
        Series sel = filter_gt(col, 4);  // indices 0,2,4 -> values 5,9,7

        ArrowSchema schema;
        ArrowArray array;
        to_arrow(sel, &schema, &array);

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
        Series s = Series::strings({"foo", "bar", "bazz"});

        ArrowSchema schema;
        ArrowArray array;
        to_arrow(s, &schema, &array);
        CHECK(std::strcmp(schema.format, "u") == 0);  // utf8
        REQUIRE(array.n_buffers == 3);
        REQUIRE(array.length == 3);

        Series back = from_arrow(&schema, &array);
        REQUIRE(back.valid());
        CHECK(back.type() == TypeId::String);
        REQUIRE(back.length() == 3);
        CHECK(back.string_at(0) == "foo");
        CHECK(back.string_at(1) == "bar");
        CHECK(back.string_at(2) == "bazz");

        schema.release(&schema);
    }

    TEST_CASE("dictionary string exports as an Arrow dictionary of strings") {
        Series d = dictionary_encode(Series::strings({"x", "y", "x", "z"}));

        ArrowSchema schema;
        ArrowArray array;
        to_arrow(d, &schema, &array);
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
        std::vector<double> lo{0.0, 1.0, 5.0};
        std::vector<double> hi{1.0, 2.0, 6.0};
        std::vector<std::int64_t> cnt{10, 20, 30};
        std::vector<Series> fields;
        fields.push_back(Series::flat_f64(lo.data(), 3));
        fields.push_back(Series::flat_f64(hi.data(), 3));
        fields.push_back(Series::flat_i64(cnt.data(), 3));
        Series hist = Series::list(
            std::vector<std::int32_t>{0, 2, 3},
            Series::structs({"lo", "hi", "count"}, std::move(fields)));

        ArrowSchema schema;
        ArrowArray array;
        to_arrow(hist, &schema, &array);

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

    // filter/take return a zero-copy SELECTION over a base, which carries no
    // value buffer of its own. Reading it through string_at used to segfault.
    TEST_CASE("string_at on a non-FLAT column is empty, not a crash") {
        Series s = Series::strings({"alpha", "beta", "gamma"});
        // Bool is bit-packed: keep rows 0 and 2.
        std::vector<std::uint8_t> keep{0b0000'0101};
        Series mask = Series::flat(TypeId::Bool, keep.data(), 3);
        Series sel = s.filter(mask);

        REQUIRE(sel.valid());
        if (!sel.is_flat()) {
            for (std::int64_t i = 0; i < sel.length(); ++i)
                CHECK(sel.string_at(i).empty());
        }

        Series flat = sel.materialize();
        REQUIRE(flat.is_flat());
        REQUIRE(flat.length() == 2);
        CHECK(flat.string_at(0) == "alpha");
        CHECK(flat.string_at(1) == "gamma");
    }

    TEST_CASE("bool round-trips through Arrow (bit-packed)") {
        std::vector<std::int64_t> v{5, 1, 9, 3, 7};
        Series b = gt(Series::flat_i64(v.data(), 5), 4);

        ArrowSchema schema;
        ArrowArray array;
        to_arrow(b, &schema, &array);
        CHECK(std::strcmp(schema.format, "b") == 0);

        Series back = from_arrow(&schema, &array);
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
#endif  // DFTRACER_UTILS_ENABLE_ARROW

TEST_CASE("slicing a Bool column respects bit packing") {
    // Bool data is bit-packed ((n+7)/8 bytes) while byte_width(Bool) is 1, so
    // a byte-stride slice both read the wrong bits and ran past the end of the
    // buffer.
    namespace df = dftracer::utils::dataframe;
    std::vector<bool> vals;
    for (int i = 0; i < 40; ++i) vals.push_back(i % 3 == 0);

    std::vector<std::uint8_t> packed((vals.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < vals.size(); ++i)
        if (vals[i]) packed[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));

    df::Series col{dftu_series_new_flat(DFTU_TYPE_BOOL, packed.data(),
                                        static_cast<std::int64_t>(vals.size()),
                                        nullptr)};
    REQUIRE(col.valid());

    for (std::int64_t off : {std::int64_t{0}, std::int64_t{1}, std::int64_t{7},
                             std::int64_t{8}, std::int64_t{13}}) {
        const std::int64_t n = 11;
        df::Series s{dftu_series_slice(col.handle(), off, n)};
        REQUIRE(s.valid());
        REQUIRE(s.length() == n);
        const auto* bits =
            static_cast<const std::uint8_t*>(dftu_series_data(s.handle()));
        for (std::int64_t i = 0; i < n; ++i) {
            const bool got = ((bits[i >> 3] >> (i & 7)) & 1u) != 0;
            CHECK(got == vals[static_cast<std::size_t>(off + i)]);
        }
    }
}

TEST_CASE("Series::data_type() round-trips a List<Int64> column") {
    namespace df = dftracer::utils::dataframe;
    std::vector<std::int64_t> items{1, 2, 3, 4};
    std::vector<std::int32_t> offsets{0, 2, 4};
    df::Series list_col =
        df::Series::list(offsets, df::Series::flat_i64(items.data(), 4));
    REQUIRE(list_col.valid());

    df::DataType dt = list_col.data_type();
    CHECK(dt.id == df::TypeId::List);
    REQUIRE(dt.fields.size() == 1);
    CHECK(dt.fields[0].type.id == df::TypeId::Int64);
    CHECK(dt.fields[0].type.fields.empty());
}

TEST_CASE("Series::data_type() round-trips a Struct{a:Int64,b:String}") {
    namespace df = dftracer::utils::dataframe;
    std::vector<std::int64_t> a{1, 2, 3};
    std::vector<Series> fields;
    fields.push_back(df::Series::flat_i64(a.data(), 3));
    fields.push_back(df::Series::strings({"x", "y", "z"}));
    df::Series st = df::Series::structs({"a", "b"}, std::move(fields));
    REQUIRE(st.valid());

    df::DataType dt = st.data_type();
    CHECK(dt.id == df::TypeId::Struct);
    REQUIRE(dt.fields.size() == 2);
    CHECK(dt.fields[0].name == "a");
    CHECK(dt.fields[0].type.id == df::TypeId::Int64);
    CHECK(dt.fields[1].name == "b");
    CHECK(dt.fields[1].type.id == df::TypeId::String);
}

TEST_CASE("List<Int64> and List<String> report different DataTypes") {
    namespace df = dftracer::utils::dataframe;
    std::vector<std::int32_t> offsets{0, 2};
    std::vector<std::int64_t> ints{1, 2};
    df::Series int_list =
        df::Series::list(offsets, df::Series::flat_i64(ints.data(), 2));
    df::Series str_list =
        df::Series::list(offsets, df::Series::strings({"a", "b"}));

    CHECK(int_list.data_type() != str_list.data_type());
    CHECK(int_list.data_type() == df::list_of(df::scalar(df::TypeId::Int64)));
    CHECK(str_list.data_type() == df::list_of(df::scalar(df::TypeId::String)));
}

TEST_CASE(
    "dftu_series_field_name reports a struct's field names, NULL out of "
    "range") {
    namespace df = dftracer::utils::dataframe;
    std::vector<std::int64_t> a{1};
    std::vector<double> b{2.0};
    std::vector<Series> fields;
    fields.push_back(df::Series::flat_i64(a.data(), 1));
    fields.push_back(df::Series::flat_f64(b.data(), 1));
    df::Series st = df::Series::structs({"first", "second"}, std::move(fields));
    REQUIRE(st.valid());

    const char* n0 = dftu_series_field_name(st.handle(), 0);
    const char* n1 = dftu_series_field_name(st.handle(), 1);
    REQUIRE(n0 != nullptr);
    REQUIRE(n1 != nullptr);
    CHECK(std::string(n0) == "first");
    CHECK(std::string(n1) == "second");
    CHECK(dftu_series_field_name(st.handle(), 2) == nullptr);
    CHECK(dftu_series_field_name(st.handle(), -1) == nullptr);
}
