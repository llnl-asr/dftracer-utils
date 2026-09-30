#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/frame_ops.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numeric>
#include <optional>
#include <random>
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
    w.params.frame = {min_count, preceding, following, mode};
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
