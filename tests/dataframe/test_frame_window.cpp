#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/frame_ops.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace df = dftracer::utils::dataframe;
using df::DataFrame;
using df::Series;
using df::TypeId;
using df::WINDOW_UNBOUNDED;
using df::WindowColumn;
using df::WindowFrameMode;
using df::WindowFunc;

namespace {

using Opt = std::optional<std::int64_t>;
using Col = std::vector<Opt>;
using SCol = std::vector<std::optional<std::string>>;
using Val = std::optional<double>;
using Vals = std::vector<Val>;

std::vector<std::uint8_t> bits_of(const std::vector<bool>& ok) {
    std::vector<std::uint8_t> bits((ok.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < ok.size(); ++i)
        if (ok[i]) bits[i >> 3] |= static_cast<std::uint8_t>(1U << (i & 7));
    return bits;
}

Series i64s(const std::vector<std::int64_t>& data,
            const std::vector<bool>& ok) {
    const std::vector<std::uint8_t> bits = bits_of(ok);
    return Series::flat_i64(data.data(), static_cast<std::int64_t>(data.size()),
                            bits.data());
}

Series f64s(const std::vector<double>& data, const std::vector<bool>& ok) {
    const std::vector<std::uint8_t> bits = bits_of(ok);
    return Series::flat_f64(data.data(), static_cast<std::int64_t>(data.size()),
                            bits.data());
}

Series i64s(const Col& v) {
    std::vector<std::int64_t> data(v.size(), 0);
    std::vector<bool> ok(v.size(), false);
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (!v[i]) continue;
        data[i] = *v[i];
        ok[i] = true;
    }
    return i64s(data, ok);
}

Series plain(const std::vector<std::int64_t>& v) {
    return Series::flat_i64(v.data(), static_cast<std::int64_t>(v.size()));
}

DataFrame make3(const std::vector<std::int64_t>& p,
                const std::vector<std::int64_t>& o, const Col& v) {
    DataFrame d;
    d.names = {"p", "o", "v"};
    d.columns.push_back(plain(p));
    d.columns.push_back(plain(o));
    d.columns.push_back(i64s(v));
    return d;
}

DataFrame make3u(const std::vector<std::int64_t>& p,
                 const std::vector<std::int64_t>& o,
                 const std::vector<std::uint64_t>& v) {
    DataFrame d;
    d.names = {"p", "o", "v"};
    d.columns.push_back(plain(p));
    d.columns.push_back(plain(o));
    d.columns.push_back(Series::flat(TypeId::Uint64, v.data(),
                                     static_cast<std::int64_t>(v.size())));
    return d;
}

Series tag_lists() {
    return Series::list(
        {0, 1, 3}, Series::strings(std::vector<std::string>{"r", "p", "q"}));
}

DataFrame win(const DataFrame& in, const std::vector<std::string>& part,
              const std::vector<std::string>& order,
              const std::vector<WindowColumn>& specs) {
    return df::window(in, part, order, specs);
}

WindowColumn spec(WindowFunc func, std::optional<std::string> value,
                  std::string out) {
    WindowColumn w{};
    w.func = func;
    if (value) w.set_value(std::move(*value));
    w.out = std::move(out);
    return w;
}

WindowColumn offset_spec(WindowFunc func, std::optional<std::string> value,
                         std::string out, std::int64_t offset) {
    WindowColumn w = spec(func, std::move(value), std::move(out));
    w.params.offset = offset;
    return w;
}

WindowColumn frame_spec(WindowFunc func, std::string value, std::string out,
                        std::int64_t preceding, std::int64_t following,
                        std::int64_t min_count = 0,
                        WindowFrameMode mode = WindowFrameMode::Rows) {
    WindowColumn w = spec(func, std::move(value), std::move(out));
    w.params.frame = {min_count, preceding, following, mode, 0.0};
    return w;
}

WindowColumn rate_spec(std::string value, std::string time, std::string out,
                       bool counter) {
    WindowColumn w = spec(WindowFunc::Rate, std::move(value), std::move(out));
    w.set_time(std::move(time));
    w.params.rate = {counter};
    return w;
}

WindowColumn session_spec(std::string time, std::string out, double gap,
                          double span = 0.0,
                          std::optional<std::string> end = std::nullopt) {
    WindowColumn w = spec(WindowFunc::Sessionize, std::nullopt, std::move(out));
    w.set_time(std::move(time));
    if (end) w.set_end(std::move(*end));
    w.params.session = {gap, span};
    return w;
}

Series column_of(const DataFrame& d, const std::string& name) {
    const Series c = d.column(name);
    REQUIRE(c.valid());
    return c.materialize();
}

Col ints(const DataFrame& d, const std::string& name) {
    const Series c = column_of(d, name);
    REQUIRE(c.type() == TypeId::Int64);
    Col out;
    for (std::int64_t i = 0; i < c.length(); ++i)
        out.push_back(c.is_null(i) ? Opt{} : Opt{c.data<std::int64_t>()[i]});
    return out;
}

Vals nums(const DataFrame& d, const std::string& name) {
    const Series c = column_of(d, name);
    const TypeId t = c.type();
    REQUIRE((t == TypeId::Int64 || t == TypeId::Float64));
    Vals out;
    for (std::int64_t i = 0; i < c.length(); ++i) {
        if (c.is_null(i))
            out.emplace_back();
        else if (t == TypeId::Int64)
            out.emplace_back(static_cast<double>(c.data<std::int64_t>()[i]));
        else
            out.emplace_back(c.data<double>()[i]);
    }
    return out;
}

Vals dbls(const DataFrame& d, const std::string& name) {
    REQUIRE(column_of(d, name).type() == TypeId::Float64);
    return nums(d, name);
}

SCol strs(const DataFrame& d, const std::string& name) {
    const Series c = column_of(d, name);
    REQUIRE(c.type() == TypeId::String);
    SCol out;
    for (std::int64_t i = 0; i < c.length(); ++i) {
        if (c.is_null(i))
            out.emplace_back();
        else
            out.emplace_back(std::string(c.string_at(i)));
    }
    return out;
}

std::vector<std::string> str_list(const DataFrame& d, const std::string& name,
                                  std::int64_t row) {
    const Series c = column_of(d, name);
    REQUIRE(c.type() == TypeId::List);
    const std::int32_t* off = c.offsets();
    REQUIRE(off != nullptr);
    const Series elems = c.child(0).materialize();
    std::vector<std::string> out;
    for (std::int32_t at = off[row]; at < off[row + 1]; ++at)
        out.emplace_back(elems.string_at(at));
    return out;
}

void check_dbls(const Vals& got, const std::vector<double>& want) {
    REQUIRE(got.size() == want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        CAPTURE(i);
        REQUIRE(got[i].has_value());
        CHECK(*got[i] == doctest::Approx(want[i]));
    }
}

}  // namespace

TEST_CASE("window - row_number resets per partition") {
    const DataFrame in =
        make3({1, 1, 1, 2, 2}, {10, 20, 30, 10, 20}, {{}, {}, {}, {}, {}});
    const DataFrame out = win(
        in, {"p"}, {"o"}, {spec(WindowFunc::RowNumber, std::nullopt, "rn")});
    REQUIRE(out.num_columns() == 4);
    CHECK(out.names[3] == "rn");
    REQUIRE(out.num_rows() == 5);
    CHECK(ints(out, "rn") == Col{1, 2, 3, 1, 2});
}

TEST_CASE("window - rank vs dense_rank on ties") {
    const DataFrame in = make3({1, 1, 1}, {10, 10, 20}, {{}, {}, {}});
    const DataFrame out =
        win(in, {"p"}, {"o"},
            {spec(WindowFunc::Rank, std::nullopt, "rk"),
             spec(WindowFunc::DenseRank, std::nullopt, "dr")});
    REQUIRE(out.num_rows() == 3);
    CHECK(ints(out, "rk") == Col{1, 1, 3});
    CHECK(ints(out, "dr") == Col{1, 1, 2});
}

TEST_CASE("window - lag/lead boundary null and interior shift") {
    const DataFrame in = make3({1, 1, 1}, {1, 2, 3}, {10, 20, 30});
    const DataFrame out = win(in, {"p"}, {"o"},
                              {offset_spec(WindowFunc::Lag, "v", "lag", 1),
                               offset_spec(WindowFunc::Lead, "v", "lead", 1)});
    REQUIRE(out.num_rows() == 3);
    CHECK(ints(out, "lag") == Col{std::nullopt, 10, 20});
    CHECK(ints(out, "lead") == Col{20, 30, std::nullopt});
}

TEST_CASE("window - running sum resets per partition") {
    const DataFrame in = make3({1, 1, 2}, {1, 2, 1}, {10, 20, 100});
    const DataFrame out =
        win(in, {"p"}, {"o"}, {spec(WindowFunc::RunningSum, "v", "rs")});
    REQUIRE(out.num_rows() == 3);
    CHECK(ints(out, "rs") == Col{10, 30, 100});
}

TEST_CASE("window - running min/max/count") {
    const DataFrame in = make3({1, 1, 1}, {1, 2, 3}, {30, 10, 20});
    const DataFrame out =
        win(in, {"p"}, {"o"},
            {spec(WindowFunc::RunningMin, "v", "mn"),
             spec(WindowFunc::RunningMax, "v", "mx"),
             spec(WindowFunc::RunningCount, std::nullopt, "cnt")});
    REQUIRE(out.num_rows() == 3);
    CHECK(ints(out, "mn") == Col{30, 10, 10});
    CHECK(ints(out, "mx") == Col{30, 30, 30});
    CHECK(ints(out, "cnt") == Col{1, 2, 3});
}

TEST_CASE("window - running sum skips null cell, count is count(*)") {
    const DataFrame in = make3({1, 1, 1}, {1, 2, 3}, {10, {}, 20});
    const DataFrame out =
        win(in, {"p"}, {"o"},
            {spec(WindowFunc::RunningSum, "v", "rs"),
             spec(WindowFunc::RunningCount, std::nullopt, "cnt")});
    REQUIRE(out.num_rows() == 3);
    CHECK(ints(out, "rs") == Col{10, 10, 30});
    CHECK(ints(out, "cnt") == Col{1, 2, 3});
}

TEST_CASE("window - partition values do not leak across boundaries") {
    const DataFrame in = make3({1, 2, 2}, {1, 1, 2}, {999, 5, 7});
    const DataFrame out =
        win(in, {"p"}, {"o"},
            {spec(WindowFunc::RunningMax, "v", "mx"),
             spec(WindowFunc::RowNumber, std::nullopt, "rn")});
    REQUIRE(out.num_rows() == 3);
    CHECK(ints(out, "mx") == Col{999, 5, 7});
    CHECK(ints(out, "rn") == Col{1, 1, 2});
}

TEST_CASE("window - determinism across input order") {
    const DataFrame a = make3({1, 1, 2}, {10, 20, 5}, {1, 2, 3});
    const DataFrame b = make3({2, 1, 1}, {5, 20, 10}, {3, 2, 1});
    const std::vector<WindowColumn> specs = {
        spec(WindowFunc::RowNumber, std::nullopt, "rn")};
    const DataFrame oa = win(a, {"p"}, {"o"}, specs);
    const DataFrame ob = win(b, {"p"}, {"o"}, specs);
    REQUIRE(oa.num_rows() == 3);
    REQUIRE(ob.num_rows() == 3);
    for (const char* name : {"p", "o", "v", "rn"})
        CHECK(ints(oa, name) == ints(ob, name));
}

TEST_CASE("window - multiple specs appended in order") {
    const DataFrame in = make3({1, 1, 1}, {1, 2, 3}, {10, 20, 30});
    const DataFrame out = win(in, {"p"}, {"o"},
                              {spec(WindowFunc::RowNumber, std::nullopt, "rn"),
                               spec(WindowFunc::RunningSum, "v", "rs"),
                               offset_spec(WindowFunc::Lag, "v", "lag", 1)});
    REQUIRE(out.num_columns() == 6);
    CHECK(out.names[3] == "rn");
    CHECK(out.names[4] == "rs");
    CHECK(out.names[5] == "lag");
    REQUIRE(out.num_rows() == 3);
    CHECK(ints(out, "rn") == Col{1, 2, 3});
    CHECK(ints(out, "rs") == Col{10, 30, 60});
    CHECK(ints(out, "lag") == Col{std::nullopt, 10, 20});
}

TEST_CASE("window - list<utf8> carry-through survives") {
    DataFrame in;
    in.names = {"p", "o", "tags"};
    in.columns.push_back(plain({1, 1}));
    in.columns.push_back(plain({20, 10}));
    in.columns.push_back(tag_lists());
    const DataFrame out = win(
        in, {"p"}, {"o"}, {spec(WindowFunc::RowNumber, std::nullopt, "rn")});
    REQUIRE(out.num_rows() == 2);
    CHECK(str_list(out, "tags", 0) == std::vector<std::string>{"p", "q"});
    CHECK(str_list(out, "tags", 1) == std::vector<std::string>{"r"});
    CHECK(ints(out, "rn") == Col{1, 2});
}

TEST_CASE("window - running sum over double column yields double") {
    const std::vector<double> v = {1.5, 2.25};
    DataFrame in;
    in.names = {"o", "v"};
    in.columns.push_back(plain({1, 2}));
    in.columns.push_back(Series::flat_f64(v.data(), 2));
    const DataFrame out =
        win(in, {}, {"o"}, {spec(WindowFunc::RunningSum, "v", "rs")});
    REQUIRE(out.num_rows() == 2);
    check_dbls(dbls(out, "rs"), {1.5, 3.75});
}

TEST_CASE("window - delta discrete difference, first row null") {
    const DataFrame in = make3({1, 1, 1}, {1, 2, 3}, {10, 15, 13});
    const DataFrame out =
        win(in, {"p"}, {"o"}, {spec(WindowFunc::Delta, "v", "d")});
    REQUIRE(out.num_columns() == 4);
    CHECK(out.names[3] == "d");
    REQUIRE(out.num_rows() == 3);
    CHECK(ints(out, "d") == Col{std::nullopt, 5, -2});
}

TEST_CASE("window - delta on unsigned column yields signed negative") {
    const DataFrame in = make3u({1, 1, 1}, {1, 2, 3}, {100, 30, 45});
    const DataFrame out =
        win(in, {"p"}, {"o"}, {spec(WindowFunc::Delta, "v", "d")});
    REQUIRE(out.num_columns() == 4);
    CHECK(out.columns[3].type() == TypeId::Int64);
    REQUIRE(out.num_rows() == 3);
    CHECK(ints(out, "d") == Col{std::nullopt, -70, 15});
}

TEST_CASE("window - rate on a decreasing unsigned column is correct") {
    const DataFrame in = make3u({1, 1}, {0, 10}, {100, 30});
    const DataFrame out =
        win(in, {"p"}, {"o"}, {rate_spec("v", "o", "rate", false)});
    REQUIRE(out.num_rows() == 2);
    const Vals r = dbls(out, "rate");
    CHECK_FALSE(r[0].has_value());
    REQUIRE(r[1].has_value());
    CHECK(*r[1] == doctest::Approx(-7.0));
}

TEST_CASE("window - delta resets per partition and nulls on null endpoint") {
    const DataFrame in =
        make3({1, 1, 1, 2, 2}, {1, 2, 3, 1, 2}, {10, {}, 20, 5, 7});
    const DataFrame out =
        win(in, {"p"}, {"o"}, {spec(WindowFunc::Delta, "v", "d")});
    REQUIRE(out.num_rows() == 5);
    CHECK(ints(out, "d") ==
          Col{std::nullopt, std::nullopt, std::nullopt, std::nullopt, 2});
}

TEST_CASE("window - rate over time, zero dt yields null") {
    const DataFrame in = make3({1, 1, 1}, {0, 10, 10}, {100, 150, 150});
    const DataFrame out =
        win(in, {"p"}, {"o"}, {rate_spec("v", "o", "rate", false)});
    REQUIRE(out.num_columns() == 4);
    REQUIRE(out.num_rows() == 3);
    const Vals r = dbls(out, "rate");
    CHECK_FALSE(r[0].has_value());
    REQUIRE(r[1].has_value());
    CHECK(*r[1] == doctest::Approx(5.0));
    CHECK_FALSE(r[2].has_value());
}

TEST_CASE("window - rate counter reset uses raw value, resets per partition") {
    const DataFrame in = make3({1, 1, 2, 2}, {0, 10, 0, 10}, {100, 30, 5, 5});
    const DataFrame out =
        win(in, {"p"}, {"o"}, {rate_spec("v", "o", "rate", true)});
    REQUIRE(out.num_rows() == 4);
    const Vals r = dbls(out, "rate");
    CHECK_FALSE(r[0].has_value());
    REQUIRE(r[1].has_value());
    CHECK(*r[1] == doctest::Approx(3.0));
    CHECK_FALSE(r[2].has_value());
    REQUIRE(r[3].has_value());
    CHECK(*r[3] == doctest::Approx(0.0));
}

TEST_CASE("window - sessionize splits on gap strictly greater than threshold") {
    const DataFrame in = make3({1, 1, 1, 1}, {0, 5, 100, 105}, {0, 0, 0, 0});
    const DataFrame out =
        win(in, {"p"}, {"o"}, {session_spec("o", "sid", 50.0)});
    REQUIRE(out.num_columns() == 4);
    REQUIRE(out.num_rows() == 4);
    CHECK(ints(out, "sid") == Col{1, 1, 2, 2});
}

TEST_CASE("window - sessionize gap equal to threshold does not split") {
    const DataFrame in = make3({1, 1, 2, 2}, {0, 50, 0, 200}, {0, 0, 0, 0});
    const DataFrame out =
        win(in, {"p"}, {"o"}, {session_spec("o", "sid", 50.0)});
    REQUIRE(out.num_rows() == 4);
    CHECK(ints(out, "sid") == Col{1, 1, 1, 2});
}

TEST_CASE("window - sessionize measures the gap from the latest end") {
    const DataFrame in = make3({1, 1}, {0, 150}, {120, 155});
    const DataFrame ends =
        win(in, {"p"}, {"o"}, {session_spec("o", "sid", 50.0, 0.0, "v")});
    CHECK(ints(ends, "sid") == Col{1, 1});
    const DataFrame starts =
        win(in, {"p"}, {"o"}, {session_spec("o", "sid", 50.0)});
    CHECK(ints(starts, "sid") == Col{1, 2});
}

TEST_CASE("window - sessionize gap between starts without an end column") {
    const DataFrame in = make3({1, 1, 1}, {0, 10, 25}, {0, 0, 0});
    const DataFrame out =
        win(in, {"p"}, {"o"}, {session_spec("o", "sid", 15.0)});
    CHECK(ints(out, "sid") == Col{1, 1, 1});
}

TEST_CASE("window - sessionize splits a session longer than the span") {
    const DataFrame in = make3({1, 1, 1, 1}, {0, 50, 100, 150}, {0, 0, 0, 0});
    const DataFrame out =
        win(in, {"p"}, {"o"}, {session_spec("o", "sid", 60.0, 120.0)});
    CHECK(ints(out, "sid") == Col{1, 1, 1, 2});
}

TEST_CASE("window - sessionize gives a null time a null session") {
    const DataFrame in = make3({1, 1, 1}, {0, 1, 2}, {0, std::nullopt, 300});
    const DataFrame out =
        win(in, {"p"}, {"o"}, {session_spec("v", "sid", 100.0)});
    CHECK(ints(out, "sid") == Col{1, std::nullopt, 2});
}

TEST_CASE("window - sessionize rejects a negative gap and a text end") {
    const DataFrame in = make3({1, 1}, {0, 10}, {0, 0});
    CHECK_THROWS_AS(win(in, {"p"}, {"o"}, {session_spec("o", "sid", -1.0)}),
                    std::invalid_argument);
    CHECK_THROWS_AS(
        win(in, {"p"}, {"o"}, {session_spec("o", "sid", 1.0, -2.0)}),
        std::invalid_argument);
    DataFrame text;
    text.names = {"p", "o", "e"};
    text.columns.push_back(plain({1}));
    text.columns.push_back(plain({0}));
    text.columns.push_back(Series::strings(std::vector<std::string>{"x"}));
    CHECK_THROWS_AS(
        win(text, {"p"}, {"o"}, {session_spec("o", "sid", 1.0, 0.0, "e")}),
        std::invalid_argument);
}

TEST_CASE("window - delta determinism across input order") {
    const DataFrame a = make3({1, 1, 2}, {1, 2, 1}, {10, 15, 100});
    const DataFrame b = make3({2, 1, 1}, {1, 1, 2}, {100, 10, 15});
    const std::vector<WindowColumn> specs = {spec(WindowFunc::Delta, "v", "d")};
    const DataFrame oa = win(a, {"p"}, {"o"}, specs);
    const DataFrame ob = win(b, {"p"}, {"o"}, specs);
    REQUIRE(oa.num_rows() == 3);
    REQUIRE(ob.num_rows() == 3);
    CHECK(ints(oa, "d") == ints(ob, "d"));
}

TEST_CASE("window - delta carries a list<utf8> column through") {
    DataFrame in;
    in.names = {"p", "o", "v", "tags"};
    in.columns.push_back(plain({1, 1}));
    in.columns.push_back(plain({20, 10}));
    in.columns.push_back(plain({15, 10}));
    in.columns.push_back(tag_lists());
    const DataFrame out =
        win(in, {"p"}, {"o"}, {spec(WindowFunc::Delta, "v", "d")});
    REQUIRE(out.num_rows() == 2);
    CHECK(str_list(out, "tags", 0) == std::vector<std::string>{"p", "q"});
    CHECK(str_list(out, "tags", 1) == std::vector<std::string>{"r"});
    CHECK(ints(out, "d") == Col{std::nullopt, 5});
}

TEST_CASE("window - frame_sum rows between 1 preceding and 1 following") {
    const DataFrame in = make3({1, 1, 1, 1}, {1, 2, 3, 4}, {1, 2, 3, 4});
    const DataFrame out = win(
        in, {"p"}, {"o"}, {frame_spec(WindowFunc::FrameSum, "v", "fs", 1, 1)});
    REQUIRE(out.num_columns() == 4);
    REQUIRE(out.num_rows() == 4);
    CHECK(ints(out, "fs") == Col{3, 6, 9, 7});
}

TEST_CASE("window - frame_min/max/mean over 1 preceding and 1 following") {
    const DataFrame in = make3({1, 1, 1, 1}, {1, 2, 3, 4}, {1, 2, 3, 4});
    const DataFrame out =
        win(in, {"p"}, {"o"},
            {frame_spec(WindowFunc::FrameMin, "v", "mn", 1, 1),
             frame_spec(WindowFunc::FrameMax, "v", "mx", 1, 1),
             frame_spec(WindowFunc::FrameMean, "v", "me", 1, 1)});
    REQUIRE(out.num_rows() == 4);
    CHECK(ints(out, "mn") == Col{1, 1, 2, 3});
    CHECK(ints(out, "mx") == Col{2, 3, 4, 4});
    check_dbls(dbls(out, "me"), {1.5, 2.0, 3.0, 3.5});
}

TEST_CASE("window - frame_count skips null and frame_sum skips null in frame") {
    const DataFrame in = make3({1, 1, 1, 1}, {1, 2, 3, 4}, {1, {}, 3, 4});
    const DataFrame out =
        win(in, {"p"}, {"o"},
            {frame_spec(WindowFunc::FrameCount, "v", "fc", 1, 1),
             frame_spec(WindowFunc::FrameSum, "v", "fs", 1, 1)});
    REQUIRE(out.num_rows() == 4);
    CHECK(ints(out, "fc") == Col{1, 2, 2, 2});
    CHECK(ints(out, "fs") == Col{1, 4, 7, 7});
}

TEST_CASE("window - frame_sum all-null frame yields null") {
    const DataFrame in = make3({1, 1, 1}, {1, 2, 3}, {{}, {}, {}});
    const DataFrame out = win(
        in, {"p"}, {"o"}, {frame_spec(WindowFunc::FrameSum, "v", "fs", 1, 1)});
    REQUIRE(out.num_rows() == 3);
    CHECK(ints(out, "fs") == Col{std::nullopt, std::nullopt, std::nullopt});
}

TEST_CASE("window - frame_sum resets per partition") {
    const DataFrame in = make3({1, 1, 2, 2}, {1, 2, 1, 2}, {1, 2, 10, 20});
    const DataFrame out = win(
        in, {"p"}, {"o"}, {frame_spec(WindowFunc::FrameSum, "v", "fs", 1, 1)});
    REQUIRE(out.num_rows() == 4);
    CHECK(ints(out, "fs") == Col{3, 3, 30, 30});
}

TEST_CASE("window - unbounded preceding frame_sum equals running_sum") {
    const DataFrame in = make3({1, 1, 1, 2}, {1, 2, 3, 1}, {10, 20, 30, 100});
    const DataFrame out =
        win(in, {"p"}, {"o"},
            {spec(WindowFunc::RunningSum, "v", "rs"),
             frame_spec(WindowFunc::FrameSum, "v", "fs", WINDOW_UNBOUNDED, 0)});
    REQUIRE(out.num_rows() == 4);
    CHECK(ints(out, "rs") == ints(out, "fs"));
    CHECK(ints(out, "fs") == Col{10, 30, 60, 100});
}

TEST_CASE("window - ntile splits partition larger buckets first") {
    const DataFrame a = make3({1, 1, 1, 1}, {1, 2, 3, 4}, {0, 0, 0, 0});
    const DataFrame oa =
        win(a, {"p"}, {"o"},
            {offset_spec(WindowFunc::Ntile, std::nullopt, "nt", 2)});
    REQUIRE(oa.num_rows() == 4);
    CHECK(ints(oa, "nt") == Col{1, 1, 2, 2});

    const DataFrame b =
        make3({1, 1, 1, 1, 1}, {1, 2, 3, 4, 5}, {0, 0, 0, 0, 0});
    const DataFrame ob =
        win(b, {"p"}, {"o"},
            {offset_spec(WindowFunc::Ntile, std::nullopt, "nt", 3)});
    REQUIRE(ob.num_rows() == 5);
    CHECK(ints(ob, "nt") == Col{1, 1, 2, 2, 3});
}

TEST_CASE("window - first/last/nth value over partition") {
    const DataFrame in =
        make3({1, 1, 1, 2, 2}, {1, 2, 3, 1, 2}, {10, 20, 30, 40, 50});
    const DataFrame out =
        win(in, {"p"}, {"o"},
            {spec(WindowFunc::FirstValue, "v", "fv"),
             spec(WindowFunc::LastValue, "v", "lv"),
             offset_spec(WindowFunc::NthValue, "v", "nth", 2)});
    REQUIRE(out.num_rows() == 5);
    CHECK(ints(out, "fv") == Col{10, 10, 10, 40, 40});
    CHECK(ints(out, "lv") == Col{30, 30, 30, 50, 50});
    CHECK(ints(out, "nth") == Col{20, 20, 20, 50, 50});
}

TEST_CASE("window - nth_value out of range yields null") {
    const DataFrame in = make3({1, 1}, {1, 2}, {10, 20});
    const DataFrame out = win(
        in, {"p"}, {"o"}, {offset_spec(WindowFunc::NthValue, "v", "nth", 3)});
    REQUIRE(out.num_rows() == 2);
    CHECK(out.names[3] == "nth");
    CHECK(ints(out, "nth") == Col{std::nullopt, std::nullopt});
}

TEST_CASE("window - first/last/nth carry a list<utf8> column") {
    DataFrame in;
    in.names = {"p", "o", "tags"};
    in.columns.push_back(plain({1, 1}));
    in.columns.push_back(plain({20, 10}));
    in.columns.push_back(tag_lists());
    const DataFrame out =
        win(in, {"p"}, {"o"},
            {spec(WindowFunc::FirstValue, "tags", "fv"),
             spec(WindowFunc::LastValue, "tags", "lv"),
             offset_spec(WindowFunc::NthValue, "tags", "nth", 2)});
    REQUIRE(out.num_rows() == 2);
    CHECK(str_list(out, "fv", 0) == std::vector<std::string>{"p", "q"});
    CHECK(str_list(out, "lv", 0) == std::vector<std::string>{"r"});
    CHECK(str_list(out, "nth", 0) == std::vector<std::string>{"r"});
}

TEST_CASE(
    "window - String column with a null through positional and extremum") {
    const std::vector<std::string_view> names = {"b", "", "a", "c"};
    const std::vector<std::uint8_t> valid = bits_of({true, false, true, true});
    DataFrame in;
    in.names = {"p", "o", "s"};
    in.columns.push_back(plain({1, 1, 1, 1}));
    in.columns.push_back(plain({4, 3, 2, 1}));
    in.columns.push_back(Series::strings(
        std::span<const std::string_view>(names), valid.data()));
    const DataFrame out =
        win(in, {"p"}, {"o"},
            {offset_spec(WindowFunc::Lag, "s", "lag", 1),
             spec(WindowFunc::FirstValue, "s", "fv"),
             spec(WindowFunc::FillForward, "s", "ff"),
             spec(WindowFunc::RunningMin, "s", "rmin"),
             frame_spec(WindowFunc::FrameMax, "s", "fmax", 1, 1)});
    REQUIRE(out.num_rows() == 4);
    CHECK(strs(out, "s") == SCol{"c", "a", std::nullopt, "b"});
    CHECK(strs(out, "lag") == SCol{std::nullopt, "c", "a", std::nullopt});
    CHECK(strs(out, "fv") == SCol{"c", "c", "c", "c"});
    CHECK(strs(out, "ff") == SCol{"c", "a", "a", "b"});
    CHECK(strs(out, "rmin") == SCol{"c", "a", "a", "a"});
    CHECK(strs(out, "fmax") == SCol{"c", "c", "b", "b"});
    CHECK_THROWS_AS(win(in, {"p"}, {"o"},
                        {frame_spec(WindowFunc::FrameSum, "s", "x", 1, 1)}),
                    std::invalid_argument);
    CHECK_THROWS_AS(
        win(in, {"p"}, {"o"}, {spec(WindowFunc::RunningSum, "missing", "x")}),
        std::out_of_range);
}

TEST_CASE("window - frame determinism across input order") {
    const DataFrame a = make3({1, 1, 1, 2}, {1, 2, 3, 1}, {1, 2, 3, 9});
    const DataFrame b = make3({2, 1, 1, 1}, {1, 3, 1, 2}, {9, 3, 1, 2});
    const std::vector<WindowColumn> specs = {
        frame_spec(WindowFunc::FrameSum, "v", "fs", 1, 1)};
    const DataFrame oa = win(a, {"p"}, {"o"}, specs);
    const DataFrame ob = win(b, {"p"}, {"o"}, specs);
    REQUIRE(oa.num_rows() == 4);
    REQUIRE(ob.num_rows() == 4);
    CHECK(ints(oa, "fs") == ints(ob, "fs"));
}

TEST_CASE("window - wide frame 3 preceding 2 following exercises deque") {
    const DataFrame in =
        make3({1, 1, 1, 1, 1, 1, 1, 1}, {1, 2, 3, 4, 5, 6, 7, 8},
              {5, 5, 3, 1, 2, 2, 4, 6});
    const DataFrame out =
        win(in, {"p"}, {"o"},
            {frame_spec(WindowFunc::FrameSum, "v", "fs", 3, 2),
             frame_spec(WindowFunc::FrameMin, "v", "mn", 3, 2),
             frame_spec(WindowFunc::FrameMax, "v", "mx", 3, 2),
             frame_spec(WindowFunc::FrameCount, "v", "fc", 3, 2),
             frame_spec(WindowFunc::FrameMean, "v", "me", 3, 2)});
    REQUIRE(out.num_rows() == 8);
    const std::vector<std::int64_t> fs = {13, 14, 16, 18, 17, 18, 15, 14};
    const std::vector<std::int64_t> fc = {3, 4, 5, 6, 6, 6, 5, 4};
    CHECK(ints(out, "fs") == Col(fs.begin(), fs.end()));
    CHECK(ints(out, "mn") == Col{3, 1, 1, 1, 1, 1, 1, 2});
    CHECK(ints(out, "mx") == Col{5, 5, 5, 5, 5, 6, 6, 6});
    CHECK(ints(out, "fc") == Col(fc.begin(), fc.end()));
    std::vector<double> me;
    for (std::size_t i = 0; i < fs.size(); ++i)
        me.push_back(static_cast<double>(fs[i]) / static_cast<double>(fc[i]));
    check_dbls(dbls(out, "me"), me);
}

TEST_CASE("window - frame slides across a null cell") {
    const DataFrame in = make3({1, 1, 1, 1}, {1, 2, 3, 4}, {10, {}, 20, 30});
    const DataFrame out =
        win(in, {"p"}, {"o"},
            {frame_spec(WindowFunc::FrameSum, "v", "fs", 1, 1),
             frame_spec(WindowFunc::FrameMin, "v", "mn", 1, 1),
             frame_spec(WindowFunc::FrameMax, "v", "mx", 1, 1),
             frame_spec(WindowFunc::FrameCount, "v", "fc", 1, 1),
             frame_spec(WindowFunc::FrameMean, "v", "me", 1, 1)});
    REQUIRE(out.num_rows() == 4);
    CHECK(ints(out, "fs") == Col{10, 30, 50, 50});
    CHECK(ints(out, "mn") == Col{10, 10, 20, 20});
    CHECK(ints(out, "mx") == Col{10, 20, 30, 30});
    CHECK(ints(out, "fc") == Col{1, 2, 2, 2});
    check_dbls(dbls(out, "me"), {10.0, 15.0, 25.0, 25.0});
}

TEST_CASE("window - frame min/max reset per partition (no deque leak)") {
    const DataFrame in = make3({1, 1, 2, 2}, {1, 2, 1, 2}, {9, 1, 5, 3});
    const DataFrame out =
        win(in, {"p"}, {"o"},
            {frame_spec(WindowFunc::FrameMin, "v", "mn", 1, 1),
             frame_spec(WindowFunc::FrameMax, "v", "mx", 1, 1)});
    REQUIRE(out.num_rows() == 4);
    CHECK(ints(out, "mn") == Col{1, 1, 3, 3});
    CHECK(ints(out, "mx") == Col{9, 9, 5, 5});
}

TEST_CASE("window - frame all-null yields null for min/max/mean/count-zero") {
    const DataFrame in = make3({1, 1, 1}, {1, 2, 3}, {{}, {}, {}});
    const DataFrame out =
        win(in, {"p"}, {"o"},
            {frame_spec(WindowFunc::FrameMin, "v", "mn", 1, 1),
             frame_spec(WindowFunc::FrameMax, "v", "mx", 1, 1),
             frame_spec(WindowFunc::FrameMean, "v", "me", 1, 1),
             frame_spec(WindowFunc::FrameCount, "v", "fc", 1, 1)});
    REQUIRE(out.num_rows() == 3);
    const Col none = {std::nullopt, std::nullopt, std::nullopt};
    CHECK(ints(out, "mn") == none);
    CHECK(ints(out, "mx") == none);
    CHECK(dbls(out, "me") == Vals{std::nullopt, std::nullopt, std::nullopt});
    CHECK(ints(out, "fc") == Col{0, 0, 0});
}

TEST_CASE("window - large-n wide frame stays correct") {
    const std::int64_t n = 10000;
    const std::int64_t prec = 50;
    const std::int64_t foll = 50;
    std::vector<std::int64_t> p(static_cast<std::size_t>(n), 1);
    std::vector<std::int64_t> o;
    Col v;
    for (std::int64_t i = 0; i < n; ++i) {
        o.push_back(i);
        v.push_back(i);
    }
    const DataFrame in = make3(p, o, v);
    const DataFrame out =
        win(in, {"p"}, {"o"},
            {frame_spec(WindowFunc::FrameSum, "v", "fs", prec, foll),
             frame_spec(WindowFunc::FrameMin, "v", "mn", prec, foll),
             frame_spec(WindowFunc::FrameMax, "v", "mx", prec, foll)});
    REQUIRE(out.num_rows() == n);
    const Col fs = ints(out, "fs");
    const Col mn = ints(out, "mn");
    const Col mx = ints(out, "mx");
    for (std::int64_t i = 0; i < n; ++i) {
        const std::int64_t lo = std::max<std::int64_t>(0, i - prec);
        const std::int64_t hi = std::min<std::int64_t>(n - 1, i + foll);
        std::int64_t sum = 0;
        for (std::int64_t j = lo; j <= hi; ++j) sum += j;
        const auto at = static_cast<std::size_t>(i);
        CHECK(fs[at] == Opt{sum});
        CHECK(mn[at] == Opt{lo});
        CHECK(mx[at] == Opt{hi});
    }
}

TEST_CASE("window - frame_min/max determinism across input order") {
    const DataFrame a = make3({1, 1, 1, 2}, {1, 2, 3, 1}, {3, 1, 2, 9});
    const DataFrame b = make3({2, 1, 1, 1}, {1, 3, 1, 2}, {9, 2, 3, 1});
    const std::vector<WindowColumn> specs = {
        frame_spec(WindowFunc::FrameMin, "v", "mn", 1, 1),
        frame_spec(WindowFunc::FrameMax, "v", "mx", 1, 1)};
    const DataFrame oa = win(a, {"p"}, {"o"}, specs);
    const DataFrame ob = win(b, {"p"}, {"o"}, specs);
    REQUIRE(oa.num_rows() == 4);
    REQUIRE(ob.num_rows() == 4);
    CHECK(ints(oa, "mn") == ints(ob, "mn"));
    CHECK(ints(oa, "mx") == ints(ob, "mx"));
}

TEST_CASE("window - percent_rank over ties and single-row partition") {
    const DataFrame in = make3({1, 1, 1, 1}, {10, 20, 20, 40}, {0, 0, 0, 0});
    const DataFrame out = win(
        in, {"p"}, {"o"}, {spec(WindowFunc::PercentRank, std::nullopt, "pr")});
    REQUIRE(out.num_columns() == 4);
    REQUIRE(out.num_rows() == 4);
    check_dbls(dbls(out, "pr"), {0.0, 1.0 / 3.0, 1.0 / 3.0, 1.0});

    const DataFrame in2 = make3({7}, {5}, {0});
    const DataFrame out2 = win(
        in2, {"p"}, {"o"}, {spec(WindowFunc::PercentRank, std::nullopt, "pr")});
    REQUIRE(out2.num_rows() == 1);
    check_dbls(dbls(out2, "pr"), {0.0});
}

TEST_CASE("window - cume_dist shares among peers and resets per partition") {
    const DataFrame in =
        make3({1, 1, 1, 1, 2, 2}, {10, 20, 20, 40, 5, 5}, {0, 0, 0, 0, 0, 0});
    const DataFrame out =
        win(in, {"p"}, {"o"}, {spec(WindowFunc::CumeDist, std::nullopt, "cd")});
    REQUIRE(out.num_rows() == 6);
    check_dbls(dbls(out, "cd"), {0.25, 0.75, 0.75, 1.0, 1.0, 1.0});
}

TEST_CASE("window - range frame sum by value delta on the order column") {
    const DataFrame in = make3({1, 1, 1, 1}, {10, 12, 20, 21}, {1, 2, 3, 4});
    const DataFrame out = win(in, {"p"}, {"o"},
                              {frame_spec(WindowFunc::FrameSum, "v", "fs", 5, 5,
                                          0, WindowFrameMode::Range)});
    REQUIRE(out.num_rows() == 4);
    CHECK(ints(out, "fs") == Col{3, 3, 7, 7});
}

TEST_CASE("window - range unbounded-preceding includes peers, rows does not") {
    const DataFrame in = make3({1, 1, 1}, {10, 10, 20}, {1, 2, 3});
    const DataFrame out =
        win(in, {"p"}, {"o"},
            {spec(WindowFunc::RunningSum, "v", "rows"),
             frame_spec(WindowFunc::FrameSum, "v", "rng", WINDOW_UNBOUNDED, 0,
                        0, WindowFrameMode::Range)});
    REQUIRE(out.num_rows() == 3);
    const Col rows = ints(out, "rows");
    const Col rng = ints(out, "rng");
    CHECK(rows == Col{1, 3, 6});
    CHECK(rng == Col{3, 3, 6});
    CHECK(rows[0] != rng[0]);
}

TEST_CASE("window - range frame over a double order column resets per part") {
    const std::vector<double> o = {10.0, 12.0, 100.0, 101.0};
    DataFrame in;
    in.names = {"p", "o", "v"};
    in.columns.push_back(plain({1, 1, 2, 2}));
    in.columns.push_back(Series::flat_f64(o.data(), 4));
    in.columns.push_back(plain({1, 2, 10, 20}));
    const DataFrame out = win(in, {"p"}, {"o"},
                              {frame_spec(WindowFunc::FrameSum, "v", "fs", 5, 5,
                                          0, WindowFrameMode::Range)});
    REQUIRE(out.num_rows() == 4);
    CHECK(ints(out, "fs") == Col{3, 3, 30, 30});
}

TEST_CASE(
    "window - range frames treat null order values as peers of each other") {
    const DataFrame in = [] {
        DataFrame d;
        d.names = {"p", "o", "v"};
        d.columns.push_back(plain({1, 1, 1, 1, 1}));
        d.columns.push_back(i64s(Col{10, 12, std::nullopt, std::nullopt, 11}));
        d.columns.push_back(plain({1, 2, 100, 200, 4}));
        return d;
    }();
    const DataFrame out = win(in, {"p"}, {"o"},
                              {frame_spec(WindowFunc::FrameSum, "v", "fs", 5, 5,
                                          0, WindowFrameMode::Range)});
    REQUIRE(out.num_rows() == 5);
    CHECK(ints(out, "fs") == Col{7, 7, 7, 300, 300});
}

TEST_CASE("window - negative frame bounds are refused") {
    const DataFrame in = make3({1, 1}, {1, 2}, {1, 2});
    CHECK_THROWS_AS(
        (void)win(in, {"p"}, {"o"},
                  {frame_spec(WindowFunc::FrameSum, "v", "fs", -3, 1)}),
        std::invalid_argument);
    CHECK_THROWS_AS(
        (void)win(in, {"p"}, {"o"},
                  {frame_spec(WindowFunc::FrameSum, "v", "fs", 1, -1)}),
        std::invalid_argument);
    CHECK_THROWS_AS(
        (void)win(in, {"p"}, {"o"},
                  {frame_spec(WindowFunc::FrameSum, "v", "fs", 1, 1, -1)}),
        std::invalid_argument);
}

TEST_CASE("window - percent_rank/cume_dist/range determinism across order") {
    const DataFrame a = make3({1, 1, 1, 2}, {10, 20, 30, 5}, {1, 2, 3, 4});
    const DataFrame b = make3({2, 1, 1, 1}, {5, 30, 10, 20}, {4, 3, 1, 2});
    const std::vector<WindowColumn> specs = {
        spec(WindowFunc::PercentRank, std::nullopt, "pr"),
        spec(WindowFunc::CumeDist, std::nullopt, "cd"),
        frame_spec(WindowFunc::FrameSum, "v", "rng", 10, 10, 0,
                   WindowFrameMode::Range)};
    const DataFrame oa = win(a, {"p"}, {"o"}, specs);
    const DataFrame ob = win(b, {"p"}, {"o"}, specs);
    REQUIRE(oa.num_rows() == 4);
    REQUIRE(ob.num_rows() == 4);
    CHECK(dbls(oa, "pr") == dbls(ob, "pr"));
    CHECK(dbls(oa, "cd") == dbls(ob, "cd"));
    CHECK(ints(oa, "rng") == ints(ob, "rng"));
}

namespace {

struct RandomFrame {
    std::vector<std::int64_t> p, o, vi;
    std::vector<double> vf;
    std::vector<bool> p_ok, o_ok, vi_ok, vf_ok;
};

RandomFrame random_frame(std::uint64_t seed, std::size_t n, bool order_nulls) {
    std::mt19937_64 rng(seed);
    const auto pick = [&rng](std::int64_t lo, std::int64_t hi) {
        return std::uniform_int_distribution<std::int64_t>(lo, hi)(rng);
    };
    RandomFrame f;
    for (std::size_t i = 0; i < n; ++i) {
        f.p_ok.push_back(pick(0, 99) >= 5);
        f.p.push_back(f.p_ok.back() ? pick(0, 3) : 777);
        f.o_ok.push_back(!order_nulls || pick(0, 99) >= 10);
        f.o.push_back(f.o_ok.back() ? pick(0, 40) : -999);
        f.vi_ok.push_back(pick(0, 99) >= 15);
        f.vi.push_back(f.vi_ok.back() ? pick(-50, 50) : 123456);
        f.vf_ok.push_back(pick(0, 99) >= 15);
        f.vf.push_back(f.vf_ok.back() ? static_cast<double>(pick(-40, 40)) / 4.0
                                      : 1.0e9);
    }
    return f;
}

DataFrame to_frame(const RandomFrame& f) {
    DataFrame d;
    d.names = {"p", "o", "vi", "vf"};
    d.columns.push_back(i64s(f.p, f.p_ok));
    d.columns.push_back(i64s(f.o, f.o_ok));
    d.columns.push_back(i64s(f.vi, f.vi_ok));
    d.columns.push_back(f64s(f.vf, f.vf_ok));
    return d;
}

Vals to_vals(const std::vector<std::int64_t>& data,
             const std::vector<bool>& ok) {
    Vals out;
    for (std::size_t i = 0; i < data.size(); ++i)
        out.push_back(ok[i] ? Val{static_cast<double>(data[i])} : Val{});
    return out;
}

Vals to_vals(const std::vector<double>& data, const std::vector<bool>& ok) {
    Vals out;
    for (std::size_t i = 0; i < data.size(); ++i)
        out.push_back(ok[i] ? Val{data[i]} : Val{});
    return out;
}

int cmp_key(bool a_ok, std::int64_t a, bool b_ok, std::int64_t b) {
    if (!a_ok || !b_ok) return a_ok == b_ok ? 0 : (a_ok ? -1 : 1);
    return a < b ? -1 : (a > b ? 1 : 0);
}

using Part = std::vector<std::size_t>;

struct Sorted {
    Part rows;
    std::vector<std::pair<std::size_t, std::size_t>> parts;
};

Sorted sort_frame(const RandomFrame& f) {
    Sorted s;
    s.rows.resize(f.p.size());
    std::iota(s.rows.begin(), s.rows.end(), std::size_t{0});
    const auto part_cmp = [&f](std::size_t x, std::size_t y) {
        return cmp_key(f.p_ok[x], f.p[x], f.p_ok[y], f.p[y]);
    };
    std::stable_sort(
        s.rows.begin(), s.rows.end(), [&](std::size_t x, std::size_t y) {
            const int c = part_cmp(x, y);
            if (c != 0) return c < 0;
            return cmp_key(f.o_ok[x], f.o[x], f.o_ok[y], f.o[y]) < 0;
        });
    for (std::size_t b = 0; b < s.rows.size();) {
        std::size_t e = b + 1;
        while (e < s.rows.size() && part_cmp(s.rows[b], s.rows[e]) == 0) ++e;
        s.parts.emplace_back(b, e);
        b = e;
    }
    return s;
}

Vals per_row(const Sorted& s,
             const std::function<Val(const Part&, std::size_t)>& fn) {
    Vals out(s.rows.size());
    for (const auto& [b, e] : s.parts) {
        const Part part(s.rows.begin() + static_cast<std::ptrdiff_t>(b),
                        s.rows.begin() + static_cast<std::ptrdiff_t>(e));
        for (std::size_t i = 0; i < part.size(); ++i) out[b + i] = fn(part, i);
    }
    return out;
}

Vals gather(const Sorted& s, const Vals& v) {
    Vals out;
    for (const std::size_t r : s.rows) out.push_back(v[r]);
    return out;
}

struct Expect {
    std::string name;
    TypeId type;
    Vals want;
    bool approx;
};

std::string show(const Val& v) { return v ? std::to_string(*v) : "null"; }

void check_col(const DataFrame& d, const Expect& e) {
    const Series c = d.column(e.name);
    REQUIRE_MESSAGE(c.valid(), e.name);
    CHECK_MESSAGE(c.type() == e.type, e.name);
    const Vals got = nums(d, e.name);
    REQUIRE(got.size() == e.want.size());
    std::string bad;
    for (std::size_t i = 0; i < got.size() && bad.empty(); ++i) {
        const Val& g = got[i];
        const Val& w = e.want[i];
        bool same = g.has_value() == w.has_value();
        if (same && g) same = e.approx ? *g == doctest::Approx(*w) : *g == *w;
        if (!same)
            bad = e.name + " differs at sorted row " + std::to_string(i) +
                  ": got " + show(g) + ", want " + show(w);
    }
    CHECK_MESSAGE(bad.empty(), bad);
}

void add(std::vector<WindowColumn>& specs, std::vector<Expect>& want,
         WindowColumn w, TypeId type, Vals v, bool approx) {
    want.push_back(Expect{w.out, type, std::move(v), approx});
    specs.push_back(std::move(w));
}

struct FrameCase {
    std::int64_t preceding;
    std::int64_t following;
    std::int64_t min_count;
};

constexpr FrameCase ROWS_FRAMES[] = {
    {0, 0, 0},
    {1, 1, 0},
    {3, 2, 0},
    {WINDOW_UNBOUNDED, 0, 0},
    {0, WINDOW_UNBOUNDED, 0},
    {WINDOW_UNBOUNDED, WINDOW_UNBOUNDED, 0},
    {2, WINDOW_UNBOUNDED, 0},
    {WINDOW_UNBOUNDED, 3, 0},
    {1, 1, 2},
    {4, 0, 3},
};

constexpr FrameCase RANGE_FRAMES[] = {
    {0, 0, 0},
    {2, 3, 0},
    {10, 0, 0},
    {5, 5, 2},
    {WINDOW_UNBOUNDED, 0, 0},
    {0, WINDOW_UNBOUNDED, 0},
    {WINDOW_UNBOUNDED, WINDOW_UNBOUNDED, 0},
};

constexpr WindowFunc FRAME_FUNCS[] = {
    WindowFunc::FrameSum, WindowFunc::FrameMin, WindowFunc::FrameMax,
    WindowFunc::FrameCount, WindowFunc::FrameMean};

struct ValueCase {
    std::string name;
    Vals vals;
    TypeId type;
};

std::vector<ValueCase> value_cases(const RandomFrame& f) {
    return {{"vi", to_vals(f.vi, f.vi_ok), TypeId::Int64},
            {"vf", to_vals(f.vf, f.vf_ok), TypeId::Float64}};
}

std::string bound(std::int64_t b) {
    return b == WINDOW_UNBOUNDED ? "u" : std::to_string(b);
}

std::string frame_name(WindowFunc func, const std::string& value,
                       const FrameCase& fc, WindowFrameMode mode) {
    std::string tag;
    switch (func) {
        case WindowFunc::FrameSum:
            tag = "fsum";
            break;
        case WindowFunc::FrameMin:
            tag = "fmin";
            break;
        case WindowFunc::FrameMax:
            tag = "fmax";
            break;
        case WindowFunc::FrameCount:
            tag = "fcount";
            break;
        default:
            tag = "fmean";
            break;
    }
    return tag + "_" + value +
           (mode == WindowFrameMode::Rows ? "_rows_" : "_range_") +
           bound(fc.preceding) + "_" + bound(fc.following) + "_m" +
           std::to_string(fc.min_count);
}

TypeId frame_type(WindowFunc func, TypeId value) {
    if (func == WindowFunc::FrameCount) return TypeId::Int64;
    if (func == WindowFunc::FrameMean) return TypeId::Float64;
    return value;
}

Val aggregate(WindowFunc func, const Vals& cells, std::int64_t min_count) {
    std::int64_t count = 0;
    double sum = 0.0;
    Val lo;
    Val hi;
    for (const Val& c : cells) {
        if (!c) continue;
        ++count;
        sum += *c;
        if (!lo || *c < *lo) lo = c;
        if (!hi || *c > *hi) hi = c;
    }
    if (func == WindowFunc::FrameCount) return static_cast<double>(count);
    if (count == 0 || count < min_count) return Val{};
    switch (func) {
        case WindowFunc::FrameSum:
            return sum;
        case WindowFunc::FrameMin:
            return lo;
        case WindowFunc::FrameMax:
            return hi;
        default:
            return sum / static_cast<double>(count);
    }
}

Vals frame_ref(const Sorted& s, const RandomFrame& f, const Vals& v,
               WindowFunc func, const FrameCase& fc, WindowFrameMode mode) {
    return per_row(s, [&](const Part& part, std::size_t i) -> Val {
        const bool rows = mode == WindowFrameMode::Rows;
        const std::int64_t base =
            rows ? static_cast<std::int64_t>(i) : f.o[part[i]];
        Vals cells;
        for (std::size_t j = 0; j < part.size(); ++j) {
            const std::int64_t at =
                rows ? static_cast<std::int64_t>(j) : f.o[part[j]];
            const bool inside =
                (fc.preceding == WINDOW_UNBOUNDED ||
                 at >= base - fc.preceding) &&
                (fc.following == WINDOW_UNBOUNDED || at <= base + fc.following);
            if (inside) cells.push_back(v[part[j]]);
        }
        return aggregate(func, cells, fc.min_count);
    });
}

void check_passthrough(const DataFrame& out, const Sorted& s,
                       const RandomFrame& f) {
    check_col(out, Expect{"p", TypeId::Int64, gather(s, to_vals(f.p, f.p_ok)),
                          false});
    check_col(out, Expect{"o", TypeId::Int64, gather(s, to_vals(f.o, f.o_ok)),
                          false});
    check_col(out, Expect{"vi", TypeId::Int64,
                          gather(s, to_vals(f.vi, f.vi_ok)), false});
    check_col(out, Expect{"vf", TypeId::Float64,
                          gather(s, to_vals(f.vf, f.vf_ok)), false});
}

}  // namespace

TEST_CASE("window - randomized frames match a naive per-row reference") {
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        CAPTURE(seed);
        const RandomFrame f = random_frame(seed, 300, true);
        const Sorted s = sort_frame(f);
        const auto ord = [&f](std::size_t a, std::size_t b) {
            return cmp_key(f.o_ok[a], f.o[a], f.o_ok[b], f.o[b]);
        };
        const auto count_where = [&ord](const Part& part, std::size_t i,
                                        bool with_peers) {
            std::int64_t k = 0;
            for (std::size_t j = 0; j < part.size(); ++j) {
                const int c = ord(part[j], part[i]);
                if (c < 0 || (with_peers && c == 0)) ++k;
            }
            return k;
        };

        std::vector<WindowColumn> specs;
        std::vector<Expect> want;
        const auto row_number =
            per_row(s, [](const Part&, std::size_t i) -> Val {
                return static_cast<double>(i + 1);
            });
        add(specs, want, spec(WindowFunc::RowNumber, std::nullopt, "rn"),
            TypeId::Int64, row_number, false);
        add(specs, want, spec(WindowFunc::RunningCount, std::nullopt, "rc"),
            TypeId::Int64, row_number, false);
        add(specs, want, spec(WindowFunc::Rank, std::nullopt, "rank"),
            TypeId::Int64,
            per_row(s,
                    [&](const Part& part, std::size_t i) -> Val {
                        return static_cast<double>(count_where(part, i, false) +
                                                   1);
                    }),
            false);
        add(specs, want, spec(WindowFunc::DenseRank, std::nullopt, "dense"),
            TypeId::Int64,
            per_row(s,
                    [&](const Part& part, std::size_t i) -> Val {
                        std::vector<std::int64_t> keys;
                        for (std::size_t j = 0; j < part.size(); ++j)
                            if (ord(part[j], part[i]) < 0)
                                keys.push_back(f.o[part[j]]);
                        std::sort(keys.begin(), keys.end());
                        keys.erase(std::unique(keys.begin(), keys.end()),
                                   keys.end());
                        return static_cast<double>(keys.size() + 1);
                    }),
            false);
        add(specs, want, spec(WindowFunc::PercentRank, std::nullopt, "prank"),
            TypeId::Float64,
            per_row(s,
                    [&](const Part& part, std::size_t i) -> Val {
                        if (part.size() < 2) return 0.0;
                        return static_cast<double>(
                                   count_where(part, i, false)) /
                               static_cast<double>(part.size() - 1);
                    }),
            true);
        add(specs, want, spec(WindowFunc::CumeDist, std::nullopt, "cume"),
            TypeId::Float64,
            per_row(s,
                    [&](const Part& part, std::size_t i) -> Val {
                        return static_cast<double>(count_where(part, i, true)) /
                               static_cast<double>(part.size());
                    }),
            true);
        for (std::int64_t k : {1, 3, 7, 200}) {
            add(specs, want,
                offset_spec(WindowFunc::Ntile, std::nullopt,
                            "ntile" + std::to_string(k), k),
                TypeId::Int64,
                per_row(s,
                        [k](const Part& part, std::size_t i) -> Val {
                            const auto sz =
                                static_cast<std::int64_t>(part.size());
                            std::int64_t pos = 0;
                            for (std::int64_t b = 1; b <= k; ++b) {
                                const std::int64_t size =
                                    sz / k + (b <= sz % k ? 1 : 0);
                                if (static_cast<std::int64_t>(i) < pos + size)
                                    return static_cast<double>(b);
                                pos += size;
                            }
                            return Val{};
                        }),
                false);
        }

        const std::vector<ValueCase> values = value_cases(f);
        for (const ValueCase& vc : values) {
            const Vals& v = vc.vals;
            const std::string& n = vc.name;
            for (std::int64_t k : {1, 3}) {
                add(specs, want,
                    offset_spec(WindowFunc::Lag, n,
                                "lag" + std::to_string(k) + "_" + n, k),
                    vc.type,
                    per_row(
                        s,
                        [&v, k](const Part& part, std::size_t i) -> Val {
                            const auto at = static_cast<std::int64_t>(i) - k;
                            return at >= 0
                                       ? v[part[static_cast<std::size_t>(at)]]
                                       : Val{};
                        }),
                    false);
            }
            for (std::int64_t k : {1, 2}) {
                add(specs, want,
                    offset_spec(WindowFunc::Lead, n,
                                "lead" + std::to_string(k) + "_" + n, k),
                    vc.type,
                    per_row(
                        s,
                        [&v, k](const Part& part, std::size_t i) -> Val {
                            const auto at = static_cast<std::int64_t>(i) + k;
                            return at < static_cast<std::int64_t>(part.size())
                                       ? v[part[static_cast<std::size_t>(at)]]
                                       : Val{};
                        }),
                    false);
            }
            add(specs, want, spec(WindowFunc::FirstValue, n, "first_" + n),
                vc.type,
                per_row(s,
                        [&v](const Part& part, std::size_t) -> Val {
                            return v[part.front()];
                        }),
                false);
            add(specs, want, spec(WindowFunc::LastValue, n, "last_" + n),
                vc.type,
                per_row(s,
                        [&v](const Part& part, std::size_t) -> Val {
                            return v[part.back()];
                        }),
                false);
            for (std::int64_t k : {1, 2, 5}) {
                add(specs, want,
                    offset_spec(WindowFunc::NthValue, n,
                                "nth" + std::to_string(k) + "_" + n, k),
                    vc.type,
                    per_row(
                        s,
                        [&v, k](const Part& part, std::size_t) -> Val {
                            return k <= static_cast<std::int64_t>(part.size())
                                       ? v[part[static_cast<std::size_t>(k -
                                                                         1)]]
                                       : Val{};
                        }),
                    false);
            }
            add(specs, want, spec(WindowFunc::FillForward, n, "ffill_" + n),
                vc.type,
                per_row(s,
                        [&v](const Part& part, std::size_t i) -> Val {
                            for (std::size_t j = i + 1; j-- > 0;)
                                if (v[part[j]]) return v[part[j]];
                            return Val{};
                        }),
                false);
            add(specs, want, spec(WindowFunc::RunningSum, n, "rsum_" + n),
                vc.type,
                per_row(s,
                        [&v](const Part& part, std::size_t i) -> Val {
                            double sum = 0.0;
                            for (std::size_t j = 0; j <= i; ++j)
                                if (v[part[j]]) sum += *v[part[j]];
                            return sum;
                        }),
                false);
            add(specs, want, spec(WindowFunc::RunningMin, n, "rmin_" + n),
                vc.type,
                per_row(s,
                        [&v](const Part& part, std::size_t i) -> Val {
                            Val best;
                            for (std::size_t j = 0; j <= i; ++j) {
                                const Val& c = v[part[j]];
                                if (c && (!best || *c < *best)) best = c;
                            }
                            return best;
                        }),
                false);
            add(specs, want, spec(WindowFunc::RunningMax, n, "rmax_" + n),
                vc.type,
                per_row(s,
                        [&v](const Part& part, std::size_t i) -> Val {
                            Val best;
                            for (std::size_t j = 0; j <= i; ++j) {
                                const Val& c = v[part[j]];
                                if (c && (!best || *c > *best)) best = c;
                            }
                            return best;
                        }),
                false);
            add(specs, want, spec(WindowFunc::RunningProd, n, "rprod_" + n),
                TypeId::Float64,
                per_row(s,
                        [&v](const Part& part, std::size_t i) -> Val {
                            double prod = 1.0;
                            for (std::size_t j = 0; j <= i; ++j)
                                if (v[part[j]]) prod *= *v[part[j]];
                            return prod;
                        }),
                true);
            add(specs, want, spec(WindowFunc::Delta, n, "delta_" + n), vc.type,
                per_row(s,
                        [&v](const Part& part, std::size_t i) -> Val {
                            if (i == 0) return Val{};
                            const Val& cur = v[part[i]];
                            const Val& prev = v[part[i - 1]];
                            if (!cur || !prev) return Val{};
                            return *cur - *prev;
                        }),
                false);
            for (const FrameCase& fc : ROWS_FRAMES) {
                for (const WindowFunc func : FRAME_FUNCS) {
                    add(specs, want,
                        frame_spec(
                            func, n,
                            frame_name(func, n, fc, WindowFrameMode::Rows),
                            fc.preceding, fc.following, fc.min_count,
                            WindowFrameMode::Rows),
                        frame_type(func, vc.type),
                        frame_ref(s, f, v, func, fc, WindowFrameMode::Rows),
                        func == WindowFunc::FrameMean);
                }
            }
        }

        const DataFrame in = to_frame(f);
        const DataFrame out = win(in, {"p"}, {"o"}, specs);
        REQUIRE(out.num_rows() == static_cast<std::int64_t>(f.p.size()));
        REQUIRE(out.num_columns() == 4 + specs.size());
        check_passthrough(out, s, f);
        for (const Expect& e : want) check_col(out, e);
    }
}

TEST_CASE("window - randomized range frames match a naive per-row reference") {
    for (std::uint64_t seed = 11; seed <= 16; ++seed) {
        CAPTURE(seed);
        const RandomFrame f = random_frame(seed, 300, false);
        const Sorted s = sort_frame(f);
        std::vector<WindowColumn> specs;
        std::vector<Expect> want;
        const std::vector<ValueCase> values = value_cases(f);
        for (const ValueCase& vc : values) {
            for (const FrameCase& fc : RANGE_FRAMES) {
                for (const WindowFunc func : FRAME_FUNCS) {
                    add(specs, want,
                        frame_spec(func, vc.name,
                                   frame_name(func, vc.name, fc,
                                              WindowFrameMode::Range),
                                   fc.preceding, fc.following, fc.min_count,
                                   WindowFrameMode::Range),
                        frame_type(func, vc.type),
                        frame_ref(s, f, vc.vals, func, fc,
                                  WindowFrameMode::Range),
                        func == WindowFunc::FrameMean);
                }
            }
        }
        const DataFrame in = to_frame(f);
        const DataFrame out = win(in, {"p"}, {"o"}, specs);
        REQUIRE(out.num_rows() == static_cast<std::int64_t>(f.p.size()));
        REQUIRE(out.num_columns() == 4 + specs.size());
        check_passthrough(out, s, f);
        for (const Expect& e : want) check_col(out, e);
    }
}

namespace {

using UCol = std::vector<std::optional<std::uint64_t>>;

UCol uints(const DataFrame& d, const std::string& name) {
    const Series c = column_of(d, name);
    REQUIRE(c.type() == TypeId::Uint64);
    UCol out;
    for (std::int64_t i = 0; i < c.length(); ++i)
        out.push_back(c.is_null(i) ? std::optional<std::uint64_t>{}
                                   : std::optional<std::uint64_t>{
                                         c.data<std::uint64_t>()[i]});
    return out;
}

std::string overflow_of(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const std::overflow_error& e) {
        return e.what();
    }
    return {};
}

constexpr std::uint64_t BIG = std::uint64_t{1} << 63;

}  // namespace

TEST_CASE("window - running_sum of uint64 values past 2^63 is exact") {
    const DataFrame in = make3u({1, 1, 1}, {1, 2, 3}, {BIG, 1, 2});
    const DataFrame out =
        win(in, {"p"}, {"o"}, {spec(WindowFunc::RunningSum, "v", "s")});
    CHECK(uints(out, "s") == UCol{BIG, BIG + 1, BIG + 3});
}

TEST_CASE("window - integer sums outside the type are errors, not wraps") {
    const DataFrame u = make3u({1, 1}, {1, 2}, {UINT64_MAX, 1});
    const std::string run = overflow_of([&] {
        (void)win(u, {"p"}, {"o"}, {spec(WindowFunc::RunningSum, "v", "s")});
    });
    CHECK(run.find("RUNNING_SUM") != std::string::npos);
    CHECK(run.find("uint64") != std::string::npos);

    const DataFrame i = make3({1, 1}, {1, 2}, {INT64_MAX, 1});
    CHECK(overflow_of([&] {
              (void)win(i, {"p"}, {"o"},
                        {spec(WindowFunc::RunningSum, "v", "s")});
          }).find("int64") != std::string::npos);
    CHECK(overflow_of([&] {
              (void)win(i, {"p"}, {"o"},
                        {frame_spec(WindowFunc::FrameSum, "v", "s",
                                    WINDOW_UNBOUNDED, 0)});
          }).find("FRAME_SUM") != std::string::npos);
}

TEST_CASE("window - frame_sum of uint64 values past 2^63 slides exactly") {
    const DataFrame in = make3u({1, 1, 1, 1}, {1, 2, 3, 4}, {BIG, 5, 6, 7});
    const DataFrame out = win(
        in, {"p"}, {"o"}, {frame_spec(WindowFunc::FrameSum, "v", "fs", 1, 0)});
    CHECK(uints(out, "fs") == UCol{BIG, BIG + 5, 11, 13});
}

TEST_CASE("window - delta is exact int64 or an error") {
    const DataFrame near = make3u({1, 1}, {1, 2}, {BIG + 5, BIG});
    CHECK(ints(win(near, {"p"}, {"o"}, {spec(WindowFunc::Delta, "v", "d")}),
               "d") == Col{std::nullopt, -5});
    for (const auto& values : {std::vector<std::uint64_t>{0, UINT64_MAX},
                               std::vector<std::uint64_t>{0, BIG}}) {
        const DataFrame in = make3u({1, 1}, {1, 2}, values);
        const std::string e = overflow_of([&] {
            (void)win(in, {"p"}, {"o"}, {spec(WindowFunc::Delta, "v", "d")});
        });
        CHECK(e.find("DELTA") != std::string::npos);
    }
    const DataFrame signed_in = make3({1, 1}, {1, 2}, {INT64_MIN, INT64_MAX});
    CHECK(overflow_of([&] {
              (void)win(signed_in, {"p"}, {"o"},
                        {spec(WindowFunc::Delta, "v", "d")});
          }).find("DELTA") != std::string::npos);
}

TEST_CASE("window - sliding integer sums equal a 128-bit reference") {
    std::mt19937_64 rng(3);
    for (const bool is_unsigned : {false, true}) {
        std::vector<std::int64_t> p, o;
        std::vector<std::uint64_t> uv;
        std::vector<std::int64_t> sv;
        for (int i = 0; i < 400; ++i) {
            p.push_back(i / 100);
            o.push_back(i % 100);
            const std::uint64_t r = rng() >> 2;  // below 2^62
            uv.push_back(r);
            sv.push_back(static_cast<std::int64_t>(rng() % (1ULL << 61)) *
                         (rng() & 1 ? 1 : -1));
        }
        DataFrame in = is_unsigned ? make3u(p, o, uv) : make3(p, o, [&] {
            Col c;
            for (auto x : sv) c.push_back(x);
            return c;
        }());
        const DataFrame out =
            win(in, {"p"}, {"o"},
                {frame_spec(WindowFunc::FrameSum, "v", "fs", 2, 1)});
        __extension__ typedef __int128 wide;
        for (int i = 0; i < 400; ++i) {
            wide sum = 0;
            for (int k = std::max(i - 2, (i / 100) * 100);
                 k <= std::min(i + 1, (i / 100) * 100 + 99); ++k)
                sum += is_unsigned
                           ? static_cast<wide>(uv[static_cast<std::size_t>(k)])
                           : static_cast<wide>(sv[static_cast<std::size_t>(k)]);
            const Series c = column_of(out, "fs");
            if (is_unsigned)
                REQUIRE(static_cast<wide>(c.data<std::uint64_t>()[i]) == sum);
            else
                REQUIRE(static_cast<wide>(c.data<std::int64_t>()[i]) == sum);
        }
    }
}

static bool same(const DataFrame& a, const DataFrame& b, WindowFunc func) {
    return func == WindowFunc::FrameMean ? dbls(a, "r") == dbls(b, "r")
                                         : ints(a, "r") == ints(b, "r");
}

TEST_CASE("window - ABI range frames equal the C++ range results") {
    const DataFrame in = make3({1, 1, 1, 1, 2, 2}, {10, 12, 12, 21, 5, 9},
                               {1, 2, std::nullopt, 4, 5, 6});
    for (const FrameCase& fc : RANGE_FRAMES) {
        CAPTURE(fc.preceding);
        CAPTURE(fc.following);
        for (const WindowFunc func : FRAME_FUNCS) {
            dftu_window_spec abi{};
            abi.func = static_cast<dftu_window_func>(func);
            abi.value = "v";
            abi.out = "r";
            abi.param.frame = {fc.min_count, fc.preceding,
                               fc.following, DFTU_WINDOW_FRAME_RANGE,
                               0.0,          nullptr};
            const DataFrame via_abi =
                win(in, {"p"}, {"o"}, {df::window_column(abi)});
            const DataFrame want =
                win(in, {"p"}, {"o"},
                    {frame_spec(func, "v", "r", fc.preceding, fc.following,
                                fc.min_count, WindowFrameMode::Range)});
            CHECK(same(via_abi, want, func));
            abi.param.frame.mode = DFTU_WINDOW_FRAME_ROWS;
            const DataFrame rows =
                win(in, {"p"}, {"o"}, {df::window_column(abi)});
            const DataFrame rows_want =
                win(in, {"p"}, {"o"},
                    {frame_spec(func, "v", "r", fc.preceding, fc.following,
                                fc.min_count)});
            CHECK(same(rows, rows_want, func));
        }
    }
}

namespace {

using Keys = std::vector<std::optional<std::string>>;

std::string key_at(const Series& c, std::int64_t i) {
    switch (c.type()) {
        case TypeId::Int64:
            return std::to_string(c.data<std::int64_t>()[i]);
        case TypeId::Float64: {
            char buf[40];
            std::snprintf(buf, sizeof buf, "%.17g", c.data<double>()[i]);
            return buf;
        }
        case TypeId::String:
            return std::string(c.string_at(i));
        case TypeId::Bool:
            return ((c.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1) ? "T"
                                                                     : "F";
        default:
            FAIL("unexpected column type");
            return {};
    }
}

Keys keys_of(const Series& column) {
    const Series c = column.materialize();
    Keys out;
    if (c.type() == TypeId::List) {
        const std::int32_t* off = c.offsets();
        REQUIRE(off != nullptr);
        const Series elems = c.child(0).materialize();
        for (std::int64_t i = 0; i < c.length(); ++i) {
            std::string joined;
            for (std::int32_t at = off[i]; at < off[i + 1]; ++at)
                joined += (at > off[i] ? "|" : "") + key_at(elems, at);
            out.emplace_back(joined);
        }
        return out;
    }
    for (std::int64_t i = 0; i < c.length(); ++i)
        out.push_back(c.is_null(i) ? std::optional<std::string>{}
                                   : std::optional<std::string>{key_at(c, i)});
    return out;
}

struct Extra {
    std::vector<std::string> vs, bs;
    std::vector<bool> vs_ok, bs_ok, vb, vb_ok, bb, bb_ok, bi_ok;
    std::vector<std::int64_t> bi;
};

Extra random_extra(std::uint64_t seed, std::size_t n) {
    std::mt19937_64 rng(seed * 7919 + 13);
    const auto pick = [&rng](int lo, int hi) {
        return std::uniform_int_distribution<int>(lo, hi)(rng);
    };
    static const char* const WORDS[] = {"a", "b", "c", "dd", "e"};
    Extra e;
    for (std::size_t i = 0; i < n; ++i) {
        e.vs_ok.push_back(pick(0, 99) >= 15);
        e.vs.push_back(e.vs_ok.back() ? WORDS[pick(0, 4)] : "");
        e.bs_ok.push_back(pick(0, 99) >= 10);
        e.bs.push_back(e.bs_ok.back() ? WORDS[pick(0, 2)] : "");
        e.vb_ok.push_back(pick(0, 99) >= 15);
        e.vb.push_back(e.vb_ok.back() && pick(0, 1) == 1);
        e.bb_ok.push_back(pick(0, 99) >= 10);
        e.bb.push_back(e.bb_ok.back() && pick(0, 1) == 1);
        e.bi_ok.push_back(pick(0, 99) >= 10);
        e.bi.push_back(e.bi_ok.back() ? pick(0, 6) : 99);
    }
    return e;
}

Series text_series(const std::vector<std::string>& v,
                   const std::vector<bool>& ok) {
    const std::vector<std::string_view> views(v.begin(), v.end());
    const std::vector<std::uint8_t> bits = bits_of(ok);
    return Series::strings(views, bits.data());
}

Series bool_series(const std::vector<bool>& v, const std::vector<bool>& ok) {
    const std::vector<std::uint8_t> data = bits_of(v);
    const std::vector<std::uint8_t> bits = bits_of(ok);
    return Series::flat(TypeId::Bool, data.data(),
                        static_cast<std::int64_t>(v.size()), bits.data());
}

DataFrame stat_frame(const RandomFrame& f, const Extra& e) {
    DataFrame d = to_frame(f);
    d.names.insert(d.names.end(), {"vs", "vb", "bi", "bs", "bb"});
    d.columns.push_back(text_series(e.vs, e.vs_ok));
    d.columns.push_back(bool_series(e.vb, e.vb_ok));
    d.columns.push_back(i64s(e.bi, e.bi_ok));
    d.columns.push_back(text_series(e.bs, e.bs_ok));
    d.columns.push_back(bool_series(e.bb, e.bb_ok));
    return d;
}

WindowColumn stat_spec(WindowFunc func, const std::string& value,
                       std::string out, const FrameCase& fc,
                       WindowFrameMode mode, double q = 0.0,
                       const std::string& by = {}) {
    WindowColumn w = frame_spec(func, value, std::move(out), fc.preceding,
                                fc.following, fc.min_count, mode);
    w.params.frame.q = q;
    if (!by.empty()) w.set_by(by);
    return w;
}

using Members = std::vector<Part>;

Members frame_members(const Sorted& s, const RandomFrame& f,
                      const FrameCase& fc, WindowFrameMode mode) {
    Members out(s.rows.size());
    for (const auto& [b, e] : s.parts) {
        for (std::size_t i = b; i < e; ++i) {
            const std::size_t gi = s.rows[i];
            for (std::size_t j = b; j < e; ++j) {
                const std::size_t gj = s.rows[j];
                bool inside;
                if (mode == WindowFrameMode::Rows) {
                    const auto pi = static_cast<std::int64_t>(i - b);
                    const auto pj = static_cast<std::int64_t>(j - b);
                    inside = (fc.preceding == WINDOW_UNBOUNDED ||
                              pj >= pi - fc.preceding) &&
                             (fc.following == WINDOW_UNBOUNDED ||
                              pj <= pi + fc.following);
                } else if (!f.o_ok[gi]) {
                    inside = !f.o_ok[gj];
                } else {
                    inside = f.o_ok[gj] &&
                             (fc.preceding == WINDOW_UNBOUNDED ||
                              f.o[gj] >= f.o[gi] - fc.preceding) &&
                             (fc.following == WINDOW_UNBOUNDED ||
                              f.o[gj] <= f.o[gi] + fc.following);
                }
                if (inside) out[i].push_back(gj);
            }
        }
    }
    return out;
}

Vals frame_cells(const Members& mem, std::size_t k, const Vals& v) {
    Vals cells;
    for (const std::size_t g : mem[k]) cells.push_back(v[g]);
    return cells;
}

Val variance_ref(const Vals& cells, bool root, std::int64_t min_count) {
    std::vector<long double> x;
    for (const Val& c : cells)
        if (c) x.push_back(*c);
    const auto n = static_cast<std::int64_t>(x.size());
    if (n < 2 || n < min_count) return Val{};
    long double mean = 0;
    for (const long double v : x) mean += v;
    mean /= static_cast<long double>(n);
    long double s2 = 0;
    for (const long double v : x) s2 += (v - mean) * (v - mean);
    const long double var = s2 / static_cast<long double>(n - 1);
    return static_cast<double>(root ? std::sqrt(var) : var);
}

double quantile_ref(std::vector<double> x, double q) {
    std::sort(x.begin(), x.end());
    const std::size_t n = x.size();
    if (q <= 0.0) return x.front();
    if (q >= 1.0) return x.back();
    const double pos = q * static_cast<double>(n - 1);
    const auto lo = static_cast<std::size_t>(pos);
    const double frac = pos - static_cast<double>(lo);
    if (lo + 1 >= n) return x[lo];
    return x[lo] * (1.0 - frac) + x[lo + 1] * frac;
}

double max_rel_error = 0.0;

void check_close(const DataFrame& d, const std::string& name, const Vals& want,
                 double rel) {
    REQUIRE_MESSAGE(column_of(d, name).type() == TypeId::Float64, name);
    const Vals got = nums(d, name);
    REQUIRE(got.size() == want.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
        const bool same = got[i].has_value() == want[i].has_value();
        REQUIRE_MESSAGE(same, name << " null mismatch at sorted row " << i);
        if (!got[i]) continue;
        if (*want[i] == 0.0)
            REQUIRE_MESSAGE(*got[i] == 0.0, name << " at sorted row " << i
                                                 << ": got " << *got[i]
                                                 << ", want exactly 0");
        const double err =
            std::fabs(*got[i] - *want[i]) / std::max(1.0, std::fabs(*want[i]));
        max_rel_error = std::max(max_rel_error, err);
        REQUIRE_MESSAGE(err <= rel, name << " at sorted row " << i << ": got "
                                         << *got[i] << ", want " << *want[i]);
    }
}

void check_keys(const DataFrame& d, const std::string& name, const Keys& want) {
    const Keys got = keys_of(d.column(name));
    REQUIRE(got.size() == want.size());
    for (std::size_t i = 0; i < got.size(); ++i)
        REQUIRE_MESSAGE(got[i] == want[i],
                        name << " differs at sorted row " << i << ": got "
                             << got[i].value_or("null") << ", want "
                             << want[i].value_or("null"));
}

struct ByCase {
    std::string name;
    std::vector<bool> ok;
    std::function<int(std::size_t, std::size_t)> cmp;
};

void run_stat_frames(std::uint64_t seed, bool order_nulls, WindowFrameMode mode,
                     std::span<const FrameCase> frames) {
    const RandomFrame f = random_frame(seed, 300, order_nulls);
    const Extra e = random_extra(seed, 300);
    const Sorted s = sort_frame(f);
    const DataFrame in = stat_frame(f, e);
    const std::size_t n = s.rows.size();

    const std::vector<std::pair<std::string, Vals>> numeric = {
        {"vi", to_vals(f.vi, f.vi_ok)}, {"vf", to_vals(f.vf, f.vf_ok)}};
    const std::vector<std::string> value_names = {"vi", "vf", "vs", "vb"};
    const std::vector<ByCase> bys = {
        {"bi", e.bi_ok,
         [&e](std::size_t a, std::size_t b) {
             return e.bi[a] < e.bi[b] ? -1 : (e.bi[a] > e.bi[b] ? 1 : 0);
         }},
        {"bs", e.bs_ok,
         [&e](std::size_t a, std::size_t b) {
             return e.bs[a].compare(e.bs[b]) < 0 ? -1
                                                 : (e.bs[a] == e.bs[b] ? 0 : 1);
         }},
        {"bb", e.bb_ok, [&e](std::size_t a, std::size_t b) {
             return static_cast<int>(e.bb[a]) - static_cast<int>(e.bb[b]);
         }}};
    constexpr double LEVELS[] = {0.0, 0.25, 0.5, 0.9, 1.0};

    std::vector<WindowColumn> specs;
    std::vector<std::function<void(const DataFrame&)>> checks;
    int id = 0;
    const auto uniq = [&id](const char* tag, const std::string& col) {
        return std::string(tag) + "_" + col + "_" + std::to_string(id++);
    };

    for (const FrameCase& fc : frames) {
        const Members mem = frame_members(s, f, fc, mode);
        for (const auto& [col, vals] : numeric) {
            for (const bool root : {false, true}) {
                const std::string out = uniq(root ? "std" : "var", col);
                specs.push_back(stat_spec(
                    root ? WindowFunc::FrameStd : WindowFunc::FrameVar, col,
                    out, fc, mode));
                Vals want(n);
                for (std::size_t k = 0; k < n; ++k)
                    want[k] = variance_ref(frame_cells(mem, k, vals), root,
                                           fc.min_count);
                checks.push_back([out, want](const DataFrame& d) {
                    check_close(d, out, want, 1e-9);
                });
            }
            for (const double q : LEVELS) {
                const std::string out = uniq("q", col);
                specs.push_back(stat_spec(WindowFunc::FrameQuantile, col, out,
                                          fc, mode, q));
                Vals want(n);
                for (std::size_t k = 0; k < n; ++k) {
                    std::vector<double> x;
                    for (const Val& c : frame_cells(mem, k, vals))
                        if (c) x.push_back(*c);
                    if (!x.empty() &&
                        static_cast<std::int64_t>(x.size()) >= fc.min_count)
                        want[k] = quantile_ref(std::move(x), q);
                }
                // Linear interpolation may compile to fused multiply-adds
                // (GCC on ARM), so it differs from the reference by a few ulp
                // of its operands, which can exceed the result's own ulp.
                checks.push_back([out, want](const DataFrame& d) {
                    check_close(d, out, want,
                                16 * std::numeric_limits<double>::epsilon());
                });
            }
        }
        for (const std::string& col : value_names) {
            const Keys vk = keys_of(in.column(col));
            const TypeId vtype = in.column(col).type();
            {
                const std::string out = uniq("cd", col);
                specs.push_back(stat_spec(WindowFunc::FrameCountDistinct, col,
                                          out, fc, mode));
                Keys want(n);
                for (std::size_t k = 0; k < n; ++k) {
                    std::set<std::string> seen;
                    for (const std::size_t g : mem[k])
                        if (vk[g]) seen.insert(*vk[g]);
                    want[k] = std::to_string(seen.size());
                }
                checks.push_back([out, want](const DataFrame& d) {
                    REQUIRE(column_of(d, out).type() == TypeId::Int64);
                    check_keys(d, out, want);
                });
            }
            {
                const std::string out = uniq("collect", col);
                specs.push_back(
                    stat_spec(WindowFunc::FrameCollect, col, out, fc, mode));
                Keys want(n);
                for (std::size_t k = 0; k < n; ++k) {
                    std::string joined;
                    bool first = true;
                    for (const std::size_t g : mem[k]) {
                        if (!vk[g]) continue;
                        joined += (first ? "" : "|") + *vk[g];
                        first = false;
                    }
                    want[k] = joined;
                }
                checks.push_back([out, want](const DataFrame& d) {
                    REQUIRE(column_of(d, out).type() == TypeId::List);
                    check_keys(d, out, want);
                });
            }
            for (const ByCase& by : bys) {
                for (const bool smallest : {false, true}) {
                    const std::string out =
                        uniq(smallest ? "argmin" : "argmax", col + by.name);
                    specs.push_back(
                        stat_spec(smallest ? WindowFunc::FrameArgMin
                                           : WindowFunc::FrameArgMax,
                                  col, out, fc, mode, 0.0, by.name));
                    Keys want(n);
                    for (std::size_t k = 0; k < n; ++k) {
                        std::optional<std::size_t> best;
                        std::int64_t present = 0;
                        for (const std::size_t g : mem[k]) {
                            if (!by.ok[g]) continue;
                            ++present;
                            if (!best) {
                                best = g;
                                continue;
                            }
                            const int c = by.cmp(g, *best);
                            if (smallest ? c < 0 : c > 0) best = g;
                        }
                        if (best && present >= fc.min_count)
                            want[k] = vk[*best];
                    }
                    checks.push_back([out, want, vtype](const DataFrame& d) {
                        REQUIRE(column_of(d, out).type() == vtype);
                        check_keys(d, out, want);
                    });
                }
            }
        }
    }
    const DataFrame out = win(in, {"p"}, {"o"}, specs);
    REQUIRE(out.num_rows() == static_cast<std::int64_t>(n));
    for (const auto& check : checks) check(out);
}

}  // namespace

TEST_CASE("window - new rows frame functions match a naive reference") {
    for (std::uint64_t seed = 21; seed <= 24; ++seed) {
        CAPTURE(seed);
        run_stat_frames(seed, true, WindowFrameMode::Rows, ROWS_FRAMES);
    }
}

TEST_CASE("window - new range frame functions match a naive reference") {
    for (std::uint64_t seed = 31; seed <= 34; ++seed) {
        CAPTURE(seed);
        run_stat_frames(seed, false, WindowFrameMode::Range, RANGE_FRAMES);
    }
}

TEST_CASE("window - new range frame functions treat null orders as peers") {
    for (std::uint64_t seed = 41; seed <= 44; ++seed) {
        CAPTURE(seed);
        run_stat_frames(seed, true, WindowFrameMode::Range, RANGE_FRAMES);
    }
}

namespace {

DataFrame names_dur(const std::vector<std::string>& names, const Col& dur) {
    DataFrame d;
    d.names = {"p", "o", "name", "dur"};
    std::vector<std::int64_t> p(names.size(), 1), o(names.size());
    std::iota(o.begin(), o.end(), std::int64_t{0});
    d.columns.push_back(plain(p));
    d.columns.push_back(plain(o));
    d.columns.push_back(Series::strings(names));
    d.columns.push_back(i64s(dur));
    return d;
}

}  // namespace

TEST_CASE("window - count_distinct over 3 rows of a, b, a, c") {
    const DataFrame in = names_dur({"a", "b", "a", "c"}, {1, 2, 3, 4});
    const DataFrame out =
        win(in, {"p"}, {"o"},
            {frame_spec(WindowFunc::FrameCountDistinct, "name", "cd", 2, 0)});
    CHECK(ints(out, "cd") == Col{1, 2, 2, 3});
}

TEST_CASE("window - arg_max over 2 rows takes the earliest row on a tie") {
    const DataFrame in = names_dur({"n0", "n1", "n2", "n3"}, {5, 9, 9, 2});
    WindowColumn mx = frame_spec(WindowFunc::FrameArgMax, "name", "mx", 1, 0);
    mx.set_by("dur");
    WindowColumn mn = frame_spec(WindowFunc::FrameArgMin, "name", "mn", 1, 0);
    mn.set_by("dur");
    const DataFrame out = win(in, {"p"}, {"o"}, {mx, mn});
    CHECK(strs(out, "mx") == SCol{"n0", "n1", "n1", "n2"});
    CHECK(strs(out, "mn") == SCol{"n0", "n0", "n1", "n3"});
}

TEST_CASE("window - arg_max skips a null by and may return a null value") {
    DataFrame in = names_dur({"x", "y", "z", "w"}, {5, std::nullopt, 9, 9});
    WindowColumn mx =
        frame_spec(WindowFunc::FrameArgMax, "name", "mx", WINDOW_UNBOUNDED, 0);
    mx.set_by("dur");
    CHECK(strs(win(in, {"p"}, {"o"}, {mx}), "mx") == SCol{"x", "x", "z", "z"});

    DataFrame nullval;
    nullval.names = {"p", "o", "v", "b"};
    nullval.columns.push_back(plain({1, 1}));
    nullval.columns.push_back(plain({1, 2}));
    nullval.columns.push_back(i64s(Col{std::nullopt, 7}));
    nullval.columns.push_back(plain({9, 1}));
    WindowColumn pick =
        frame_spec(WindowFunc::FrameArgMax, "v", "r", WINDOW_UNBOUNDED, 0);
    pick.set_by("b");
    CHECK(ints(win(nullval, {"p"}, {"o"}, {pick}), "r") ==
          Col{std::nullopt, std::nullopt});
}

TEST_CASE("window - quantile over 3 rows is exact with interpolation") {
    const DataFrame in = names_dur({"a", "b", "c", "d", "e"}, {4, 1, 3, 10, 2});
    const auto q = [&](double level, std::int64_t pre) {
        WindowColumn w =
            frame_spec(WindowFunc::FrameQuantile, "dur", "q", pre, 0);
        w.params.frame.q = level;
        return dbls(win(in, {"p"}, {"o"}, {w}), "q");
    };
    const Vals half = q(0.5, 2);
    const std::vector<double> want = {4.0, 2.5, 3.0, 3.0, 3.0};
    for (std::size_t i = 0; i < want.size(); ++i) CHECK(*half[i] == want[i]);
    CHECK(*q(0.25, 2)[1] == 1.75);
    CHECK(*q(0.0, 2)[4] == 2.0);
    CHECK(*q(1.0, 2)[3] == 10.0);
}

TEST_CASE("window - collect lists present values in frame order") {
    DataFrame in = names_dur({"x", "y", "z"}, {1, 2, 3});
    in.columns[2] = text_series({"x", "", "z"}, {true, false, true});
    const DataFrame out = win(
        in, {"p"}, {"o"},
        {frame_spec(WindowFunc::FrameCollect, "name", "c", WINDOW_UNBOUNDED, 0),
         frame_spec(WindowFunc::FrameCollect, "name", "e", 0, 0)});
    CHECK(keys_of(out.column("c")) == Keys{"x", "x", "x|z"});
    CHECK(keys_of(out.column("e")) == Keys{"x", "", "z"});
}

TEST_CASE("window - collect past 2^27 values in all fails naming the frame") {
    const std::int64_t n = 12000;
    std::vector<std::int64_t> p(static_cast<std::size_t>(n), 1), o(p.size());
    std::iota(o.begin(), o.end(), std::int64_t{0});
    DataFrame in;
    in.names = {"p", "o", "v"};
    in.columns.push_back(plain(p));
    in.columns.push_back(plain(o));
    in.columns.push_back(plain(o));
    const auto run = [&](std::int64_t rows) {
        DataFrame part;
        part.names = in.names;
        for (const Series& c : in.columns)
            part.columns.push_back(c.slice(0, rows));
        return win(part, {"p"}, {"o"},
                   {frame_spec(WindowFunc::FrameCollect, "v", "c",
                               WINDOW_UNBOUNDED, WINDOW_UNBOUNDED)});
    };
    try {
        (void)run(n);
        FAIL("expected the cap error");
    } catch (const std::length_error& e) {
        CHECK(std::string(e.what()).find("FRAME_COLLECT") != std::string::npos);
    }
    CHECK(run(100).num_rows() == 100);
}

TEST_CASE("window - new frame functions refuse bad parameters") {
    const DataFrame in = names_dur({"a", "b"}, {1, 2});
    const auto quantile = [&](double level) {
        WindowColumn w = frame_spec(WindowFunc::FrameQuantile, "dur", "q",
                                    WINDOW_UNBOUNDED, 0);
        w.params.frame.q = level;
        return win(in, {"p"}, {"o"}, {w});
    };
    CHECK_THROWS_AS((void)quantile(-0.1), std::invalid_argument);
    CHECK_THROWS_AS((void)quantile(1.5), std::invalid_argument);
    CHECK_THROWS_AS((void)quantile(std::nan("")), std::invalid_argument);
    CHECK_NOTHROW((void)quantile(0.0));
    CHECK_NOTHROW((void)quantile(1.0));

    CHECK_THROWS_AS(
        (void)win(in, {"p"}, {"o"},
                  {frame_spec(WindowFunc::FrameArgMax, "name", "r", 1, 0)}),
        std::invalid_argument);
    WindowColumn unknown =
        frame_spec(WindowFunc::FrameArgMax, "name", "r", 1, 0);
    unknown.set_by("nope");
    CHECK_THROWS_AS((void)win(in, {"p"}, {"o"}, {unknown}), std::out_of_range);
    WindowColumn listed = frame_spec(WindowFunc::FrameArgMin, "dur", "r", 1, 0);
    listed.set_by("tags");
    DataFrame with_tags = names_dur({"a", "b"}, {1, 2});
    with_tags.names.push_back("tags");
    with_tags.columns.push_back(tag_lists());
    CHECK_THROWS_AS((void)win(with_tags, {"p"}, {"o"}, {listed}),
                    std::invalid_argument);
    CHECK_THROWS_AS(
        (void)win(in, {"p"}, {"o"},
                  {frame_spec(WindowFunc::FrameVar, "name", "r", 1, 0)}),
        std::invalid_argument);
    CHECK_THROWS_AS(
        (void)win(in, {"p"}, {"o"},
                  {frame_spec(WindowFunc::FrameQuantile, "name", "r", 1, 0)}),
        std::invalid_argument);
}

TEST_CASE("window - frame variance near 1e9 matches a two-pass reference") {
    std::mt19937_64 rng(42);
    std::uniform_real_distribution<double> noise(0.0, 10.0);
    constexpr std::size_t PER = 400;
    std::vector<std::int64_t> p, o;
    std::vector<double> v;
    std::vector<bool> ok;
    for (std::size_t i = 0; i < 3 * PER; ++i) {
        p.push_back(static_cast<std::int64_t>(i / PER));
        o.push_back(static_cast<std::int64_t>((i % PER) / 2));
        v.push_back(1.0e9 + noise(rng));
        ok.push_back(i % 17 != 3);
    }
    DataFrame in;
    in.names = {"p", "o", "v"};
    in.columns.push_back(plain(p));
    in.columns.push_back(plain(o));
    in.columns.push_back(f64s(v, ok));
    const std::vector<std::pair<FrameCase, WindowFrameMode>> cases = {
        {{7, 3, 0}, WindowFrameMode::Rows},
        {{WINDOW_UNBOUNDED, 0, 0}, WindowFrameMode::Rows},
        {{0, WINDOW_UNBOUNDED, 0}, WindowFrameMode::Rows},
        {{WINDOW_UNBOUNDED, WINDOW_UNBOUNDED, 0}, WindowFrameMode::Rows},
        {{50, 0, 0}, WindowFrameMode::Rows},
        {{5, 5, 0}, WindowFrameMode::Range},
        {{WINDOW_UNBOUNDED, 0, 0}, WindowFrameMode::Range},
        {{20, 20, 0}, WindowFrameMode::Range}};
    max_rel_error = 0.0;
    const Vals all = to_vals(v, ok);
    for (const auto& [fc, mode] : cases) {
        std::vector<WindowColumn> specs;
        specs.push_back(stat_spec(WindowFunc::FrameVar, "v", "var", fc, mode));
        specs.push_back(stat_spec(WindowFunc::FrameStd, "v", "std", fc, mode));
        const DataFrame out = win(in, {"p"}, {"o"}, specs);
        Vals want_var(p.size()), want_std(p.size());
        for (std::size_t i = 0; i < p.size(); ++i) {
            Vals cells;
            for (std::size_t j = (i / PER) * PER; j < (i / PER + 1) * PER;
                 ++j) {
                bool inside;
                if (mode == WindowFrameMode::Rows) {
                    const auto pi = static_cast<std::int64_t>(i % PER);
                    const auto pj = static_cast<std::int64_t>(j % PER);
                    inside = (fc.preceding == WINDOW_UNBOUNDED ||
                              pj >= pi - fc.preceding) &&
                             (fc.following == WINDOW_UNBOUNDED ||
                              pj <= pi + fc.following);
                } else {
                    inside = (fc.preceding == WINDOW_UNBOUNDED ||
                              o[j] >= o[i] - fc.preceding) &&
                             (fc.following == WINDOW_UNBOUNDED ||
                              o[j] <= o[i] + fc.following);
                }
                if (inside) cells.push_back(all[j]);
            }
            want_var[i] = variance_ref(cells, false, 0);
            want_std[i] = variance_ref(cells, true, 0);
        }
        check_close(out, "var", want_var, 1e-9);
        check_close(out, "std", want_std, 1e-9);
    }
    MESSAGE("max relative error near 1e9: " << max_rel_error);
    CHECK(max_rel_error <= 1e-9);
}

TEST_CASE("window - ABI new frame functions equal the C++ results") {
    const RandomFrame f = random_frame(77, 120, false);
    const Extra e = random_extra(77, 120);
    const DataFrame in = stat_frame(f, e);
    std::vector<const char*> names;
    std::vector<dftu_series*> cols;
    for (std::size_t i = 0; i < in.names.size(); ++i) {
        names.push_back(in.names[i].c_str());
        cols.push_back(in.columns[i].share().release());
    }
    dftu_dataframe* handle = dftu_dataframe_new(
        names.data(), cols.data(), static_cast<std::int32_t>(names.size()));
    REQUIRE(handle);
    const char* part[1] = {"p"};
    const char* order[1] = {"o"};

    struct Case {
        dftu_window_func func;
        const char* value;
        const char* by;
        double q;
    };
    const Case cases[] = {
        {DFTU_WINDOW_FRAME_VAR, "vf", nullptr, 0.0},
        {DFTU_WINDOW_FRAME_STD, "vi", nullptr, 0.0},
        {DFTU_WINDOW_FRAME_QUANTILE, "vf", nullptr, 0.25},
        {DFTU_WINDOW_FRAME_COUNT_DISTINCT, "vs", nullptr, 0.0},
        {DFTU_WINDOW_FRAME_ARG_MAX, "vs", "bi", 0.0},
        {DFTU_WINDOW_FRAME_ARG_MIN, "vi", "bs", 0.0},
        {DFTU_WINDOW_FRAME_COLLECT, "vs", nullptr, 0.0}};
    for (const dftu_window_frame_mode mode :
         {DFTU_WINDOW_FRAME_ROWS, DFTU_WINDOW_FRAME_RANGE}) {
        for (const Case& c : cases) {
            dftu_window_spec abi{};
            abi.func = c.func;
            abi.value = c.value;
            abi.out = "r";
            abi.param.frame = {1, 3, 2, mode, c.q, c.by};
            dftu_dataframe* got =
                dftu_dataframe_window(handle, part, 1, order, 1, &abi, 1);
            REQUIRE(got);
            const Series col(dftu_dataframe_column(got, "r"));
            REQUIRE(col.valid());
            const DataFrame want =
                win(in, {"p"}, {"o"}, {df::window_column(abi)});
            CHECK(keys_of(col) == keys_of(want.column("r")));
            CHECK(col.type() == want.column("r").type());
            dftu_dataframe_free(got);

            abi.param.frame.q =
                c.func == DFTU_WINDOW_FRAME_QUANTILE ? 2.0 : 0.0;
            if (c.func == DFTU_WINDOW_FRAME_QUANTILE)
                CHECK(dftu_dataframe_window(handle, part, 1, order, 1, &abi,
                                            1) == nullptr);
            if (c.by) {
                abi.param.frame.by = nullptr;
                CHECK(dftu_dataframe_window(handle, part, 1, order, 1, &abi,
                                            1) == nullptr);
            }
        }
    }
    dftu_dataframe_free(handle);
}

TEST_CASE("window - frame variance and std are exactly 0 on constant frames") {
    for (const double base : {0.0, 3.25, 1.0e9}) {
        std::vector<std::int64_t> p, o;
        std::vector<double> v;
        std::vector<bool> ok;
        std::mt19937_64 rng(5);
        std::uniform_real_distribution<double> noise(0.0, 10.0);
        for (int i = 0; i < 90; ++i) {
            p.push_back(1);
            o.push_back(i);
            const bool run = i >= 30 && i < 60;
            v.push_back(run ? base + 0.5 : base + noise(rng));
            ok.push_back(!(run && i % 7 == 0));
        }
        DataFrame in;
        in.names = {"p", "o", "v"};
        in.columns.push_back(plain(p));
        in.columns.push_back(plain(o));
        in.columns.push_back(f64s(v, ok));
        const Vals all = to_vals(v, ok);
        for (const auto& [fc, mode] :
             std::vector<std::pair<FrameCase, WindowFrameMode>>{
                 {{3, 0, 0}, WindowFrameMode::Rows},
                 {{4, 2, 0}, WindowFrameMode::Rows},
                 {{WINDOW_UNBOUNDED, 0, 0}, WindowFrameMode::Rows},
                 {{3, 3, 0}, WindowFrameMode::Range}}) {
            const DataFrame out =
                win(in, {"p"}, {"o"},
                    {stat_spec(WindowFunc::FrameVar, "v", "var", fc, mode),
                     stat_spec(WindowFunc::FrameStd, "v", "std", fc, mode)});
            Vals want_var(90), want_std(90);
            bool saw_constant = false;
            for (int i = 0; i < 90; ++i) {
                Vals cells;
                for (int j = 0; j < 90; ++j) {
                    const bool inside = (fc.preceding == WINDOW_UNBOUNDED ||
                                         j >= i - fc.preceding) &&
                                        (fc.following == WINDOW_UNBOUNDED ||
                                         j <= i + fc.following);
                    if (inside)
                        cells.push_back(all[static_cast<std::size_t>(j)]);
                }
                want_var[static_cast<std::size_t>(i)] =
                    variance_ref(cells, false, 0);
                want_std[static_cast<std::size_t>(i)] =
                    variance_ref(cells, true, 0);
                saw_constant = saw_constant ||
                               want_var[static_cast<std::size_t>(i)] == 0.0;
            }
            if (fc.preceding != WINDOW_UNBOUNDED) CHECK(saw_constant);
            check_close(out, "var", want_var, 1e-9);
            check_close(out, "std", want_std, 1e-9);
        }
    }
}

TEST_CASE("window - frame variance of values drifting past 1e9 stays close") {
    std::mt19937_64 rng(9);
    std::uniform_real_distribution<double> noise(0.0, 1.0);
    std::vector<std::int64_t> p, o;
    std::vector<double> v;
    std::vector<bool> ok;
    for (int i = 0; i < 2000; ++i) {
        p.push_back(1);
        o.push_back(i);
        v.push_back(1.0e9 + 1000.0 * i + noise(rng));
        ok.push_back(true);
    }
    DataFrame in;
    in.names = {"p", "o", "v"};
    in.columns.push_back(plain(p));
    in.columns.push_back(plain(o));
    in.columns.push_back(f64s(v, ok));
    const FrameCase fc{10, 0, 0};
    const DataFrame out = win(in, {"p"}, {"o"},
                              {stat_spec(WindowFunc::FrameVar, "v", "var", fc,
                                         WindowFrameMode::Rows)});
    const Vals all = to_vals(v, ok);
    Vals want(2000);
    for (int i = 0; i < 2000; ++i) {
        Vals cells(all.begin() + std::max(0, i - 10), all.begin() + i + 1);
        want[static_cast<std::size_t>(i)] = variance_ref(cells, false, 0);
    }
    max_rel_error = 0.0;
    const Vals got = nums(out, "var");
    for (std::size_t i = 0; i < got.size(); ++i)
        if (got[i])
            max_rel_error = std::max(max_rel_error,
                                     std::fabs(*got[i] - *want[i]) / *want[i]);
    MESSAGE("drift max relative error: " << max_rel_error);
    CHECK(max_rel_error <= 1e-9);
}

TEST_CASE("lag and lead read the row `offset` back or ahead in the partition") {
    const std::vector<std::int64_t> k = {1, 1, 1, 1, 2, 2, 2};
    const std::vector<std::int64_t> ord = {0, 1, 2, 3, 0, 1, 2};
    const std::vector<std::int64_t> v = {10, 11, 12, 13, 20, 21, 22};
    DataFrame frame;
    frame.names = {"k", "ord", "v"};
    frame.columns.push_back(Series::flat_i64(k.data(), 7));
    frame.columns.push_back(Series::flat_i64(ord.data(), 7));
    frame.columns.push_back(Series::flat_i64(v.data(), 7));
    const std::vector<std::int64_t> offsets = {0, 1, 3, 4, 100, -2};
    std::vector<WindowColumn> specs;
    for (std::int64_t n : offsets) {
        specs.push_back(
            offset_spec(WindowFunc::Lag, "v", "lag" + std::to_string(n), n));
        specs.push_back(
            offset_spec(WindowFunc::Lead, "v", "lead" + std::to_string(n), n));
    }
    const DataFrame out = df::window(frame, {"k"}, {"ord"}, specs);
    const auto start_of = [&](std::size_t r) { return r < 4 ? 0 : 4; };
    const auto size_of = [&](std::size_t r) { return r < 4 ? 4 : 3; };
    for (std::int64_t n : offsets) {
        for (const char* dir : {"lag", "lead"}) {
            const Series col =
                out.column(dir + std::to_string(n)).materialize();
            for (std::size_t r = 0; r < 7; ++r) {
                const std::int64_t local =
                    static_cast<std::int64_t>(r) - start_of(r);
                const std::int64_t at =
                    std::string(dir) == "lag" ? local - n : local + n;
                INFO(dir << " " << n << " row " << r);
                if (at < 0 || at >= size_of(r)) {
                    CHECK(col.is_null(static_cast<std::int64_t>(r)));
                } else {
                    REQUIRE_FALSE(col.is_null(static_cast<std::int64_t>(r)));
                    CHECK(col.data<std::int64_t>()[r] ==
                          v[static_cast<std::size_t>(start_of(r) + at)]);
                }
            }
        }
    }
}
