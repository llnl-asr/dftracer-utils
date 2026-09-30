#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/index/indexer.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "test_view_common.h"

namespace df = dftracer::utils::dataframe;

namespace {

dftu_series* plug_twice(const dftu_series* in) {
    const std::int64_t n = dftu_series_length(in);
    std::vector<std::int64_t> v(static_cast<std::size_t>(n));
    const auto* d = static_cast<const std::int64_t*>(dftu_series_data(in));
    for (std::int64_t i = 0; i < n; ++i)
        v[static_cast<std::size_t>(i)] = d[i] * 2;
    return dftu_series_new_flat(DFTU_TYPE_INT64, v.data(), n, nullptr);
}

dftu_series* plug_add2(const dftu_series* a, const dftu_series* b) {
    const std::int64_t n = dftu_series_length(a);
    std::vector<std::int64_t> v(static_cast<std::size_t>(n));
    const auto* x = static_cast<const std::int64_t*>(dftu_series_data(a));
    const auto* y = static_cast<const std::int64_t*>(dftu_series_data(b));
    for (std::int64_t i = 0; i < n; ++i)
        v[static_cast<std::size_t>(i)] = x[i] + y[i];
    return dftu_series_new_flat(DFTU_TYPE_INT64, v.data(), n, nullptr);
}

dftu_series* plug_scale(const dftu_series* in, double f) {
    const std::int64_t n = dftu_series_length(in);
    std::vector<double> v(static_cast<std::size_t>(n));
    const auto* d = static_cast<const std::int64_t*>(dftu_series_data(in));
    for (std::int64_t i = 0; i < n; ++i)
        v[static_cast<std::size_t>(i)] = static_cast<double>(d[i]) * f;
    return dftu_series_new_flat(DFTU_TYPE_FLOAT64, v.data(), n, nullptr);
}

dftu_dataframe* plug_head(const dftu_dataframe* df, int64_t n) {
    return dftu_dataframe_head(df, n);
}

std::vector<double> series_values(const dftu_series* in) {
    const std::int64_t n = dftu_series_length(in);
    std::vector<double> out(static_cast<std::size_t>(n));
    const void* d = dftu_series_data(in);
    for (std::int64_t i = 0; i < n; ++i)
        out[static_cast<std::size_t>(i)] =
            dftu_series_type(in) == DFTU_TYPE_FLOAT64
                ? static_cast<const double*>(d)[i]
                : static_cast<double>(static_cast<const std::int64_t*>(d)[i]);
    return out;
}

double plug_total(const dftu_series* in) {
    double sum = 0;
    for (const double v : series_values(in)) sum += v;
    return sum;
}

double plug_scaled(const dftu_series* in, double f) {
    return plug_total(in) * f;
}

std::int64_t plug_count(const dftu_series* in) {
    return dftu_series_length(in);
}

std::int32_t plug_big(const dftu_series* in, std::int32_t n) {
    for (const double v : series_values(in))
        if (v > n) return 1;
    return 0;
}

dftu_scalar plug_unrun(const dftu_series*, std::int32_t) { return {}; }

struct PluginReducers {
    static constexpr const char* NAMES[] = {"myplug.total", "myplug.scaled",
                                            "myplug.count_i", "myplug.big",
                                            "myplug.unrun"};
    PluginReducers() {
        const dftu_op_desc ops[] = {
            {"myplug.total", DFTU_OP_SIG(F64, SERIES, NONE, NONE),
             reinterpret_cast<const void*>(&plug_total)},
            {"myplug.scaled", DFTU_OP_SIG(F64, SERIES, F64, NONE),
             reinterpret_cast<const void*>(&plug_scaled)},
            {"myplug.count_i", DFTU_OP_SIG(I64, SERIES, NONE, NONE),
             reinterpret_cast<const void*>(&plug_count)},
            {"myplug.big", DFTU_OP_SIG(BOOL, SERIES, I32, NONE),
             reinterpret_cast<const void*>(&plug_big)},
            {"myplug.unrun", DFTU_OP_SIG(SCALAR, SERIES, I32, NONE),
             reinterpret_cast<const void*>(&plug_unrun)}};
        for (const auto& op : ops) REQUIRE(dftu_op_register(&op) == 0);
    }
    ~PluginReducers() {
        for (const char* n : NAMES) dftu_op_unregister(n);
    }
};

struct PluginOps {
    PluginOps() {
        const dftu_op_desc ops[] = {
            {"myplug.twice", DFTU_OP_SIG(SERIES, SERIES, NONE, NONE),
             reinterpret_cast<const void*>(&plug_twice)},
            {"myplug.add2", DFTU_OP_SIG(SERIES, SERIES, SERIES, NONE),
             reinterpret_cast<const void*>(&plug_add2)},
            {"myplug.scale", DFTU_OP_SIG(SERIES, SERIES, F64, NONE),
             reinterpret_cast<const void*>(&plug_scale)},
            {"myplug.head", DFTU_OP_SIG(FRAME, FRAME, I64, NONE),
             reinterpret_cast<const void*>(&plug_head)}};
        for (const auto& op : ops) REQUIRE(dftu_op_register(&op) == 0);
    }
    ~PluginOps() {
        for (const char* n :
             {"myplug.twice", "myplug.add2", "myplug.scale", "myplug.head"})
            dftu_op_unregister(n);
    }
};

View base() {
    const auto& s = shared_trace();
    return View::from_file(s.gz, s.idx);
}

df::DataFrame frame(const View& v) { return run(v.collect()); }

df::DataFrame pipe(const std::string& text, const duql::Params& p = {}) {
    return frame(base().duql(text, p));
}

std::vector<double> nums(const df::DataFrame& f, std::string_view name) {
    std::vector<double> out;
    for (std::int64_t r = 0; r < f.num_rows(); ++r)
        out.push_back(bnum(f, r, name));
    return out;
}

// Row `r` of the list column `name`, as `[a,b]`.
std::string items(const df::DataFrame& f, std::int64_t r,
                  std::string_view name) {
    const df::Series& c = f.columns[static_cast<std::size_t>(bcol(f, name))];
    if (c.is_null(r)) return "null";
    const df::Series l = c.materialize();
    REQUIRE((l.offsets() || l.offsets64()));
    const std::int64_t lo = l.offsets() ? l.offsets()[r] : l.offsets64()[r];
    const std::int64_t hi =
        l.offsets() ? l.offsets()[r + 1] : l.offsets64()[r + 1];
    const df::Series child = l.child(0).materialize();
    std::string out = "[";
    for (std::int64_t i = lo; i < hi; ++i) {
        if (i > lo) out += ",";
        if (child.is_null(i))
            out += "null";
        else if (child.type() == df::TypeId::String)
            out += child.string_at(i);
        else if (child.type() == df::TypeId::Float64)
            out += std::to_string(child.data<double>()[i]);
        else
            out += std::to_string(child.data<std::int64_t>()[i]);
    }
    return out + "]";
}

std::string text(const df::DataFrame& f) {
    std::string out;
    for (std::size_t c = 0; c < f.columns.size(); ++c) {
        out += f.names[c] + ":";
        for (std::int64_t r = 0; r < f.num_rows(); ++r) {
            if (f.columns[c].is_null(r))
                out += "null";
            else if (f.columns[c].type() == df::TypeId::List)
                out += items(f, r, f.names[c]);
            else if (f.columns[c].type() == df::TypeId::String)
                out += bstr(f, r, f.names[c]);
            else
                out += std::to_string(bnum(f, r, f.names[c]));
            out += ",";
        }
    }
    return out;
}

bool fails_with(const std::string& q, std::vector<std::string> words) {
    try {
        (void)frame(base().duql(q));
    } catch (const DFTUtilsException& e) {
        const std::string what = e.what();
        for (const auto& w : words)
            if (what.find(w) == std::string::npos) {
                MESSAGE(what);
                return false;
            }
        return true;
    }
    return false;
}

std::string event(int ts, const std::string& args, int dur = 10) {
    return R"({"ph":"X","name":"op","cat":"POSIX","pid":1,"tid":1,"ts":)" +
           std::to_string(ts) + R"(,"dur":)" + std::to_string(dur) +
           R"(,"args":{)" + args + "}}";
}

View view_of_lines(TestEnvironment& env, const std::vector<std::string>& lines,
                   const std::string& name = "lines.pfw") {
    const std::string pfw = env.get_dir() + "/" + name;
    {
        std::ofstream o(pfw);
        for (const auto& l : lines) o << l << "\n";
    }
    const std::string gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    fs::remove(pfw);
    dftracer::utils::index::Indexer::open({gz}).build();
    return View::from_file(gz, determine_index_path(gz, ""));
}

std::int32_t col_of(const View& v, std::string_view name) {
    const auto names = v.schema();
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i] == name) return static_cast<std::int32_t>(i);
    FAIL("no column ", name);
    return -1;
}

}  // namespace

TEST_SUITE("View duql pipeline") {
    TEST_CASE("derive, sort and take equal the hand-written View calls") {
        const df::DataFrame got =
            pipe(R"(where cat == "POSIX" | derive ms = dur / 1000 | sort -ms)"
                 " | take 5 | select name, ms");

        View hand = base().duql(R"(cat == "POSIX")");
        hand = hand.with_column(
            "ms",
            df::expr_arith(df::ArithOp::Div, df::expr_col(col_of(hand, "dur")),
                           df::expr_lit(std::int64_t{1000})));
        const df::DataFrame want =
            frame(hand.sort_by_multi({"ms"}, std::vector<bool>{true})
                      .head(5)
                      .select({"name", "ms"}));

        CHECK(got.names == std::vector<std::string>{"name", "ms"});
        CHECK(nums(got, "ms") == nums(want, "ms"));
        CHECK(nums(got, "ms") ==
              std::vector<double>{0.039, 0.038, 0.037, 0.036, 0.035});
        CHECK(bstr(got, 0, "name") == "read");
    }

    TEST_CASE("durations take the unit of the field's role") {
        CHECK(pipe("where dur > 30us").num_rows() == 18);
        CHECK(pipe("where dur > 0.03ms").num_rows() == 18);
        CHECK(pipe("where dur between 10us and 0.012ms").num_rows() == 3);
        CHECK(pipe("derive x = 1 | where dur + 1us > 39us").num_rows() == 2);
        CHECK_THROWS_AS(base().duql("where pid > 1ms"), DFTUtilsException);
    }

    TEST_CASE("a duration keeps its unit only where the unit is clear") {
        const auto n = pipe("where dur > 30us").num_rows();
        CHECK(pipe("where coalesce(dur, 0) > 30us").num_rows() == n);
        CHECK(pipe("where if(dur > 0, dur, 0) > 30us").num_rows() == n);
        CHECK(pipe("where -dur < -30us").num_rows() == n);
        CHECK(pipe("where dur - 10 > 20us").num_rows() == n);
        CHECK(pipe("derive gap = dur - 10 | where gap > 20us").num_rows() == n);
        CHECK(pipe("select d = dur | where d > 30us").num_rows() == n);
        CHECK(pipe("rename d = dur | where d > 30us").num_rows() == n);
        CHECK(pipe(R"(where as_time(dur / 1000, "ms") > 30us)").num_rows() ==
              n);
        CHECK(pipe("group name { m = max(dur) } | where m > 30us").num_rows() ==
              pipe("group name { m = max(dur) } | where m > 30").num_rows());
        for (const char* q : {"where dur / 1000 > 30us", "where dur * 2 > 30us",
                              "group name { dur = count() } | where dur > 30us",
                              "group name { c = count() } | where c > 1ms",
                              "derive x = dur / 1000 | where x > 30us"}) {
            CAPTURE(q);
            CHECK_THROWS_AS(base().duql(q), DFTUtilsException);
        }
    }

    TEST_CASE("time functions") {
        const df::DataFrame f = pipe(
            "where cat == \"POSIX\" | derive b = bin(ts, 1ms),"
            " s = to_seconds(dur), n = as_time(dur, \"ms\")"
            " | sort ts | take 12 | select ts, b, s, n");
        CHECK(bnum(f, 0, "b") == 1000);
        CHECK(bnum(f, 11, "ts") == 2100);
        CHECK(bnum(f, 11, "b") == 2000);
        CHECK(bnum(f, 0, "s") == doctest::Approx(10e-6));
        CHECK(bnum(f, 0, "n") == 10000);
    }

    TEST_CASE("calendar time") {
        TestEnvironment env(10);
        const std::int64_t T0 = 1700000000123456;
        const std::int64_t DAY = 86400000000;
        const auto at = [](std::int64_t ts) {
            return R"({"ph":"X","name":"op","cat":"POSIX","pid":1,"tid":1,"ts":)" +
                   std::to_string(ts) + R"(,"dur":10,"args":{}})";
        };
        const View v =
            view_of_lines(env, {at(T0), at(T0 + DAY), at(T0 + 7 * DAY)});

        const df::DataFrame hour = frame(v.duql(
            R"(derive h = date_part(ts, "hour"), )"
            R"(d = format_time(ts, "%F %T.%f") | sort ts | select ts, h, d)"));
        CHECK(bnum(hour, 0, "h") == 22);
        CHECK(bstr(hour, 0, "d") == "2023-11-14 22:13:20.123456");

        const df::DataFrame week = frame(
            v.duql(R"(where ts > 0 | group w = date_part(ts, "day_of_week") )"
                   R"({ n = count() } | sort w)"));
        REQUIRE(week.num_rows() == 2);
        CHECK(bnum(week, 0, "w") == 1);
        CHECK(bnum(week, 0, "n") == 2);
        CHECK(bnum(week, 1, "w") == 2);
        CHECK(bnum(week, 1, "n") == 1);

        const df::DataFrame scan = frame(v.duql(
            R"(where date_part(ts, "year") == 2023 | derive y = date_part(ts, "year"))"));
        CHECK(scan.num_rows() == 3);
        duql::Params p;
        p.emplace("part", duql::LiteralValue{std::string("minute")});
        p.emplace("fmt", duql::LiteralValue{std::string("%H:%M")});
        const df::DataFrame bound =
            frame(v.duql("where ts > 0 | derive m = date_part(ts, $part), "
                         "t = format_time(ts, $fmt) | sort ts | take 1",
                         p));
        CHECK(bnum(bound, 0, "m") == 13);
        CHECK(bstr(bound, 0, "t") == "22:13");

        CHECK(fails_with(R"(derive x = date_part(ts, "fortnight"))",
                         {"fortnight", "iso_week"}));
        CHECK(fails_with(R"(derive x = format_time(ts, "%Q"))", {"%Q"}));
        CHECK(fails_with(R"(derive x = format_time(ts, "%Y%"))", {"'%'"}));
        CHECK(fails_with(R"(derive x = date_part(pid, "hour"))",
                         {"date_part()", "time"}));
        CHECK(fails_with(R"(group name { c = count() } | derive x = )"
                         R"(date_part(c, "hour"))",
                         {"time"}));
        CHECK(fails_with(R"(derive x = date_part(ts))", {"date_part"}));
    }

    TEST_CASE("now is read once per query") {
        TestEnvironment env(10);
        const auto micros = [] {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        };
        const std::int64_t stamp = micros();
        const auto at = [](std::int64_t ts) {
            return R"({"ph":"X","name":"op","cat":"POSIX","pid":1,"tid":1,"ts":)" +
                   std::to_string(ts) + R"(,"dur":10,"args":{}})";
        };
        const View v = view_of_lines(
            env, {at(stamp), at(stamp - 7200000000), at(stamp - 5)});

        const std::int64_t before = micros();
        const df::DataFrame f =
            frame(v.duql("derive a = now(), b = now() | sort ts"));
        const std::int64_t after = micros();
        REQUIRE(f.num_rows() == 3);
        for (std::int64_t r = 0; r < f.num_rows(); ++r) {
            const auto a = static_cast<std::int64_t>(bnum(f, r, "a"));
            CHECK(a == static_cast<std::int64_t>(bnum(f, r, "b")));
            CHECK(a == static_cast<std::int64_t>(bnum(f, 0, "a")));
            CHECK(a >= before);
            CHECK(a <= after);
        }

        const df::DataFrame last =
            frame(v.duql("where ts > now() - 1h | derive a = now() | sort ts"));
        REQUIRE(last.num_rows() == 2);
        CHECK(bnum(last, 0, "ts") == doctest::Approx(double(stamp - 5)));
        CHECK(bnum(last, 1, "ts") == doctest::Approx(double(stamp)));

        const df::DataFrame both = frame(
            v.duql("derive a = now(), b = now() | where ts > now() - 1h"));
        CHECK(both.num_rows() == 2);

        CHECK(fails_with("derive x = now(1)", {"'now' takes 0 arguments"}));
    }

    TEST_CASE("parameters bind counts and values") {
        duql::Params p;
        p.emplace("min", duql::LiteralValue{std::uint64_t{30}});
        p.emplace("n", duql::LiteralValue{std::uint64_t{3}});
        CHECK(pipe("where dur > $min | take $n", p).num_rows() == 3);
    }

    TEST_CASE("list and pattern parameters") {
        duql::Params p;
        p.emplace("names", std::vector<duql::LiteralValue>{
                               std::string("read"), std::string("write")});
        p.emplace("pat", duql::LiteralValue{std::string("re%")});
        p.emplace("one", duql::LiteralValue{std::string("read")});
        CHECK(
            text(pipe("where name in $names | select ts, name", p)) ==
            text(pipe(R"(where name in ["read", "write"] | select ts, name)")));
        CHECK(text(pipe("where name not in $names | select ts", p)) ==
              text(pipe(R"(where name not in ["read", "write"] | select ts)")));
        CHECK(text(pipe("where name like $pat | select ts", p)) ==
              text(pipe(R"(where name like "re%" | select ts)")));
        CHECK(
            text(pipe("select ts, name | where name in $names", p)) ==
            text(pipe(R"(select ts, name | where name in ["read", "write"])")));
        for (const char* q :
             {"where name == $names", "where name in $one",
              "where name like $names", "where name in $nope"}) {
            CAPTURE(q);
            CHECK_THROWS_AS(base().duql(q, p), DFTUtilsException);
        }
    }

    TEST_CASE(
        "regex_replace agrees at scan time, after a group and as a Series") {
        const std::string fn =
            R"(regex_replace(name, "^(?<h>.)(.*)$", "$2${h}"))";
        const df::DataFrame scan = pipe(
            "derive r = " + fn + " | select name, r | distinct | sort name");
        const df::DataFrame grouped =
            pipe("group name { n = count() } | derive r = " + fn +
                 " | sort name | select name, r");
        CHECK(text(scan) == text(grouped));
        REQUIRE(scan.num_rows() > 1);
        const df::Series names = grouped.columns[0].materialize();
        const df::Series want =
            names.str_regex_replace("^(?<h>.)(.*)$", "$2${h}").materialize();
        const df::Series got = grouped.columns[1].materialize();
        for (std::int64_t r = 0; r < grouped.num_rows(); ++r) {
            CAPTURE(r);
            CHECK(std::string(got.string_at(r)) ==
                  std::string(want.string_at(r)));
            const std::string n(names.string_at(r));
            CHECK(std::string(got.string_at(r)) == n.substr(1) + n[0]);
        }
    }

    TEST_CASE("count arguments from a column match the scan evaluator") {
        const df::DataFrame g = pipe(
            "group name { n = count() } | derive a = substr(name, n), "
            "b = substr(name, 1, n), r = round(1.23456, n) | sort name");
        REQUIRE(g.num_rows() > 1);
        for (std::int64_t r = 0; r < g.num_rows(); ++r) {
            CAPTURE(r);
            const std::string name = bstr(g, r, "name");
            const auto n = static_cast<std::size_t>(bnum(g, r, "n"));
            CHECK(bstr(g, r, "a") ==
                  (n < name.size() ? name.substr(n) : std::string()));
            CHECK(bstr(g, r, "b") ==
                  (name.size() > 1 ? name.substr(1, n) : std::string()));
            double want = 1.23456;
            const double scale = std::pow(10.0, static_cast<double>(n));
            want = std::round(want * scale) / scale;
            CHECK(bnum(g, r, "r") == doctest::Approx(want));
        }
    }

    TEST_CASE("a column needle keeps the same rows at scan time and after") {
        const std::string keep = "contains(name, lower(substr(cat, 3, 1)))";
        const df::DataFrame scan = pipe("where " + keep +
                                        " | select name, cat | distinct | "
                                        "sort name");
        const df::DataFrame grouped =
            pipe("group name, cat { n = count() } | where " + keep +
                 " | sort name | select name, cat");
        CHECK(text(scan) == text(grouped));
        CHECK(scan.num_rows() >= 1);
        CHECK(text(scan).find("fwrite") != std::string::npos);
        CHECK(text(scan).find("read,") == std::string::npos);
    }

    TEST_CASE("regex_replace takes parameter patterns and compile errors") {
        duql::Params p;
        p.emplace("re", duql::LiteralValue{std::string("^re")});
        p.emplace("to", duql::LiteralValue{std::string("RE")});
        CHECK(text(pipe("derive r = regex_replace(name, $re, $to) | "
                        "select name, r | distinct | sort name",
                        p)) ==
              text(pipe(R"(derive r = regex_replace(name, "^re", "RE") | )"
                        "select name, r | distinct | sort name")));
        CHECK(fails_with(R"re(derive r = regex_replace(name, "(a)", "$2"))re",
                         {"group 2"}));
        CHECK(fails_with(
            R"re(group name { n = count() } | derive r = regex_replace(name, "(a)", "$"))re",
            {"Invalid replacement"}));
    }

    TEST_CASE("parse adds a String column per named group") {
        TestEnvironment env(10);
        const auto named = [](int ts, const std::string& name) {
            return std::string(R"({"ph":"X","name":")") + name +
                   R"(","cat":"POSIX","pid":1,"tid":1,"ts":)" +
                   std::to_string(ts) + R"(,"dur":10})";
        };
        const View v = view_of_lines(
            env,
            {named(1, "open64_17"), named(2, "read"), named(3, "close"),
             R"({"ph":"X","cat":"POSIX","pid":1,"tid":1,"ts":4,"dur":10})"});
        const auto run_parse = [&](const std::string& q) {
            return frame(v.duql(q + " | sort ts"));
        };

        df::DataFrame f = run_parse(
            R"re(parse name ~ "(?<op>[a-z]+)\d*_(?<fd>\d+)" | select ts, op, fd)re");
        REQUIRE(f.num_rows() == 4);
        CHECK(f.columns[bcol(f, "op")].type() == df::TypeId::String);
        CHECK(bstr(f, 0, "op") == "open");
        CHECK(bstr(f, 0, "fd") == "17");
        for (std::int64_t r = 1; r < 4; ++r) {
            CHECK(f.columns[bcol(f, "op")].is_null(r));
            CHECK(f.columns[bcol(f, "fd")].is_null(r));
        }

        f = run_parse(
            R"re(parse name ~ "(?<op>[a-z]+)(_(?<fd>\d+))?" | select ts, op, fd)re");
        CHECK(bstr(f, 0, "op") == "open");
        CHECK(f.columns[bcol(f, "fd")].is_null(0));
        CHECK(bstr(f, 2, "op") == "close");
        CHECK(f.columns[bcol(f, "fd")].is_null(2));
        CHECK(f.columns[bcol(f, "op")].is_null(3));
        CHECK(f.columns[bcol(f, "fd")].is_null(3));

        duql::Params p;
        p.emplace("re", duql::LiteralValue{std::string("(?<op>[a-z]+)")});
        CHECK(text(frame(
                  v.duql("parse name ~ $re | select ts, op | sort ts", p))) ==
              text(run_parse(
                  R"re(parse name ~ "(?<op>[a-z]+)" | select ts, op)re")));

        f = run_parse(
            R"re(parse name ~ "(?<name>[a-z]+)" | select ts, name)re");
        CHECK(bstr(f, 0, "name") == "open");
        CHECK(bstr(f, 1, "name") == "read");
        CHECK(f.columns[bcol(f, "name")].is_null(3));

        CHECK(
            fails_with(R"re(parse name ~ "(\\w+)_(\\d+)")re", {"named group"}));
    }

    TEST_CASE("a builder pipe runs as its text with its parameters") {
        const duql::Pipe q = duql::Pipe()
                                 .where(duql::c("cat") == "POSIX" &&
                                        duql::c("dur") > duql::param("min"))
                                 .derive({{"ms", duql::c("dur") / 1000}})
                                 .sort({-duql::c("ms")})
                                 .take(duql::param("n"))
                                 .select({"name", "ms"})
                                 .bind("min", std::uint64_t{30})
                                 .bind("n", std::uint64_t{3});
        duql::Params p;
        p.emplace("min", duql::LiteralValue{std::uint64_t{30}});
        p.emplace("n", duql::LiteralValue{std::uint64_t{3}});
        const df::DataFrame want =
            pipe(R"(where cat == "POSIX" and dur > $min | derive ms = dur /)"
                 " 1000 | sort -ms | take $n | select name, ms",
                 p);
        const df::DataFrame got = frame(base().duql(q));
        CHECK(got.names == want.names);
        CHECK(nums(got, "ms") == nums(want, "ms"));
        CHECK(got.num_rows() == 3);
    }

    TEST_CASE("sort keeps nulls last unless nulls first") {
        const std::string q =
            "derive y = if(dur > 35, null, dur) | sort -y{} | take 9"
            " | select y";
        const df::DataFrame last =
            pipe(std::string(q).replace(q.find("{}"), 2, ""));
        CHECK(bnum(last, 0, "y") == 35);
        const df::DataFrame first =
            pipe(std::string(q).replace(q.find("{}"), 2, " nulls first"));
        const auto& y = first.columns[0];
        for (std::int64_t r = 0; r < 8; ++r) CHECK(y.is_null(r));
        CHECK_FALSE(y.is_null(8));
        CHECK(bnum(first, 8, "y") == 35);
    }

    TEST_CASE("skip and take window a sorted result") {
        const df::DataFrame f = pipe("sort ts | skip 45 | take 10 | select ts");
        CHECK(nums(f, "ts") ==
              std::vector<double>{6500, 6600, 6700, 6800, 6900});
    }

    TEST_CASE("named distinct keys, pivot labels and take a..b") {
        CHECK(text(pipe("distinct c = cat | sort c")) ==
              text(pipe("distinct cat | sort cat | rename c = cat")));
        CHECK(text(pipe("distinct dur // 10 as d | sort d")) ==
              text(pipe("derive d = dur // 10 | distinct d | sort d")));

        const df::DataFrame one = pipe(
            R"(pivot name in ["read" as r, "write" as w] { n = count() })");
        CHECK(one.names == std::vector<std::string>{"r", "w"});
        const df::DataFrame plain =
            pipe(R"(pivot name in ["read", "write"] { n = count() })");
        CHECK(nums(one, "r") == nums(plain, "n.read"));
        const df::DataFrame two = pipe(
            R"(pivot name in ["read" as r, "write"] { n = count(), s = sum(dur) })");
        CHECK(two.names ==
              std::vector<std::string>{"n_r", "n.write", "s_r", "s.write"});

        const auto all = nums(pipe("sort ts | select ts"), "ts");
        const auto mid = nums(pipe("sort ts | take 3..5 | select ts"), "ts");
        REQUIRE(mid.size() == 3);
        CHECK(mid == std::vector<double>(all.begin() + 2, all.begin() + 5));
        CHECK(nums(pipe("sort ts | take 1..1 | select ts"), "ts").size() == 1);
        for (const char* q : {"take 0..2", "take 5..3", "take 1..2 by cat"}) {
            CAPTURE(q);
            CHECK_THROWS_AS(base().duql(q), DFTUtilsException);
        }
    }

    TEST_CASE("aggregate expressions in a block") {
        const df::DataFrame f = pipe(
            "group name { n = count(), ms = sum(dur) / 1000, "
            "r = sum(dur) / count(), m = coalesce(max(dur), 0), "
            "k = max(max(dur), 0) } | sort name");
        CHECK(f.names ==
              std::vector<std::string>{"name", "n", "ms", "r", "m", "k"});
        CHECK(text(f) ==
              text(pipe("group name { n = count(), s = sum(dur), "
                        "m = max(dur) } | derive ms = s / 1000, r = s / n, "
                        "m = coalesce(m, 0), k = max(m, 0) | "
                        "select name, n, ms, r, m, k | sort name")));
        const df::DataFrame e =
            pipe("where dur < 0 | agg { r = sum(dur) / count(), n = count() }");
        CHECK(e.names == std::vector<std::string>{"r", "n"});
        REQUIRE(e.num_rows() == 1);
        CHECK(text(e) == text(pipe("where dur < 0 | agg { s = sum(dur), "
                                   "n = count() } | derive r = s / n | "
                                   "select r, n")));
    }

    TEST_CASE("unnamed aggregates and case blocks") {
        const df::DataFrame f =
            pipe("group cat { count(), sum(dur) } | sort cat");
        CHECK(f.names == std::vector<std::string>{"cat", "count", "sum_dur"});
        CHECK(text(f) ==
              text(pipe("group cat { count = count(), sum_dur = sum(dur) } | "
                        "sort cat")));
        for (const char* q :
             {"agg { count(), count() }", "group cat { count(), count() }"}) {
            CAPTURE(q);
            try {
                (void)base().duql(q);
                FAIL("no error");
            } catch (const DFTUtilsException& e) {
                CHECK(std::string(e.what()).find("'count'") !=
                      std::string::npos);
            }
        }

        CHECK(
            text(pipe(
                R"(derive s = case { dur > 500 => "slow", else => "fast" } | select s)")) ==
            text(pipe(
                R"(derive s = case(dur > 500, "slow", "fast") | select s)")));
        CHECK(text(pipe(
                  R"(derive s = case { dur > 500 => "slow" } | select s)")) ==
              text(pipe(
                  R"(derive s = case(dur > 500, "slow", null) | select s)")));
    }

    TEST_CASE("distinct keeps first occurrences of the keys") {
        const df::DataFrame f = pipe("distinct cat | sort cat");
        REQUIRE(f.num_rows() == 2);
        CHECK(f.names == std::vector<std::string>{"cat"});
        CHECK(bstr(f, 0, "cat") == "POSIX");
        CHECK(bstr(f, 1, "cat") == "STDIO");
    }

    TEST_CASE("select, rename and drop shape the columns") {
        const df::DataFrame f =
            pipe("select name, dur | rename d = dur | drop name | take 1");
        CHECK(f.names == std::vector<std::string>{"d"});
        const df::DataFrame g =
            pipe("take 1 | select twice = dur * 2, dur, n = name");
        CHECK(g.names == std::vector<std::string>{"twice", "dur", "n"});
        CHECK(bnum(g, 0, "twice") == 2 * bnum(g, 0, "dur"));
    }

    TEST_CASE("after the scan a missing field is a null column") {
        CHECK(pipe("derive y = 1 | where nope is null").num_rows() == 50);
        CHECK(pipe("derive y = 1 | where nope is missing").num_rows() == 50);
        CHECK(pipe("derive y = 1 | where exists(nope)").num_rows() == 0);
        CHECK(pipe("derive y = 1 | where exists(dur)").num_rows() == 50);
        const df::DataFrame f = pipe("take 1 | derive r = nope + 1");
        CHECK(f.columns[static_cast<std::size_t>(bcol(f, "r"))].is_null(0));
    }

    TEST_CASE("explain lists each step") {
        const std::string plan = base().explain_duql(
            R"(where cat == "POSIX" | derive ms = dur / 1000 | sort -ms)"
            " | take 5");
        CHECK(plan.find("scan filter: cat == \"POSIX\" (pushed)\n") == 0);
        CHECK(plan.find("with_column ms = dur / 1000\n") != std::string::npos);
        CHECK(plan.find("sort_by_multi ms\n") != std::string::npos);
        CHECK(plan.find("head 5\n") != std::string::npos);
    }

    TEST_CASE("explain lists the reshaping steps") {
        const View v = base();
        auto has = [](const std::string& plan, const char* line) {
            INFO(plan);
            CHECK(plan.find(line) != std::string::npos);
        };
        has(v.explain_duql("window name sort -dur { r = row_number() }"),
            "window keys name; sort -dur; r = row_number()\n");
        has(v.explain_duql("expand tags as t with_index i keep_empty"),
            "expand tags as t with_index i keep_empty\n");
        has(v.explain_duql("pivot cat { n = count() }"),
            "pivot cat (columns after the scan)\n");
        has(v.explain_duql("pivot cat in [\"POSIX\"] { n = count() }"),
            "pivot cat in [POSIX]\n");
        has(v.explain_duql("unpivot ts, dur as k, v"),
            "unpivot ts, dur as k, v\n");
        has(v.explain_duql("derive z = 1 | where any(tags, . == \"a\")"),
            "quantify __duql_q_0 = any(tags, . == \"a\")\n");
    }

    TEST_CASE("results do not depend on workers or checkpoint size") {
        const std::vector<std::string> queries = {
            "where dur > 50 | derive x = dur * 2 | skip 7 | take 300"
            " | select ts, x",
            "derive k = dur % 7 | distinct k",
            "sort -(dur % 13), ts | take 40 | select ts",
            "group name { n = count(), f = first(ts) } | take 1 by name",
            "group k = dur % 7 { n = count(), t = sum(dur), l = last(ts) }",
            "group k = dur % 7 { d = count_distinct(dur % 11),"
            " c = collect(ts), a = arg_max(ts, dur % 5), t = sum(dur) }",
            "take 2 by dur % 5 sort -dur | select ts",
            "sample 100 seed 7 | select ts",
            "sample 3% seed 1 | select ts",
            "call_tree | select ts, depth, parent",
        };
        TestEnvironment big(10);
        TestEnvironment small(10);
        const std::vector<std::string> files = {
            create_multimember_trace(big, 3000, 20000),
            create_multimember_trace(small, 3000, 2048)};
        for (const auto& q : queries) {
            INFO("query: ", q);
            std::optional<std::string> want;
            for (const auto& gz : files)
                for (std::size_t workers : {1, 2, 8}) {
                    dftracer::utils::Runtime rt(workers);
                    const View v =
                        View::from_file(gz, determine_index_path(gz, ""))
                            .duql(q);
                    const std::string got = text(rt.submit(v.collect()).get());
                    if (!want) want = got;
                    CHECK_MESSAGE(got == *want, gz, " workers ", workers, ": ",
                                  got.substr(0, 200), " vs ",
                                  want->substr(0, 200));
                }
        }
    }

    TEST_CASE("stages of a later release fail naming it") {
        CHECK(fails_with("where x > 1 | call myplug.sessions(gap = 5)",
                         {"Unknown function 'myplug.sessions'"}));
        CHECK(fails_with("derive c = collect(dur)", {"collect", "aggregate"}));
    }
}

TEST_SUITE("View duql aggregation") {
    TEST_CASE("a bare distinct keeps every column") {
        const df::DataFrame got = pipe("select name, cat | distinct");
        CHECK(got.names == std::vector<std::string>{"name", "cat"});
        CHECK(got.num_rows() == 2);
    }

    TEST_CASE("a select before a trace group keeps the group's columns") {
        const df::DataFrame got =
            pipe("select name | group name { n = count() }");
        CHECK(got.names == std::vector<std::string>{"name", "n"});
        CHECK(text(got) == text(pipe("group name { n = count() }")));
    }

    TEST_CASE("count and time per name equal the trace group_by") {
        const df::DataFrame got =
            pipe("group name { n = count(), t = sum(dur) }");
        CHECK(got.names == std::vector<std::string>{"name", "n", "t"});
        const df::DataFrame want =
            frame(base()
                      .group_by({GroupKey::field("name")})
                      .agg({{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "t"}})
                      .sort_by("name"));
        REQUIRE(got.num_rows() == 2);
        CHECK(text(got) == text(want));
        CHECK(bstr(got, 0, "name") == "fwrite");
        CHECK(bnum(got, 0, "n") == 20);
        CHECK(bnum(got, 1, "t") == 30 * 10 + 29 * 30 / 2);
        CHECK(got.columns[2].type() == df::TypeId::Int64);
    }

    TEST_CASE("an expression key gives true, false and null groups") {
        const df::DataFrame f = pipe(
            "derive y = if(dur > 35, null, dur) | group big = y > 30"
            " { n = count() }");
        REQUIRE(f.num_rows() == 3);
        CHECK(f.names == std::vector<std::string>{"big", "n"});
        CHECK(nums(f, "n") == std::vector<double>{32, 10, 8});
        CHECK(f.columns[0].is_null(2));
    }

    TEST_CASE("agg over no rows gives one row") {
        for (const char* q :
             {"where dur < 0 | agg { n = count(), m = max(dur) }",
              "where dur < 0 | derive d = dur"
              " | agg { n = count(), m = max(d) }"}) {
            INFO(q);
            const df::DataFrame f = pipe(q);
            REQUIRE(f.num_rows() == 1);
            CHECK(bnum(f, 0, "n") == 0);
            CHECK(f.columns[1].is_null(0));
        }
    }

    TEST_CASE("aggregates skip nulls on both plans") {
        TestEnvironment env(10);
        const View v =
            view_of_lines(env, {event(1, R"("x":1)"), event(2, R"("x":null)"),
                                event(3, R"("x":3)")});
        for (const char* q :
             {"agg { n = count(), c = count(x), s = sum(x), m = mean(x) }",
              "derive z = 1 | agg { n = count(), c = count(x), s = sum(x),"
              " m = mean(x) }"}) {
            INFO(q);
            const df::DataFrame f = frame(v.duql(q));
            CHECK(bnum(f, 0, "n") == 3);
            CHECK(bnum(f, 0, "c") == 2);
            CHECK(bnum(f, 0, "s") == 4);
            CHECK(f.columns[2].type() == df::TypeId::Int64);
            CHECK(bnum(f, 0, "m") == 2.0);
        }
    }

    TEST_CASE("quantile is within 1% on both plans") {
        TestEnvironment env(10);
        std::vector<std::string> lines;
        for (int i = 1; i <= 1000; ++i) lines.push_back(event(i, "", i));
        const View v = view_of_lines(env, lines);
        for (const char* q : {"agg { p = quantile(dur, 0.5) }",
                              "derive z = 1 | agg { p = quantile(dur, $q) }"}) {
            INFO(q);
            duql::Params p;
            p.emplace("q", duql::LiteralValue{0.5});
            const double got = bnum(frame(v.duql(q, p)), 0, "p");
            CHECK(got >= 495);
            CHECK(got <= 505);
        }
    }

    TEST_CASE("the trace and frame plans give the same groups") {
        CHECK(text(pipe("group name { n = count(), t = sum(dur) }")) ==
              text(pipe("derive d = dur | group name { n = count(),"
                        " t = sum(d) }")));
    }

    TEST_CASE("every shared aggregate agrees across plans over nulls") {
        TestEnvironment env(10);
        std::vector<std::string> lines;
        for (int i = 0; i < 60; ++i) {
            std::string rec = R"({"ph":"X","name":")" +
                              std::string(i % 2 ? "a" : "b") + R"(","pid":)" +
                              std::to_string(i % 4) + R"(,"tid":1,"ts":)" +
                              std::to_string(100 + i * 10);
            if (i % 3 != 0) rec += R"(,"dur":)" + std::to_string(i * 7 % 23);
            if (i % 5 != 0)
                rec += R"(,"cat":")" + std::string(i % 4 ? "x" : "") + "\"";
            lines.push_back(rec + R"(,"args":{}})");
        }
        const View v = view_of_lines(env, lines);
        const std::string aggs =
            "{ n = count(), su = sum(dur), lo = min(dur), hi = max(dur),"
            " m = mean(dur), va = var(dur), sd = std(dur),"
            " p = quantile(dur, 0.9), h = histogram(dur) }";
        for (const char* keys : {"", "cat", "pid", "cat, pid", "name, cat"}) {
            const std::string stage =
                std::string(*keys ? "group " : "agg ") + keys + " " + aggs;
            INFO(stage);
            const std::string trace_plan = v.explain_duql(stage);
            CHECK(trace_plan.find("(trace plan)") != std::string::npos);
            const std::string frame_plan =
                v.explain_duql("derive z = 1 | " + stage);
            CHECK(frame_plan.find("(frame plan)") != std::string::npos);
            const df::DataFrame a = frame(v.duql(stage));
            const df::DataFrame b = frame(v.duql("derive z = 1 | " + stage));
            REQUIRE(a.names == b.names);
            CHECK(a.num_rows() == b.num_rows());
            for (std::size_t c = 0; c < a.columns.size(); ++c) {
                INFO(a.names[c]);
                CHECK(a.columns[c].type() == b.columns[c].type());
                if (a.names[c] == "h") continue;
                for (std::int64_t r = 0;
                     r < std::min(a.num_rows(), b.num_rows()); ++r) {
                    INFO("row ", r);
                    REQUIRE(a.columns[c].is_null(r) == b.columns[c].is_null(r));
                    if (a.columns[c].is_null(r)) continue;
                    if (a.columns[c].type() == df::TypeId::String)
                        CHECK(bstr(a, r, a.names[c]) == bstr(b, r, b.names[c]));
                    else
                        CHECK(bnum(a, r, a.names[c]) ==
                              doctest::Approx(bnum(b, r, b.names[c])));
                }
            }
        }
    }

    TEST_CASE("first, last, count_if and count(e) run on the columns") {
        const df::DataFrame f = pipe(
            "group cat { f = first(ts), l = last(ts), c = count_if(dur > 20),"
            " e = count(nope) }");
        CHECK(base()
                  .explain_duql("group cat { f = first(ts) }")
                  .find("(frame plan)") != std::string::npos);
        REQUIRE(f.num_rows() == 2);
        CHECK(bnum(f, 0, "f") == 1000);
        CHECK(bnum(f, 0, "l") == 3900);
        CHECK(bnum(f, 0, "c") == 19);
        CHECK(bnum(f, 1, "c") == 19);
        CHECK(bnum(f, 0, "e") == 0);
    }

    TEST_CASE("occupancy needs the trace plan") {
        CHECK(fails_with("derive x = 1 | group name { b = busy() }",
                         {"busy", "derive"}));
        CHECK(pipe("group name { b = busy() }").num_rows() == 2);
    }

    TEST_CASE("an aggregate stands only in a block") {
        CHECK(fails_with("derive c = count()", {"count", "aggregate"}));
        CHECK(fails_with("group name { d = dur }", {"inside an aggregate"}));
        CHECK(fails_with("group name { x = dur + count() }",
                         {"inside an aggregate"}));
        CHECK(fails_with("group name { d = 1 + 2 }", {"aggregate call"}));
        CHECK(fails_with("agg { m = sum(count()) }", {"count", "aggregate"}));
        CHECK(fails_with(R"(pivot name in ["a"] { t = sum(dur) / 2 })",
                         {"one aggregate call"}));
        CHECK(fails_with("agg { q = quantile(dur, 2) }", {"[0, 1]"}));
        CHECK(fails_with("agg { n = count(dur, ts) }", {"count"}));
        CHECK(pipe("agg { t = sum(dur / 1000) }").num_rows() == 1);
    }

    TEST_CASE("top rows per key") {
        const df::DataFrame f =
            pipe("take 2 by name sort -dur | select name, dur");
        CHECK(f.names == std::vector<std::string>{"name", "dur"});
        REQUIRE(f.num_rows() == 4);
        CHECK(nums(f, "dur") == std::vector<double>{39, 39, 38, 38});
        CHECK(bstr(f, 0, "name") == "read");
        CHECK(bstr(f, 1, "name") == "fwrite");
        CHECK(pipe("take 3 by cat").num_rows() == 6);
        CHECK(pipe("take 0 by cat").num_rows() == 0);
        CHECK(nums(pipe("take 1 by dur % 2 | select dur"), "dur") ==
              std::vector<double>{10, 11});
    }

    TEST_CASE("sample keeps rows in input order") {
        const df::DataFrame a = pipe("sample 10 seed 7 | select ts");
        REQUIRE(a.num_rows() == 10);
        const auto ts = nums(a, "ts");
        CHECK(std::is_sorted(ts.begin(), ts.end()));
        CHECK(nums(pipe("sample 10 seed 8 | select ts"), "ts") != ts);
        CHECK(nums(pipe("sample 40% seed 1 | select ts"), "ts") !=
              nums(pipe("sample 40% seed 2 | select ts"), "ts"));
        CHECK(pipe("sample 100%").num_rows() == 50);
        CHECK(pipe("sample 0%").num_rows() == 0);
    }

    TEST_CASE("sample a percent") {
        TestEnvironment env(10);
        const std::string gz = create_multimember_trace(env, 100000, 1 << 20);
        const View v = View::from_file(gz, determine_index_path(gz, ""));
        const std::int64_t n =
            frame(v.duql("sample 10% | select ts")).num_rows();
        CHECK(n >= 9000);
        CHECK(n <= 11000);
    }

    TEST_CASE("time_range pushed and on the columns") {
        const df::DataFrame pushed = pipe("time_range 1ms .. 2ms | select ts");
        CHECK(base().explain_duql("time_range 1ms .. 2ms").find("(pushed)") !=
              std::string::npos);
        CHECK(text(pushed) ==
              text(frame(base().time_range(1000, 2000).select({"ts"}))));
        CHECK(pushed.num_rows() == 10);
        CHECK(
            text(pipe("derive z = 1 | time_range 1000 .. 2000 | select ts")) ==
            text(pushed));
        CHECK(pipe("time_range 1005 .. 1006 overlap").num_rows() == 1);
        CHECK(pipe("time_range 1005 .. 1006").num_rows() == 0);
    }

    TEST_CASE("time clauses fold constants and take one bound") {
        duql::Params p;
        p.emplace("t0", duql::LiteralValue{std::uint64_t{1000}});
        const auto want = text(pipe("time_range 1ms .. 2ms | select ts"));
        CHECK(text(pipe("time_range $t0 .. $t0 + 1ms | select ts", p)) == want);
        CHECK(text(pipe("time_range 2ms - 1ms .. 4ms / 2 | select ts")) ==
              want);
        CHECK(text(pipe("time_range 1ms .. | select ts")) ==
              text(pipe("where ts >= 1000 | select ts")));
        CHECK(text(pipe("time_range .. 2ms | select ts")) ==
              text(pipe("where ts < 2000 | select ts")));
        CHECK(pipe("time_range .. 1006 overlap").num_rows() ==
              pipe("where ts < 1006").num_rows());
        CHECK_THROWS_AS(base().duql("time_range 2ms .. 1ms"),
                        DFTUtilsException);
    }

    TEST_CASE("sample takes parameters and bucket a name") {
        duql::Params p;
        p.emplace("n", duql::LiteralValue{std::uint64_t{10}});
        p.emplace("s", duql::LiteralValue{std::uint64_t{7}});
        CHECK(text(pipe("sample $n seed $s | select ts", p)) ==
              text(pipe("sample 10 seed 7 | select ts")));
        const df::DataFrame named =
            pipe("bucket 1ms as sec | agg { n = count() }");
        const df::DataFrame plain = pipe("bucket 1ms | agg { n = count() }");
        CHECK(named.names[0] == "sec");
        CHECK(nums(named, "sec") == nums(plain, "bucket"));
        CHECK(nums(named, "n") == nums(plain, "n"));
    }

    TEST_CASE("union takes a row set or a file by name") {
        const std::string let =
            R"(let r = where cat == "POSIX" | select ts; select ts | )";
        CHECK(text(pipe(let + "union r")) ==
              text(pipe(let + "union (from r)")));
    }

    TEST_CASE("busy time per bucket clips each event") {
        TestEnvironment env(10);
        const View v = view_of_lines(env, {event(0, "", 3500)});
        const df::DataFrame f =
            frame(v.duql("bucket 1ms | agg { b = busy(), n = count() }"));
        REQUIRE(f.num_rows() == 4);
        CHECK(f.names[0] == "bucket");
        CHECK(nums(f, "bucket") == std::vector<double>{0, 1000, 2000, 3000});
        CHECK(nums(f, "b") == std::vector<double>{1000, 1000, 1000, 500});
        CHECK(bnum(f, 0, "n") == 1);
        for (std::int64_t r = 1; r < 4; ++r)
            CHECK((f.columns[2].is_null(r) || bnum(f, r, "n") == 0));
    }

    TEST_CASE("filled buckets") {
        TestEnvironment env(10);
        const View v =
            view_of_lines(env, {event(0, "", 1), event(3000, "", 1)});
        for (const char* q : {"bucket 1ms fill | agg { n = count() }",
                              "derive z = 1 | bucket 1ms fill"
                              " | agg { n = count(), m = max(dur) }"}) {
            INFO(q);
            const df::DataFrame f = frame(v.duql(q));
            CHECK(nums(f, "bucket") ==
                  std::vector<double>{0, 1000, 2000, 3000});
            CHECK(nums(f, "n") == std::vector<double>{1, 0, 0, 1});
        }
        const df::DataFrame g =
            pipe("bucket 1ms fill | group cat { n = count() }");
        CHECK(g.names == std::vector<std::string>{"bucket", "cat", "n"});
        CHECK(g.num_rows() == 6 * 2);
        CHECK(fails_with("bucket 1ms | derive z = 1", {"bucket", "group"}));
    }

    TEST_CASE("fill stops at DUQL_FILL_MAX_ROWS") {
        ::setenv("DUQL_FILL_MAX_ROWS", "5", 1);
        CHECK_THROWS_AS(
            frame(base().duql("bucket 100us fill | agg { n = count() }")),
            DFTUtilsException);
        ::unsetenv("DUQL_FILL_MAX_ROWS");
    }

    TEST_CASE("fill modes and ranges") {
        struct Cell {
            std::string g;
            int bucket;
            std::optional<double> v;
        };
        const std::vector<Cell> data = {{"a", 1, 10.0},
                                        {"a", 3, std::nullopt},
                                        {"a", 4, 40.0},
                                        {"b", 0, 5.0},
                                        {"b", 6, 65.0}};
        std::vector<std::string> lines;
        for (const auto& c : data) {
            std::string args = R"("g":")" + c.g + "\"";
            if (c.v) args += R"(,"v":)" + std::to_string(int(*c.v));
            lines.push_back(event(c.bucket * 1000, args, 1));
        }
        TestEnvironment env(10);
        const View v = view_of_lines(env, lines);
        const std::string tail =
            " | group grp { m = mean(args.v), n = count() }";
        const auto run_q = [&](const std::string& q) {
            return frame(v.duql("derive grp = args.g | " + q + tail));
        };
        const auto find = [](const df::DataFrame& f, const std::string& g,
                             int bucket) -> std::int64_t {
            for (std::int64_t r = 0; r < f.num_rows(); ++r)
                if (bstr(f, r, "grp") == g &&
                    static_cast<int>(bnum(f, r, "bucket")) == bucket * 1000)
                    return r;
            return -1;
        };
        const auto real = [&](const std::string& g,
                              int b) -> std::optional<Cell> {
            for (const auto& c : data)
                if (c.g == g && c.bucket == b) return c;
            return std::nullopt;
        };
        const auto present = [&](const std::string& g,
                                 int b) -> std::optional<double> {
            const auto c = real(g, b);
            return c ? c->v : std::nullopt;
        };
        const auto before = [&](const std::string& g, int b) {
            for (int i = b - 1; i >= 0; --i)
                if (present(g, i)) return i;
            return -1;
        };
        const auto after = [&](const std::string& g, int b) {
            for (int i = b + 1; i <= 6; ++i)
                if (present(g, i)) return i;
            return -1;
        };
        for (const char* mode : {"", "forward", "linear"}) {
            INFO(mode);
            const df::DataFrame f =
                run_q(std::string("bucket 1ms fill ") + mode);
            CHECK(f.num_rows() == 14);
            for (const char* g : {"a", "b"})
                for (int b = 0; b <= 6; ++b) {
                    INFO(g << " " << b);
                    const std::int64_t r = find(f, g, b);
                    REQUIRE(r >= 0);
                    const auto c = real(g, b);
                    CHECK(bnum(f, r, "n") == (c ? 1 : 0));
                    const int lo = before(g, b);
                    const int hi = after(g, b);
                    std::optional<double> want;
                    if (c) {
                        want = c->v;
                    } else if (std::string(mode) == "forward" && lo >= 0) {
                        want = present(g, lo);
                    } else if (std::string(mode) == "linear" && lo >= 0 &&
                               hi >= 0) {
                        want = *present(g, lo) +
                               (*present(g, hi) - *present(g, lo)) * (b - lo) /
                                   (hi - lo);
                    }
                    const std::int64_t col = bcol(f, "m");
                    if (want) {
                        REQUIRE_FALSE(
                            f.columns[static_cast<std::size_t>(col)].is_null(
                                r));
                        CHECK(bnum(f, r, "m") == doctest::Approx(*want));
                    } else {
                        CHECK(f.columns[static_cast<std::size_t>(col)].is_null(
                            r));
                    }
                }
        }

        const df::DataFrame wide = frame(
            v.duql("bucket 1ms fill from 0 to 8ms | agg { n = count() }"));
        CHECK(nums(wide, "bucket") == std::vector<double>{0, 1000, 2000, 3000,
                                                          4000, 5000, 6000,
                                                          7000, 8000});
        CHECK(nums(wide, "n") ==
              std::vector<double>{1, 1, 0, 1, 1, 0, 1, 0, 0});
        const df::DataFrame narrow =
            frame(v.duql("bucket 1ms fill from 2ms to 3500us"
                         " | agg { n = count() }"));
        CHECK(nums(narrow, "bucket") ==
              std::vector<double>{0, 1000, 2000, 3000, 4000, 6000});
        CHECK(nums(narrow, "n") == std::vector<double>{1, 1, 0, 1, 1, 1});
        duql::Params p;
        p.emplace("lo", duql::LiteralValue{std::uint64_t{4000}});
        p.emplace("hi", duql::LiteralValue{std::uint64_t{7000}});
        const df::DataFrame par =
            frame(v.duql("bucket 1ms fill forward from $lo to $hi + 1ms"
                         " | agg { m = max(args.v), n = count() }",
                         p));
        CHECK(nums(par, "bucket") ==
              std::vector<double>{0, 1000, 3000, 4000, 5000, 6000, 7000, 8000});
        CHECK(nums(par, "n") == std::vector<double>{1, 1, 1, 1, 0, 1, 0, 0});
        CHECK(bnum(par, 4, "m") == 40);
        CHECK(bnum(par, 6, "m") == 65);
        CHECK(bnum(par, 7, "m") == 65);
        CHECK(
            fails_with("bucket 1ms fill from 5ms to 2ms | agg { n = count() }",
                       {"lo <= hi"}));
        ::setenv("DUQL_FILL_MAX_ROWS", "5", 1);
        CHECK_THROWS_AS(
            frame(
                v.duql("bucket 1ms fill from 0 to 8ms | agg { n = count() }")),
            DFTUtilsException);
        ::unsetenv("DUQL_FILL_MAX_ROWS");
    }

    struct HopRow {
        int t;
        const char* g;
        int size;
    };
    using HopCell = std::pair<std::int64_t, std::int64_t>;

    // Brute force: each window start T + k * E that holds the time.
    static std::map<std::pair<std::int64_t, std::string>, HopCell>
    hop_reference(const std::vector<HopRow>& rows, int W, int E, int T) {
        std::map<std::pair<std::int64_t, std::string>, HopCell> out;
        for (const auto& r : rows)
            for (int k = -200; k <= 200; ++k) {
                const int s = T + k * E;
                if (s <= r.t && r.t < s + W) {
                    auto& c = out[{s, r.g}];
                    c.first += r.size;
                    c.second += 1;
                }
            }
        return out;
    }

    static std::vector<std::string> hop_lines(const std::vector<HopRow>& rows) {
        std::vector<std::string> lines;
        for (const auto& r : rows)
            lines.push_back(event(r.t,
                                  std::string(R"("g":")") + r.g +
                                      R"(","size":)" + std::to_string(r.size),
                                  1));
        return lines;
    }

    const std::vector<HopRow> HOP_ROWS = {{0, "a", 1},    {1000, "b", 2},
                                          {2000, "a", 3}, {3000, "b", 4},
                                          {3500, "a", 5}, {9000, "b", 6}};

    TEST_CASE("hopping buckets: the moving rate") {
        TestEnvironment env(10);
        const View v = view_of_lines(
            env, {event(0, R"("size":1)"), event(1000, R"("size":2)"),
                  event(2000, R"("size":3)"), event(3000, R"("size":4)")});
        const df::DataFrame f =
            frame(v.duql("bucket 2ms every 1ms | agg { b = sum(args.size),"
                         " n = count() }"));
        CHECK(nums(f, "bucket") ==
              std::vector<double>{-1000, 0, 1000, 2000, 3000});
        CHECK(nums(f, "b") == std::vector<double>{1, 3, 5, 7, 4});
        CHECK(nums(f, "n") == std::vector<double>{1, 2, 2, 2, 1});
        const df::DataFrame o =
            frame(v.duql("bucket 2ms at 1ms | agg { n = count() }"));
        CHECK(nums(o, "bucket") == std::vector<double>{-1000, 1000, 3000});
        CHECK(nums(o, "n") == std::vector<double>{1, 2, 1});
        CHECK(
            text(frame(v.duql("bucket 1ms every 1ms | agg { n = count() }"))) ==
            text(frame(v.duql("bucket 1ms | agg { n = count() }"))));
    }

    TEST_CASE("hopping buckets match a brute-force reference") {
        TestEnvironment env(10);
        const View v = view_of_lines(env, hop_lines(HOP_ROWS));
        struct Shape {
            const char* q;
            int W, E, T;
        };
        const std::vector<Shape> shapes = {
            {"bucket 2ms every 1ms", 2000, 1000, 0},
            {"bucket 3ms every 2ms", 3000, 2000, 0},
            {"bucket 1ms every 2ms", 1000, 2000, 0},
            {"bucket 2ms every 1ms at 500us", 2000, 1000, 500},
            {"bucket 2000us every 1000us at 1500us", 2000, 1000, 1500}};
        for (const auto& sh : shapes) {
            INFO(sh.q);
            const auto want = hop_reference(HOP_ROWS, sh.W, sh.E, sh.T);
            const df::DataFrame f = frame(
                v.duql(std::string("derive grp = args.g | ") + sh.q +
                       " | group grp { b = sum(args.size), n = count() }"));
            REQUIRE(static_cast<std::size_t>(f.num_rows()) == want.size());
            std::int64_t r = 0;
            for (const auto& [key, cell] : want) {
                CHECK(bstr(f, r, "grp") == key.second);
                CHECK(static_cast<std::int64_t>(bnum(f, r, "bucket")) ==
                      key.first);
                CHECK(static_cast<std::int64_t>(bnum(f, r, "b")) == cell.first);
                CHECK(static_cast<std::int64_t>(bnum(f, r, "n")) ==
                      cell.second);
                ++r;
            }
            std::set<std::int64_t> seen;
            for (const auto& [key, cell] : want) seen.insert(key.first);
            const df::DataFrame all =
                frame(v.duql(std::string(sh.q) + " | agg { n = count() }"));
            CHECK(static_cast<std::size_t>(all.num_rows()) == seen.size());
        }
    }

    TEST_CASE("hopping buckets take durations and parameters") {
        TestEnvironment env(10);
        const View v = view_of_lines(env, hop_lines(HOP_ROWS));
        duql::Params p;
        p.emplace("w", duql::LiteralValue{std::uint64_t{3000}});
        p.emplace("e", duql::LiteralValue{std::uint64_t{2000}});
        p.emplace("t", duql::LiteralValue{std::uint64_t{500}});
        const auto want = hop_reference(HOP_ROWS, 3000, 2000, 500);
        for (const char* q :
             {"bucket $w every $e at $t | agg { n = count() }",
              "bucket 3ms every 2000us at 0.5ms | agg { n = count() }"}) {
            INFO(q);
            const df::DataFrame f = frame(v.duql(q, p));
            std::map<std::int64_t, std::int64_t> by_start;
            for (const auto& [key, cell] : want)
                by_start[key.first] += cell.second;
            REQUIRE(static_cast<std::size_t>(f.num_rows()) == by_start.size());
            std::int64_t r = 0;
            for (const auto& [start, n] : by_start) {
                CHECK(static_cast<std::int64_t>(bnum(f, r, "bucket")) == start);
                CHECK(static_cast<std::int64_t>(bnum(f, r, "n")) == n);
                ++r;
            }
        }
        CHECK(fails_with("bucket 0 every 1ms | agg { n = count() }",
                         {"positive"}));
        CHECK(fails_with("bucket 1ms every 0 | agg { n = count() }",
                         {"positive"}));
    }

    TEST_CASE("hopping buckets fill the E grid") {
        TestEnvironment env(10);
        const View v =
            view_of_lines(env, {event(0, "", 1), event(4500, "", 1)});
        const df::DataFrame f =
            frame(v.duql("bucket 1ms every 2ms fill | agg { n = count() }"));
        CHECK(nums(f, "bucket") == std::vector<double>{0, 2000, 4000});
        CHECK(nums(f, "n") == std::vector<double>{1, 0, 1});
        const df::DataFrame g =
            frame(v.duql("bucket 2ms every 2ms at 1ms fill from 0 to 7ms"
                         " | agg { n = count() }"));
        CHECK(nums(g, "bucket") ==
              std::vector<double>{-1000, 1000, 3000, 5000, 7000});
        CHECK(nums(g, "n") == std::vector<double>{1, 0, 1, 0, 0});
        const df::DataFrame h = frame(v.duql(
            "bucket 2ms every 1ms fill forward | agg { m = max(dur), n = "
            "count() }"));
        CHECK(nums(h, "bucket") ==
              std::vector<double>{-1000, 0, 1000, 2000, 3000, 4000});
        CHECK(nums(h, "n") == std::vector<double>{1, 1, 0, 0, 1, 1});
        CHECK(nums(h, "m") == std::vector<double>{1, 1, 1, 1, 1, 1});
    }

    TEST_CASE("hopping buckets drop a row without a time") {
        TestEnvironment env(10);
        const View v = view_of_lines(
            env, {event(0, "", 1), event(500, "", 7), event(1000, "", 1)});
        const df::DataFrame f =
            frame(v.duql("derive ts = if(dur == 7, null, ts)"
                         " | bucket 2ms every 1ms | agg { n = count() }"));
        CHECK(nums(f, "bucket") == std::vector<double>{-1000, 0, 1000});
        CHECK(nums(f, "n") == std::vector<double>{1, 2, 1});
    }

    TEST_CASE("a bucket origin gives the same rows on both plans") {
        TestEnvironment env(10);
        const View v = view_of_lines(env, hop_lines(HOP_ROWS));
        const df::DataFrame trace = frame(v.duql(
            "bucket 2ms at 500us | agg { n = count(), s = sum(args.size) }"));
        const df::DataFrame fold = frame(
            v.duql("bucket 2ms at 500us | agg { n = count(),"
                   " s = sum(args.size), d = count_distinct(args.size) }"));
        CHECK(nums(trace, "bucket") == nums(fold, "bucket"));
        CHECK(nums(trace, "n") == nums(fold, "n"));
        CHECK(nums(trace, "s") == nums(fold, "s"));
        std::vector<double> want;
        std::map<std::int64_t, double> by;
        for (const auto& r : HOP_ROWS) {
            const std::int64_t s =
                (r.t - 500) / 2000 * 2000 + 500 - (r.t < 500 ? 2000 : 0);
            by[s] += 1;
        }
        for (const auto& [s, n] : by) want.push_back(n);
        CHECK(nums(trace, "n") == want);
        const std::string plan =
            base().explain_duql("bucket 2ms at 500us | agg { n = count() }");
        CHECK(plan.find("time_bucket 2000 us at 500 us\n") !=
              std::string::npos);
    }

    TEST_CASE(
        "a bucket width that is not a whole microsecond leaves the trace "
        "plan") {
        TestEnvironment env(10);
        const View v = view_of_lines(env, hop_lines(HOP_ROWS));
        const df::DataFrame plain =
            frame(v.duql("bucket 1500ns | agg { n = count() }"));
        const df::DataFrame fold =
            frame(v.duql("bucket 1500ns | agg { n = count(), d = "
                         "count_distinct(args.size) }"));
        CHECK(nums(plain, "bucket") == nums(fold, "bucket"));
        CHECK(nums(plain, "n") == nums(fold, "n"));
        CHECK(base()
                  .explain_duql("bucket 1500ns | agg { n = count() }")
                  .find("group (frame plan)") != std::string::npos);
    }

    TEST_CASE("hopping buckets explain and refuse occupancy") {
        const std::string plan = base().explain_duql(
            "bucket 2ms every 1ms at 500us | agg { n = count() }");
        CHECK(plan.find("hop buckets 2000 every 1000 at 500\n") !=
              std::string::npos);
        CHECK(plan.find("group (frame plan)") != std::string::npos);
        CHECK(fails_with("bucket 5s every 1s | agg { u = utilization() }",
                         {"utilization", "hopping bucket"}));
        CHECK(fails_with("bucket 5s every 1s | agg { b = busy() }",
                         {"busy", "hopping bucket"}));
    }

    TEST_CASE("a trace stage needs its role") {
        const View generic = base().record_schema("generic");
        try {
            (void)generic.duql("time_range 1s .. 2s");
            FAIL("expected a failure");
        } catch (const DFTUtilsException& e) {
            CHECK(std::string(e.what()).find("time role") != std::string::npos);
        }
    }

    TEST_CASE("call_tree gives depth and parent") {
        const df::DataFrame f = pipe("call_tree");
        const df::DataFrame want = run(base().call_tree().collect());
        CHECK(f.num_rows() == want.num_rows());
        CHECK(bhas(f, "depth"));
        CHECK(bhas(f, "parent"));
        CHECK(nums(f, "depth") == nums(want, "level"));
        CHECK(nums(f, "parent") == nums(want, "parent_id"));
        CHECK(fails_with("derive z = 1 | call_tree", {"call_tree"}));
    }

    TEST_CASE("explain names each new step") {
        const std::string plan = base().explain_duql(
            "time_range 0 .. 10s | bucket 1ms | group cat { n = count() }"
            " | take 1 by cat | sample 5 seed 3");
        CHECK(plan.find("scan time_range 0 .. 10000000 (pushed)\n") !=
              std::string::npos);
        CHECK(plan.find("time_bucket 1000 us\n") != std::string::npos);
        CHECK(plan.find("group (trace plan): keys cat; aggs n = count()\n") !=
              std::string::npos);
        CHECK(plan.find("head_by cat 1\n") != std::string::npos);
        CHECK(plan.find("sample 5 seed 3\n") != std::string::npos);
    }

    TEST_CASE("a group takes parameters") {
        duql::Params p;
        p.emplace("min", duql::LiteralValue{std::uint64_t{30}});
        const df::DataFrame f =
            pipe("where dur > $min | group name { n = count() }", p);
        CHECK(nums(f, "n") == std::vector<double>{9, 9});
    }
}

namespace {

std::string cell(const df::Series& s, std::int64_t r) {
    if (s.is_null(r)) return "null";
    if (s.type() == df::TypeId::String) return std::string(s.string_at(r));
    if (s.type() == df::TypeId::Bool)
        return s.cast(df::TypeId::Int64).data<std::int64_t>()[r] ? "true"
                                                                 : "false";
    const double v = s.cast(df::TypeId::Float64).data<double>()[r];
    if (v == static_cast<double>(static_cast<std::int64_t>(v)))
        return std::to_string(static_cast<std::int64_t>(v));
    return std::to_string(v);
}

// Every row as `a,b`, rows joined by `;`.
std::string rows(const df::DataFrame& f) {
    std::string out;
    for (std::int64_t r = 0; r < f.num_rows(); ++r) {
        if (r) out += ';';
        for (std::size_t c = 0; c < f.columns.size(); ++c) {
            if (c) out += ',';
            out += cell(f.columns[c], r);
        }
    }
    return out;
}

std::string sorted(const df::DataFrame& f) {
    std::vector<std::string> out;
    for (std::int64_t r = 0; r < f.num_rows(); ++r)
        out.push_back(cell(f.columns.front(), r));
    std::sort(out.begin(), out.end());
    std::string text;
    for (const auto& o : out) text += o + ";";
    return text;
}

std::string line(const std::string& cat, int ts, const std::string& args) {
    return R"({"ph":"X","name":"op","cat":")" + cat +
           R"(","pid":1,"tid":1,"ts":)" + std::to_string(ts) +
           R"(,"dur":10,"args":{)" + args + "}}";
}

// `where` as the scan filter and as a stage after the scan.
std::vector<std::string> both(const std::string& cond) {
    return {"where " + cond + " | select ts",
            "derive z = 1 | where " + cond + " | select ts"};
}

}  // namespace

TEST_SUITE("View duql quantifiers") {
    TEST_CASE("an element path and a record path in one condition") {
        TestEnvironment env;
        const View v = view_of_lines(
            env, {line("io", 1, R"("hosts":[{"up":false},{"up":true}])"),
                  line("net", 2, R"("hosts":[{"up":true}])"),
                  line("io", 3, R"("hosts":[{"up":false}])")});
        for (const auto& q : both(R"(any(hosts, .up and ^.cat == "io"))")) {
            INFO(q);
            CHECK(rows(frame(v.duql(q))) == "1");
        }
    }

    TEST_CASE("empty and missing arrays follow the any and all rules") {
        TestEnvironment env;
        const View v = view_of_lines(
            env, {line("io", 1, R"("h":[])"), line("io", 2, R"("h":[1,5])"),
                  line("io", 3, R"("x":1)")});
        for (const auto& [cond, want] :
             std::vector<std::pair<std::string, std::string>>{
                 {"all(h, . > 0)", "1;2"},
                 {"any(h, . > 3)", "2"},
                 {"not all(h, . > 0)", ""},
                 {"not any(h, . > 3)", "1"},
                 {"all(h) > 0", "1;2"},
                 {"any(h) == 5", "2"}})
            for (const auto& q : both(cond)) {
                INFO(q);
                CHECK(rows(frame(v.duql(q))) == want);
            }
    }

    TEST_CASE("an unknown element makes any unknown unless one holds") {
        // h mixes numbers and strings.
        TestEnvironment env;
        const View v = view_of_lines(env, {line("io", 1, R"("h":[1,"a"])"),
                                           line("io", 2, R"("h":[9,"a"])"),
                                           line("io", 3, R"("h":[1,2])")});
        for (const auto& [cond, want] :
             std::vector<std::pair<std::string, std::string>>{
                 {"any(h, . > 3)", "2"},
                 {"not any(h, . > 3)", "3"},
                 {"all(h, . < 3)", "3"},
                 {"not all(h, . < 3)", "2"}}) {
            INFO(cond);
            CHECK(rows(frame(v.duql(both(cond).front()))) == want);
            // After the scan, elements of mixed types have no column kernel.
            CHECK_THROWS_WITH_AS(frame(v.duql(both(cond).back())),
                                 doctest::Contains("mix types"),
                                 DFTUtilsException);
        }
    }

    TEST_CASE("an indexed subject reads the inner array") {
        TestEnvironment env;
        const View v = view_of_lines(env, {line("io", 1, R"("g":[[1,2],[3]])"),
                                           line("io", 2, R"("g":[[4],[2]])")});
        for (const auto& q : both("any(g[0], . == 2)")) {
            INFO(q);
            CHECK(rows(frame(v.duql(q))) == "1");
        }
        CHECK(rows(frame(v.duql(both("any(g[-1], . == 2)").front()))) == "2");
        CHECK_THROWS_WITH_AS(frame(v.duql(both("any(g[-1], . == 2)").back())),
                             doctest::Contains("negative index"),
                             DFTUtilsException);
    }

    TEST_CASE("a quantifier in a derive") {
        TestEnvironment env;
        const View v = view_of_lines(
            env, {line("io", 1, R"("sizes":[10,200])"),
                  line("io", 2, R"("sizes":[10])"), line("io", 3, "")});
        CHECK(rows(frame(v.duql(
                  "derive big = any(sizes, . > 100) | select ts, big"))) ==
              "1,true;2,false;3,null");
    }
}

TEST_SUITE("View duql window") {
    TEST_CASE("rank within a file keeps the top rows in input order") {
        const df::DataFrame all = pipe("select fhash, dur");
        const df::DataFrame got = pipe(
            "window fhash sort -dur { r = row_number() } | where r <= 3"
            " | select fhash, dur, r");
        // Reference: per fhash, the three largest dur (ties by input order).
        std::map<std::string, std::vector<std::pair<double, std::int64_t>>> by;
        for (std::int64_t r = 0; r < all.num_rows(); ++r)
            by[cell(all.columns[0], r)].emplace_back(bnum(all, r, "dur"), r);
        std::map<std::int64_t, std::int64_t> rank;
        for (auto& [k, v] : by) {
            std::stable_sort(v.begin(), v.end(),
                             [](auto a, auto b) { return a.first > b.first; });
            for (std::size_t i = 0; i < v.size() && i < 3; ++i)
                rank[v[i].second] = static_cast<std::int64_t>(i) + 1;
        }
        std::string want;
        for (const auto& [row, r] : rank) {
            if (!want.empty()) want += ';';
            want += cell(all.columns[0], row) + "," +
                    cell(all.columns[1], row) + "," + std::to_string(r);
        }
        CHECK(rows(got) == want);
    }

    TEST_CASE("the gap to the previous call") {
        TestEnvironment env;
        const View v = view_of_lines(
            env, {event(100, ""), event(150, ""), event(400, "")});
        CHECK(rows(frame(v.duql("window pid, tid sort ts { g = ts - lag(ts) }"
                                " | select g"))) == "null;50;250");
    }

    TEST_CASE("a share of the partition sums to one") {
        const df::DataFrame f = pipe(
            "window name { t = sum(dur) } | derive share = dur / t"
            " | select name, share");
        std::map<std::string, double> sums;
        for (std::int64_t r = 0; r < f.num_rows(); ++r)
            sums[cell(f.columns[0], r)] += bnum(f, r, "share");
        REQUIRE_FALSE(sums.empty());
        for (const auto& [name, total] : sums)
            CHECK(total == doctest::Approx(1));
        CHECK(f.num_rows() == pipe("select name").num_rows());
    }

    TEST_CASE("every window function against a reference") {
        TestEnvironment env;
        // g: a a b a b; x: 3 null 5 3 1; ts ascending.
        const View v = view_of_lines(
            env, {event(1, R"("g":"a","x":3)"), event(2, R"("g":"a")"),
                  event(3, R"("g":"b","x":5)"), event(4, R"("g":"a","x":3)"),
                  event(5, R"("g":"b","x":1)")});
        const auto run_q = [&](const std::string& entry) {
            return rows(frame(
                v.duql("window g sort -x { v = " + entry + " } | select v")));
        };
        // Order within a by -x (nulls last): rows 1, 4, 2; within b: 3, 5.
        CHECK(run_q("row_number()") == "1;3;1;2;2");
        CHECK(run_q("rank()") == "1;3;1;1;2");
        CHECK(run_q("dense_rank()") == "1;2;1;1;2");
        CHECK(run_q("lag(x)") == "null;3;null;3;5");
        CHECK(run_q("lead(x)") == "3;null;1;null;null");
        CHECK(run_q("lag(x, 2)") == "null;3;null;null;null");
        CHECK(run_q("running_sum(x)") == "3;6;5;6;6");
        CHECK(run_q("running_count()") == "1;3;1;2;2");
        CHECK(run_q("count()") == "3;3;2;3;2");
        CHECK(run_q("count(x)") == "2;2;2;2;2");
        CHECK(run_q("sum(x)") == "6;6;6;6;6");
        CHECK(run_q("min(x)") == "3;3;1;3;1");
        CHECK(run_q("max(x)") == "3;3;5;3;5");
        CHECK(run_q("mean(x)") == "3;3;3;3;3");
        CHECK(run_q("first(x)") == "3;3;5;3;5");
        CHECK(run_q("last(x)") == "3;3;1;3;1");
        CHECK(run_q("var(x)") == "0;0;8;0;8");
        // The rest equal `group` on each partition, on every row in order.
        const auto by_group = [&](const std::string& agg) {
            const df::DataFrame g =
                frame(v.duql("group g { v = " + agg + " } | select g, v"));
            std::map<std::string, std::string> value;
            for (std::int64_t r = 0; r < g.num_rows(); ++r)
                value[cell(g.columns[0], r)] = cell(g.columns[1], r);
            const df::DataFrame w = frame(
                v.duql("window g sort -x { v = " + agg + " } | select g, v"));
            std::string out;
            for (std::int64_t r = 0; r < w.num_rows(); ++r) {
                CHECK(cell(w.columns[1], r) == value[cell(w.columns[0], r)]);
                out += (out.empty() ? "" : ";") + cell(w.columns[0], r);
            }
            return out;
        };
        CHECK(by_group("std(x)") == "a;a;b;a;b");
        CHECK(by_group("quantile(x, 0.5)") == "a;a;b;a;b");
        CHECK(rows(frame(v.duql("window g { h = histogram(x) } | derive n = "
                                "len(h) | select n"))) ==
              rows(frame(v.duql("group g { h = histogram(x) } | derive n = "
                                "len(h) | select g, n"))
                       .select({"n"})
                       .take(std::vector<std::int64_t>{0, 0, 1, 0, 1})));
        CHECK(rows(frame(v.duql("window { n = count(), r = rank() }"
                                " | select n, r"))) == "5,1;5,1;5,1;5,1;5,1");
    }

    struct Pt {
        std::string g;
        std::int64_t key;
        std::optional<std::int64_t> x;
    };

    static std::string shown(std::optional<double> v) {
        if (!v) return "null";
        if (*v == static_cast<double>(static_cast<std::int64_t>(*v)))
            return std::to_string(static_cast<std::int64_t>(*v));
        return std::to_string(*v);
    }

    static std::string aggregated(const std::string& fn,
                                  const std::vector<double>& in) {
        if (fn == "count") return std::to_string(in.size());
        if (in.empty()) return "null";
        double sum = 0;
        for (const double v : in) sum += v;
        if (fn == "sum") return shown(sum);
        if (fn == "mean") return shown(sum / static_cast<double>(in.size()));
        return shown(fn == "min" ? *std::min_element(in.begin(), in.end())
                                 : *std::max_element(in.begin(), in.end()));
    }

    static View points_view(TestEnvironment & env, const std::vector<Pt>& pts,
                            bool key_in_ts) {
        std::vector<std::string> lines;
        for (const auto& p : pts) {
            std::string args =
                R"("g":")" + p.g + R"(","k":)" + std::to_string(p.key);
            if (p.x) args += R"(,"x":)" + std::to_string(*p.x);
            lines.push_back(event(key_in_ts
                                      ? static_cast<int>(p.key)
                                      : static_cast<int>(lines.size()) + 1,
                                  args));
        }
        return view_of_lines(env, lines);
    }

    // `fn(x)` per row over the rows `in_frame(i, j)` selects.
    template <class InFrame>
    std::string framed_ref(const std::vector<Pt>& pts, const std::string& fn,
                           InFrame in_frame) {
        std::string out;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            std::vector<double> present;
            for (std::size_t j = 0; j < pts.size(); ++j)
                if (pts[j].g == pts[i].g && in_frame(i, j) && pts[j].x)
                    present.push_back(static_cast<double>(*pts[j].x));
            out += (i ? ";" : "") + aggregated(fn, present);
        }
        return out;
    }

    TEST_CASE("rows frames against a reference") {
        TestEnvironment env;
        const std::vector<Pt> pts = {
            {"a", 1, 4}, {"a", 2, std::nullopt}, {"a", 3, 7}, {"b", 4, 2},
            {"a", 5, 1}, {"b", 6, std::nullopt}, {"b", 7, 9}, {"a", 8, 5}};
        const View v = points_view(env, pts, false);
        std::vector<std::size_t> at(pts.size());
        std::map<std::string, std::size_t> seen;
        for (std::size_t i = 0; i < pts.size(); ++i) at[i] = seen[pts[i].g]++;
        for (const std::int64_t n : {1, 3, 20}) {
            for (const char* fn : {"sum", "mean", "min", "max", "count"}) {
                const std::string want =
                    framed_ref(pts, fn, [&](std::size_t i, std::size_t j) {
                        return at[j] <= at[i] &&
                               static_cast<std::int64_t>(at[j]) >
                                   static_cast<std::int64_t>(at[i]) - n;
                    });
                const std::string q =
                    "window g sort ts { v = " + std::string(fn) + "(x) over " +
                    std::to_string(n) + " rows } | select v";
                CHECK_MESSAGE(rows(frame(v.duql(q))) == want, q);
            }
        }
        CHECK(rows(frame(v.duql("window g sort ts { v = count() over 2 rows }"
                                " | select v"))) == "1;2;2;1;2;2;2;2");
    }

    TEST_CASE("a rows frame takes a parameter") {
        TestEnvironment env;
        const View v = view_of_lines(
            env,
            {event(1, R"("x":5)"), event(2, R"("x":1)"), event(3, R"("x":9)")});
        duql::Params p;
        p.emplace("n", duql::LiteralValue{std::uint64_t{2}});
        CHECK(rows(frame(v.duql(
                  "window sort ts { m = max(x) over $n rows } | select m",
                  p))) == "5;5;9");
    }

    TEST_CASE("the moving maximum and the bytes in the last second") {
        TestEnvironment env;
        const View m = view_of_lines(env, {event(1, "", 5), event(2, "", 1),
                                           event(3, "", 9), event(4, "", 2)});
        CHECK(rows(frame(m.duql("window tid sort ts { m = max(dur) over 2 "
                                "rows } | select m"))) == "5;5;9;9");
        const View b = view_of_lines(
            env,
            {event(0, R"("size":10)"), event(400000, R"("size":20)"),
             event(900000, R"("size":30)"), event(2000000, R"("size":40)")},
            "bytes.pfw");
        CHECK(rows(frame(b.duql("window sort ts { b = sum(size) over 1s }"
                                " | select b"))) == "10;30;60;40");
    }

    TEST_CASE("time frames over a duration and a number against a reference") {
        TestEnvironment env;
        // Rows out of key order, with ties and null values.
        const std::vector<Pt> pts = {{"a", 3000000, 4},
                                     {"a", 400000, std::nullopt},
                                     {"b", 900000, 2},
                                     {"a", 3000000, 7},
                                     {"b", 900000, std::nullopt},
                                     {"a", 400000, std::nullopt},
                                     {"b", 2500000, 9},
                                     {"a", 1200000, 5},
                                     {"a", 3400000, std::nullopt}};
        const View v = points_view(env, pts, true);
        for (const char* fn : {"sum", "mean", "min", "max", "count"}) {
            const std::string want =
                framed_ref(pts, fn, [&](std::size_t i, std::size_t j) {
                    return pts[j].key <= pts[i].key &&
                           pts[j].key >= pts[i].key - 1000000;
                });
            const std::string by_time =
                "window g sort ts { v = " + std::string(fn) +
                "(x) over 1s } | select v";
            CHECK_MESSAGE(rows(frame(v.duql(by_time))) == want, by_time);
            const std::string by_number =
                "window g sort k { v = " + std::string(fn) +
                "(x) over 1000000 } | select v";
            CHECK_MESSAGE(rows(frame(v.duql(by_number))) == want, by_number);
        }
        duql::Params p;
        p.emplace("w", duql::LiteralValue{std::uint64_t{600000}});
        const std::string want =
            framed_ref(pts, "sum", [&](std::size_t i, std::size_t j) {
                return pts[j].key <= pts[i].key &&
                       pts[j].key >= pts[i].key - 600000;
            });
        CHECK(rows(frame(v.duql("window g sort k { v = sum(x) over $w }"
                                " | select v",
                                p))) == want);
    }

    TEST_CASE("more window functions against a reference") {
        TestEnvironment env;
        const std::vector<Pt> pts = {{"a", 3, 4},
                                     {"a", 1, std::nullopt},
                                     {"b", 2, 8},
                                     {"a", 3, 7},
                                     {"b", 2, std::nullopt},
                                     {"a", 5, 1},
                                     {"b", 9, 2},
                                     {"a", 1, 6},
                                     {"a", 4, std::nullopt}};
        const View v = points_view(env, pts, false);
        // Each partition in sort order, ties in row order.
        std::map<std::string, std::vector<std::size_t>> order;
        for (std::size_t i = 0; i < pts.size(); ++i)
            order[pts[i].g].push_back(i);
        for (auto& [g, idx] : order)
            std::stable_sort(idx.begin(), idx.end(),
                             [&](std::size_t a, std::size_t b) {
                                 return pts[a].key < pts[b].key;
                             });
        const auto per_row = [&](auto&& at_position) {
            std::vector<std::string> out(pts.size());
            for (const auto& [g, idx] : order)
                for (std::size_t p = 0; p < idx.size(); ++p)
                    out[idx[p]] = at_position(idx, p);
            std::string text;
            for (std::size_t i = 0; i < out.size(); ++i)
                text += (i ? ";" : "") + out[i];
            return text;
        };
        const auto x_at = [&](const std::vector<std::size_t>& idx,
                              std::size_t p) -> std::optional<double> {
            if (pts[idx[p]].x) return static_cast<double>(*pts[idx[p]].x);
            return std::nullopt;
        };
        const auto run_q = [&](const std::string& entry) {
            return rows(frame(
                v.duql("window g sort k { v = " + entry + " } | select v")));
        };
        const auto running = [&](bool mean, int which) {
            return per_row([&](const auto& idx, std::size_t p) {
                std::vector<double> seen;
                for (std::size_t q = 0; q <= p; ++q)
                    if (const auto x = x_at(idx, q)) seen.push_back(*x);
                if (seen.empty()) return std::string("null");
                if (mean) return aggregated("mean", seen);
                return aggregated(which ? "max" : "min", seen);
            });
        };
        CHECK(run_q("running_min(x)") == running(false, 0));
        CHECK(run_q("running_max(x)") == running(false, 1));
        CHECK(run_q("running_mean(x)") == running(true, 0));
        CHECK(run_q("fill_forward(x)") ==
              per_row([&](const auto& idx, std::size_t p) {
                  for (std::size_t q = p + 1; q-- > 0;)
                      if (const auto x = x_at(idx, q)) return shown(x);
                  return std::string("null");
              }));
        CHECK(run_q("nth(x, 2)") == per_row([&](const auto& idx, std::size_t) {
                  return idx.size() >= 2 ? shown(x_at(idx, 1))
                                         : std::string("null");
              }));
        CHECK(run_q("nth(x, 9)") == per_row([&](const auto&, std::size_t) {
                  return std::string("null");
              }));
        CHECK(run_q("ntile(3)") == per_row([&](const auto& idx, std::size_t p) {
                  const std::size_t sz = idx.size(), base = sz / 3,
                                    rem = sz % 3;
                  std::size_t bucket = 0, left = p;
                  for (std::size_t b = 0; b < 3; ++b) {
                      const std::size_t w = base + (b < rem ? 1 : 0);
                      if (left < w) {
                          bucket = b + 1;
                          break;
                      }
                      left -= w;
                  }
                  return std::to_string(bucket);
              }));
        CHECK(run_q("percent_rank()") ==
              per_row([&](const auto& idx, std::size_t p) {
                  std::size_t below = 0;
                  for (const auto j : idx)
                      below += pts[j].key < pts[idx[p]].key;
                  return shown(idx.size() > 1
                                   ? static_cast<double>(below) /
                                         static_cast<double>(idx.size() - 1)
                                   : 0.0);
              }));
        CHECK(run_q("cume_dist()") ==
              per_row([&](const auto& idx, std::size_t p) {
                  std::size_t upto = 0;
                  for (const auto j : idx)
                      upto += pts[j].key <= pts[idx[p]].key;
                  return shown(static_cast<double>(upto) /
                               static_cast<double>(idx.size()));
              }));
        CHECK(rows(frame(v.duql("window { p = percent_rank(), c = cume_dist() }"
                                " | select p, c"))) ==
              "0,1;0,1;0,1;0,1;0,1;0,1;0,1;0,1;0,1");
        CHECK(run_q("lag(x, 1, -1)") ==
              per_row([&](const auto& idx, std::size_t p) {
                  return p == 0 ? std::string("-1") : shown(x_at(idx, p - 1));
              }));
        CHECK(run_q("lead(x, 2, 0)") ==
              per_row([&](const auto& idx, std::size_t p) {
                  return p + 2 >= idx.size() ? std::string("0")
                                             : shown(x_at(idx, p + 2));
              }));
    }

    TEST_CASE("count_distinct and arg_max over a frame") {
        TestEnvironment env;
        const auto at = [](int ts, const std::string& f, int dur) {
            return event(ts,
                         R"("g":"a","f":")" + f + R"(","n":"r)" +
                             std::to_string(ts) + R"(")",
                         dur);
        };
        const View files = view_of_lines(
            env, {at(1, "a", 5), at(2, "b", 5), at(3, "a", 5), at(4, "c", 5)});
        CHECK(rows(frame(files.duql("window g sort ts { n = count_distinct(f)"
                                    " over 3 rows } | select n"))) ==
              "1;2;2;3");
        CHECK(rows(frame(files.duql("window g { n = count_distinct(f) }"
                                    " | select n"))) == "3;3;3;3");
        const View slow = view_of_lines(
            env, {at(1, "a", 5), at(2, "a", 9), at(3, "a", 9), at(4, "a", 2)},
            "slow.pfw");
        CHECK(rows(frame(slow.duql("window g sort ts { s = arg_max(n, dur)"
                                   " over 2 rows } | select s"))) ==
              "r1;r2;r2;r3");
        CHECK(rows(frame(slow.duql("window g sort ts { s = arg_min(n, dur)"
                                   " over 2 rows } | select s"))) ==
              "r1;r1;r2;r4");
        CHECK(rows(frame(slow.duql("window g { s = arg_max(n, dur) }"
                                   " | select s"))) == "r2;r2;r2;r2");
    }

    TEST_CASE("count_if and collect over a partition and a frame") {
        TestEnvironment env;
        const auto at = [](int ts, const std::string& g, const std::string& x,
                           int dur) {
            return event(ts,
                         R"("g":")" + g + R"(","n":"r)" + std::to_string(ts) +
                             R"(")" + x,
                         dur);
        };
        const View v = view_of_lines(
            env, {at(1, "a", "", 3), at(2, "b", "", 9), at(3, "a", "", 7),
                  at(4, "b", "", 1), at(5, "a", "", 8)});
        CHECK(rows(frame(v.duql("window g { c = count_if(dur > 5) }"
                                " | select c"))) == "2;1;2;1;2");
        const df::DataFrame f =
            frame(v.duql("window g { l = collect(n) } | select l"));
        CHECK(items(f, 0, "l") == "[r1,r3,r5]");
        CHECK(items(f, 1, "l") == "[r2,r4]");
        CHECK(items(f, 4, "l") == "[r1,r3,r5]");
        const df::DataFrame w =
            frame(v.duql("window g sort ts { l = collect(n) over 2 rows,"
                         " c = count_if(dur > 5) over 2 rows }"
                         " | select l, c"));
        CHECK(items(w, 0, "l") == "[r1]");
        CHECK(items(w, 2, "l") == "[r1,r3]");
        CHECK(items(w, 4, "l") == "[r3,r5]");
        CHECK(bnum(w, 4, "c") == 2);
        CHECK(bnum(w, 0, "c") == 0);
        CHECK(w.columns[static_cast<std::size_t>(bcol(w, "l"))].type() ==
              df::TypeId::List);
        const View gaps = view_of_lines(
            env,
            {at(1, "a", R"(,"x":3)", 1), at(2, "a", "", 1),
             at(3, "a", R"(,"x":9)", 1), at(4, "a", R"(,"x":3)", 1)},
            "gaps.pfw");
        CHECK(
            rows(frame(gaps.duql("window g { c = count_if(x > 5),"
                                 " d = count_distinct(x) } | select c, d"))) ==
            "1,2;1,2;1,2;1,2");
    }

    TEST_CASE("framed var, std and quantile against a reference") {
        TestEnvironment env;
        const std::vector<std::int64_t> ts = {1000000, 1400000, 1900000,
                                              2600000, 2700000, 3900000};
        const std::vector<std::int64_t> dur = {
            1000000007, 1000000003, 1000000011, 1000000001, 1000000020, 9};
        std::vector<std::string> lines;
        for (std::size_t i = 0; i < ts.size(); ++i)
            lines.push_back(event(static_cast<int>(ts[i]), R"("g":"a")",
                                  static_cast<int>(dur[i])));
        const View v = view_of_lines(env, lines);
        const auto stats = [&](const std::vector<std::size_t>& in) {
            std::vector<double> x;
            for (const auto j : in) x.push_back(static_cast<double>(dur[j]));
            std::sort(x.begin(), x.end());
            const double pos = 0.5 * static_cast<double>(x.size() - 1);
            const auto lo = static_cast<std::size_t>(pos);
            const double q = lo + 1 < x.size()
                                 ? x[lo] + (pos - static_cast<double>(lo)) *
                                               (x[lo + 1] - x[lo])
                                 : x[lo];
            double mean = 0;
            for (const double d : x) mean += d;
            mean /= static_cast<double>(x.size());
            double ss = 0;
            for (const double d : x) ss += (d - mean) * (d - mean);
            return std::array<double, 2>{
                q, x.size() < 2 ? -1 : ss / static_cast<double>(x.size() - 1)};
        };
        const df::DataFrame rowsf = frame(
            v.duql("window g sort ts { m = quantile(dur, 0.5) over 3 rows,"
                   " s = var(dur) over 3 rows } | select m, s"));
        const df::DataFrame timef =
            frame(v.duql("window g sort ts { m = quantile(dur, 0.5) over 1s,"
                         " s = var(dur) over 1s, d = std(dur) over 1s }"
                         " | select m, s, d"));
        for (std::size_t i = 0; i < ts.size(); ++i) {
            std::vector<std::size_t> in;
            for (std::size_t j = i >= 2 ? i - 2 : 0; j <= i; ++j)
                in.push_back(j);
            auto st = stats(in);
            const auto r = static_cast<std::int64_t>(i);
            CHECK(bnum(rowsf, r, "m") == st[0]);
            if (st[1] < 0)
                CHECK(rowsf.columns[1].is_null(r));
            else
                CHECK(bnum(rowsf, r, "s") ==
                      doctest::Approx(st[1]).epsilon(1e-9));
            in.clear();
            for (std::size_t j = 0; j <= i; ++j)
                if (ts[j] >= ts[i] - 1000000) in.push_back(j);
            st = stats(in);
            CHECK(bnum(timef, r, "m") == st[0]);
            if (st[1] < 0) {
                CHECK(timef.columns[1].is_null(r));
                CHECK(timef.columns[2].is_null(r));
            } else {
                CHECK(bnum(timef, r, "s") ==
                      doctest::Approx(st[1]).epsilon(1e-9));
                CHECK(bnum(timef, r, "d") ==
                      doctest::Approx(std::sqrt(st[1])).epsilon(1e-9));
            }
        }
    }

    TEST_CASE("window aggregate compile errors") {
        CHECK(
            fails_with("window pid sort ts { h = histogram(dur) over 3 rows }",
                       {"'histogram' takes no frame", "quantile"}));
        CHECK(fails_with(
            "window pid sort ts { q = quantile(dur, 1.5) over 3 rows }",
            {"quantile level", "[0, 1]"}));
        CHECK(fails_with("window pid sort ts { q = quantile(dur) over 3 rows }",
                         {"quantile"}));
        CHECK(fails_with("window pid sort ts { a = arg_max(name) over 3 rows }",
                         {"arg_max"}));
        CHECK(fails_with("window pid sort ts { a = count_if() over 3 rows }",
                         {"count_if"}));
        CHECK(fails_with("window pid sort ts { a = rank() over 3 rows }",
                         {"'rank' takes no frame", "collect"}));
    }

    TEST_CASE("window frame compile errors") {
        CHECK(fails_with("derive m = max(dur) over 2 rows",
                         {"'over'", "'window' block"}));
        CHECK(fails_with("window name { r = rank() over 2 rows }",
                         {"'rank' takes no frame"}));
        CHECK(fails_with("window name { r = abs(dur) over 2 rows }",
                         {"'abs' takes no frame"}));
        CHECK(fails_with("window name { r = max(dur, 2) over 2 rows }",
                         {"'max' takes no frame"}));
        CHECK(fails_with("window name sort ts { s = sum(dur) over 0 rows }",
                         {"positive integer"}));
        CHECK(fails_with("window name sort ts { s = sum(dur) over 2.5 rows }",
                         {"positive integer"}));
        CHECK(fails_with("window name { s = sum(dur) over 1s }",
                         {"needs a 'sort' key"}));
        CHECK(fails_with("window name sort ts, pid { s = sum(dur) over 1s }",
                         {"exactly one sort key"}));
        CHECK(fails_with("window name sort -ts { s = sum(dur) over 1s }",
                         {"ascending sort key"}));
        CHECK(fails_with("window name sort pid { s = sum(dur) over 1s }",
                         {"'over' with a duration needs a time key"}));
        CHECK(fails_with("window name sort ts { s = sum(dur) over 0.5 }",
                         {"whole number"}));
        CHECK(fails_with("window name sort name { s = sum(dur) over 2 }",
                         {"numeric sort key"}));
        CHECK(fails_with("window name sort ts { n = ntile(0) }",
                         {"'ntile' takes a positive integer"}));
        CHECK(fails_with("window name sort ts { n = nth(dur, 0) }",
                         {"'nth' takes a positive integer"}));
        CHECK(fails_with("window name sort ts { n = lag(dur, 1, dur) }",
                         {"default is a literal"}));
        CHECK(fails_with("derive p = percent_rank()", {"window function"}));
    }

    TEST_CASE("partition aggregates keep a null key as its own partition") {
        TestEnvironment env;
        const View v = view_of_lines(
            env, {event(1, R"("g":"a","x":1)"), event(2, R"("x":10)"),
                  event(3, R"("g":"a","x":3)"), event(4, R"("x":20)")});
        CHECK(rows(frame(v.duql("window g { s = std(x), m = mean(x) } | derive "
                                "t = round(s * s) | select t, m"))) ==
              "2,2;50,15;2,2;50,15");
    }

    TEST_CASE("window compile errors") {
        CHECK(fails_with("derive r = row_number()", {"window"}));
        CHECK(fails_with("window name { d = dur * 2 }", {"window function"}));
        CHECK(fails_with("window name { s = std(dur, 2) }", {"std"}));
        CHECK(fails_with("window name { l = lag(dur, -1) }", {"distance"}));
    }
}

TEST_SUITE("View duql expand") {
    const std::vector<std::string> HOSTS = {
        line("io", 1, R"("hosts":["a","b"])"), line("io", 2, R"("hosts":[])"),
        line("io", 3, R"("x":1)")};

    TEST_CASE("one row per element with its index") {
        TestEnvironment env;
        const View v = view_of_lines(env, HOSTS);
        CHECK(rows(frame(v.duql("expand hosts with_index k"
                                " | select hosts, k"))) == "a,0;b,1;null,null");
        CHECK(rows(frame(v.duql("select ts, hosts | expand hosts as h"
                                " | select ts, h"))) == "1,a;1,b;3,null");
    }

    TEST_CASE("keep_empty keeps a row for an empty array") {
        TestEnvironment env;
        const View v = view_of_lines(env, HOSTS);
        CHECK(
            rows(frame(v.duql("expand hosts keep_empty | select ts, hosts"))) ==
            "1,a;1,b;2,null;3,null");
    }

    TEST_CASE("object elements read as fields") {
        TestEnvironment env;
        const View v = view_of_lines(
            env,
            {line("io", 1, R"("io":[{"op":"r","n":2},{"op":"w","n":5}])")});
        CHECK(rows(frame(v.duql("expand io as x | select x.op, x.n"))) ==
              "r,2;w,5");
        CHECK(rows(frame(v.duql("expand io as x | where x.n > 3"
                                " | select x.op"))) == "w");
    }

    TEST_CASE("a missing field gives one null row") {
        TestEnvironment env;
        const View v = view_of_lines(env, HOSTS);
        CHECK(rows(frame(v.duql("expand nope | select ts, nope"))) ==
              "1,null;2,null;3,null");
    }
}

TEST_SUITE("View duql pivot and unpivot") {
    const std::vector<std::string> METRICS = {
        line("io", 1, R"("f":"a","metric":"cpu","v":1)"),
        line("io", 2, R"("f":"a","metric":"mem","v":2)"),
        line("io", 3, R"("f":"b","metric":"cpu","v":3)")};

    TEST_CASE("metrics as columns") {
        TestEnvironment env;
        const View v = view_of_lines(env, METRICS);
        const df::DataFrame f =
            frame(v.duql("select f, metric, v | pivot metric { s = sum(v) }"));
        CHECK(f.names == std::vector<std::string>{"f", "s.cpu", "s.mem"});
        CHECK(rows(f) == "a,1,2;b,3,null");
    }

    TEST_CASE("fixed columns allow later stages") {
        TestEnvironment env;
        const View v = view_of_lines(env, METRICS);
        const df::DataFrame f =
            frame(v.duql("select f, metric, v | pivot metric in [\"mem\"]"
                         " { s = sum(v) } | where s.mem > 0"));
        CHECK(f.names == std::vector<std::string>{"f", "s.mem"});
        CHECK(rows(f) == "a,2");
    }

    TEST_CASE("too many values fail naming both counts") {
        TestEnvironment env;
        const View v = view_of_lines(
            env, {line("io", 1, R"("m":"x")"), line("io", 2, R"("m":"y")"),
                  line("io", 3, R"("m":"z")")});
        setenv("DUQL_PIVOT_MAX_COLUMNS", "2", 1);
        std::string what;
        try {
            (void)frame(v.duql("select m | pivot m { n = count() }"));
        } catch (const DFTUtilsException& e) {
            what = e.what();
        }
        unsetenv("DUQL_PIVOT_MAX_COLUMNS");
        CHECK(what.find("gives 3") != std::string::npos);
        CHECK(what.find("(2)") != std::string::npos);
    }

    TEST_CASE("a stage after an open pivot asks for in") {
        CHECK(fails_with("pivot cat { n = count() } | take 1", {"in [...]"}));
    }

    TEST_CASE("two fields to rows") {
        TestEnvironment env;
        const View v = view_of_lines(env, {line("io", 1, R"("a":1,"b":2)")});
        const df::DataFrame f =
            frame(v.duql("select name, a, b | unpivot a, b as k, v"));
        CHECK(f.names == std::vector<std::string>{"name", "k", "v"});
        CHECK(rows(f) == "op,a,1;op,b,2");
    }

    TEST_CASE("unpivot unifies integers and doubles and refuses mixes") {
        TestEnvironment env;
        const View v =
            view_of_lines(env, {line("io", 1, R"("a":1,"b":2.5,"s":"t")")});
        CHECK(rows(frame(v.duql("select a, b | unpivot a, b as k, v"))) ==
              "a,1;b,2.500000");
        std::string what;
        try {
            (void)frame(v.duql("select a, s | unpivot a, s as k, v"));
        } catch (const DFTUtilsException& e) {
            what = e.what();
        }
        CHECK(what.find("mixes") != std::string::npos);
    }
}

TEST_SUITE("View duql reshape determinism") {
    // 3000 events over three names with repeated durations; `tags` cycles
    // through ["a", "b"], [], ["c"] and missing.
    static std::string reshape_trace(TestEnvironment & env,
                                     std::size_t member_bytes) {
        const std::string pfw = env.get_dir() + "/rs.pfw";
        {
            std::ofstream o(pfw);
            const char* tags[] = {R"(,"tags":["a","b"])", R"(,"tags":[])",
                                  R"(,"tags":["c"])", ""};
            for (int i = 0; i < 3000; ++i)
                o << R"({"ph":"X","name":")" << "abc"[i % 3]
                  << R"(","cat":"POSIX","pid":1,"tid":1,"ts":)"
                  << 1000 + i * 100 << R"(,"dur":)" << (i * 37) % 101
                  << R"(,"args":{"n":)" << i << tags[i % 4] << "}}\n";
        }
        const std::string gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip_multimember(pfw, gz,
                                                           member_bytes);
        fs::remove(pfw);
        dftracer::utils::index::Indexer::open({gz}).build();
        return gz;
    }

    TEST_CASE("reshaping stages do not depend on workers or checkpoints") {
        const std::vector<std::string> queries = {
            "window name sort ts { r = row_number(), p = lag(dur) }"
            " | expand tags | select ts, name, r, p, tags",
            "window name sort -dur { k = rank(), d = dense_rank(),"
            " s = sum(dur), f = first(dur), l = last(dur) }"
            " | select ts, k, d, s, f, l",
            "expand tags with_index i keep_empty | select ts, tags, i",
            "select name, cat, dur | pivot name { n = count(), t = sum(dur) }",
            "select ts, n, dur | unpivot n, dur as k, v | select ts, k, v",
        };
        TestEnvironment big(10);
        TestEnvironment small(10);
        const std::vector<std::string> files = {reshape_trace(big, 20000),
                                                reshape_trace(small, 2048)};
        for (const auto& q : queries) {
            INFO("query: ", q);
            std::optional<std::string> want;
            for (const auto& gz : files)
                for (std::size_t workers : {1, 2, 8}) {
                    dftracer::utils::Runtime rt(workers);
                    const View v =
                        View::from_file(gz, determine_index_path(gz, ""))
                            .duql(q);
                    const std::string got = rows(rt.submit(v.collect()).get());
                    if (!want) want = got;
                    CHECK_MESSAGE(got == *want, gz, " workers ", workers, ": ",
                                  got.substr(0, 200), " vs ",
                                  want->substr(0, 200));
                }
            CHECK_FALSE(want->empty());
        }
    }

    TEST_CASE("a quantifier gives the same rows in the scan and after it") {
        TestEnvironment env(10);
        const std::string gz = reshape_trace(env, 4096);
        const View v = View::from_file(gz, determine_index_path(gz, ""));
        for (const char* cond :
             {R"(any(tags, . == "b"))", R"(all(tags, . != "a"))",
              R"(not any(tags, . < "b" and ^.dur > 50))"}) {
            INFO(std::string(cond));
            const auto qs = both(cond);
            // A scan filter alone keeps no row order.
            const std::string scan = sorted(frame(v.duql(qs[0])));
            CHECK_FALSE(scan.empty());
            CHECK(sorted(frame(v.duql(qs[1]))) == scan);
        }
    }
}

TEST_SUITE("View duql folded aggregates") {
    static View groups_view(TestEnvironment & env) {
        return view_of_lines(
            env,
            {event(1, R"("g":"a","x":1,"y":5)"),
             event(2, R"("g":"a","x":2,"y":9)"),
             event(3, R"("g":"a","x":1,"y":9)"), event(4, R"("g":"a","x":3)"),
             event(5, R"("g":"b","x":7,"y":2)"),
             event(6, R"("g":"b","x":7,"y":2)"), event(7, R"("x":4,"y":1)")});
    }

    TEST_CASE("count_distinct, collect, arg_max and arg_min per group") {
        TestEnvironment env(10);
        const View v = groups_view(env);
        const std::string q =
            "group g { n = count(), d = count_distinct(x), c = collect(x),"
            " hi = arg_max(x, y), lo = arg_min(x, y), s = sum(x) }";
        CHECK(v.explain_duql(q).find("group fold over") != std::string::npos);
        for (const std::string& stage : {q, "derive z = 1 | " + q}) {
            INFO(stage);
            const df::DataFrame f = frame(v.duql(stage));
            REQUIRE(f.num_rows() == 3);
            CHECK(bstr(f, 0, "g") == "a");
            CHECK(bstr(f, 1, "g") == "b");
            CHECK(f.columns[0].is_null(2));
            CHECK(nums(f, "n") == std::vector<double>{4, 2, 1});
            CHECK(nums(f, "d") == std::vector<double>{3, 1, 1});
            CHECK(items(f, 0, "c") == "[1,2,1,3]");
            CHECK(items(f, 1, "c") == "[7,7]");
            CHECK(items(f, 2, "c") == "[4]");
            // Ties keep the first row: y = 9 at x = 2, then at x = 1.
            CHECK(nums(f, "hi") == std::vector<double>{2, 7, 4});
            CHECK(nums(f, "lo") == std::vector<double>{1, 7, 4});
            CHECK(nums(f, "s") == std::vector<double>{7, 14, 4});
            CHECK(f.columns[bcol(f, "hi")].type() == df::TypeId::Int64);
            CHECK(f.columns[bcol(f, "d")].type() == df::TypeId::Int64);
        }
    }

    TEST_CASE("folded aggregates skip nulls and give one row over none") {
        TestEnvironment env(10);
        const View v = groups_view(env);
        const df::DataFrame f =
            frame(v.duql("agg { d = count_distinct(y), c = collect(y) }"));
        CHECK(bnum(f, 0, "d") == 4);
        CHECK(items(f, 0, "c") == "[5,9,9,2,2,1]");
        const df::DataFrame none = frame(v.duql(
            "where dur < 0 | agg { d = count_distinct(x), c = collect(x) }"));
        REQUIRE(none.num_rows() == 1);
        CHECK(bnum(none, 0, "d") == 0);
        CHECK(none.columns[1].is_null(0));
    }

    TEST_CASE("sketches merge into quantiles") {
        TestEnvironment env(10);
        std::vector<std::string> lines;
        for (int i = 1; i <= 1000; ++i)
            lines.push_back(event(i, R"("g":)" + std::to_string(i % 2), i));
        const View v = view_of_lines(env, lines);
        const df::DataFrame f =
            frame(v.duql("group g { s = sketch(dur) }"
                         " | agg { p = quantile(merge(s), 0.5), m = merge(s),"
                         " n = count() }"));
        CHECK(bnum(f, 0, "n") == 2);
        CHECK(bnum(f, 0, "p") >= 495);
        CHECK(bnum(f, 0, "p") <= 505);
        CHECK(!bstr(f, 0, "m").empty());
        const df::DataFrame again =
            frame(v.duql("group g { s = sketch(dur) } | agg { m = merge(s) }"
                         " | agg { p = quantile(merge(m), 0.5) }"));
        CHECK(bnum(again, 0, "p") == bnum(f, 0, "p"));
        const df::DataFrame junk = frame(v.duql(
            R"(derive j = "junk" | agg { m = merge(j), s = sketch(j) })"));
        CHECK(junk.columns[0].is_null(0));
        CHECK(junk.columns[1].is_null(0));
    }

    TEST_CASE("folded aggregates in a pivot") {
        TestEnvironment env(10);
        const View v = groups_view(env);
        const df::DataFrame f =
            frame(v.duql(R"(where g is not null | select g, x, y)"
                         R"( | pivot g in ["a", "b"] { c = collect(x) })"));
        REQUIRE(f.num_rows() >= 1);
        CHECK(std::find(f.names.begin(), f.names.end(), "c.a") !=
              f.names.end());
    }
}

TEST_SUITE("View duql array and object functions") {
    static View functions_view(TestEnvironment & env) {
        return view_of_lines(
            env, {event(1, R"("s":"a,b,,c","xs":[1,2,3,4],"o":{"b":2,"a":1},)"
                           R"("j":"{\"k\": [1, 2]}")"),
                  event(2, R"("s":"x","xs":[],"o":{"a":5},"j":"nope")")});
    }

    TEST_CASE("each function after the scan") {
        TestEnvironment env(10);
        const View v = functions_view(env);
        const df::DataFrame f = frame(v.duql(
            R"(derive p = split(s, ","), n = len(split(s, ",")),)"
            R"( m = slice(xs, 1, -1), t = slice(xs, -2), e = slice(xs, 9),)"
            R"( k = keys(o), w = values(o), q = parse_json(j))"
            R"( | select p, n, m, t, e, k, w, q)"));
        REQUIRE(f.num_rows() == 2);
        CHECK(items(f, 0, "p") == "[a,b,,c]");
        CHECK(items(f, 1, "p") == "[x]");
        CHECK(nums(f, "n") == std::vector<double>{4, 1});
        CHECK(items(f, 0, "m") == "[2,3]");
        CHECK(items(f, 0, "t") == "[3,4]");
        CHECK(items(f, 0, "e") == "[]");
        CHECK(items(f, 1, "m") == "[]");
        CHECK(items(f, 0, "k") == "[a,b]");
        CHECK(items(f, 1, "k") == "[a]");
        CHECK(items(f, 0, "w") == "[1,2]");
        CHECK(items(f, 1, "w") == "[5]");
        CHECK(bstr(f, 0, "q") == R"({"k":[1,2]})");
        CHECK(f.columns[bcol(f, "q")].is_null(1));
        for (const auto& n : frame(v.duql(R"(derive p = split(s, ","))")).names)
            CHECK_FALSE(n.starts_with("__duql"));
    }

    TEST_CASE("each function in the scan filter and after it") {
        TestEnvironment env(10);
        const View v = functions_view(env);
        for (const char* cond :
             {R"(len(split(s, ",")) == 4)", "first(slice(xs, 1)) == 2",
              "len(slice(xs, -2)) == 2", "len(keys(o)) == 2",
              "sum(values(o)) == 3", "len(flatten(xs)) == 4"}) {
            INFO(cond);
            const std::string scan = std::string("where ") + cond;
            const std::string after =
                std::string("derive z = 1 | where ") + cond;
            CHECK(v.explain_duql(scan).find("with_column") ==
                  std::string::npos);
            CHECK(frame(v.duql(scan)).num_rows() == 1);
            CHECK(frame(v.duql(after)).num_rows() == 1);
        }
        CHECK(frame(v.duql(R"(where type(parse_json(j)) == "object")"))
                  .num_rows() == 1);
        CHECK(frame(v.duql(R"(where contains(keys(o), "b"))")).num_rows() == 1);
        for (const char* q : {R"(where split(s, "") is null)",
                              R"(derive z = 1 | where split(s, "") is null)"})
            CHECK(frame(v.duql(q)).num_rows() == 2);
    }

    TEST_CASE("a wrong input type is unknown") {
        TestEnvironment env(10);
        const View v = functions_view(env);
        const df::DataFrame f = frame(
            v.duql(R"(derive a = split(dur, ","), b = slice(s, 0, 1),)"
                   R"( c = keys(s), d = flatten(o) | select a, b, c, d)"));
        for (std::size_t c = 0; c < f.columns.size(); ++c) {
            INFO(f.names[c]);
            CHECK(f.columns[c].null_count() == 2);
        }
        CHECK(frame(v.duql("where slice(s, 0, 1) is null")).num_rows() == 2);
    }

    TEST_CASE("a function of parse_json after the scan is a compile error") {
        TestEnvironment env(10);
        const View v = functions_view(env);
        try {
            (void)frame(v.duql(R"(derive p = split(parse_json(j), ","))"));
            FAIL("split(parse_json(j)) compiled");
        } catch (const DFTUtilsException& e) {
            CHECK(std::string(e.what()).find("scan filter") !=
                  std::string::npos);
        }
        CHECK(frame(v.duql(R"(where split(parse_json(j), ",") is null)"))
                  .num_rows() == 2);
    }

    TEST_CASE("values of fields with different types is a compile error") {
        TestEnvironment env(10);
        const View v = view_of_lines(env, {event(1, R"("o":{"a":1,"b":"x"})")});
        try {
            (void)frame(v.duql("derive w = values(o)"));
            FAIL("values() of mixed fields compiled");
        } catch (const DFTUtilsException& e) {
            CHECK(std::string(e.what()).find("different types") !=
                  std::string::npos);
        }
    }
}

TEST_SUITE("View duql array indexes and lists") {
    static View arrays_view(TestEnvironment & env) {
        auto line = [](int ts, const char* name, const std::string& args) {
            return R"({"ph":"X","name":")" + std::string(name) +
                   R"(","cat":"POSIX","pid":1,"tid":1,"ts":)" +
                   std::to_string(ts) + R"(,"dur":10,"args":{)" + args + "}}";
        };
        return view_of_lines(
            env, {line(1, "read",
                       R"("xs":[10,20,30],"i":2,"tags":["b","a","b",null])"),
                  line(2, "write", R"("xs":[1],"i":5,"tags":["c"])"),
                  line(3, "open", R"("xs":[],"i":0,"tags":[])")});
    }

    TEST_CASE("a per-row index") {
        TestEnvironment env(10);
        const View v = arrays_view(env);
        duql::Params p;
        p.emplace("n", duql::LiteralValue{std::int64_t{1}});
        const df::DataFrame f = frame(v.duql(
            "derive a = xs[i], b = xs[$n], c = xs[i - 3] | select a, b, c", p));
        REQUIRE(f.num_rows() == 3);
        CHECK(bnum(f, 0, "a") == 30);
        CHECK(bnum(f, 0, "b") == 20);
        CHECK(bnum(f, 0, "c") == 30);
        for (const char* n : {"a", "b", "c"}) {
            INFO(n);
            CHECK(f.columns[static_cast<std::size_t>(bcol(f, n))].is_null(1));
            CHECK(f.columns[static_cast<std::size_t>(bcol(f, n))].is_null(2));
        }
        CHECK(v.explain_duql("derive a = xs[i]").find("call __duql_c_") !=
              std::string::npos);
    }

    TEST_CASE("a list value") {
        TestEnvironment env(10);
        const View v = arrays_view(env);
        const df::DataFrame f = frame(
            v.duql(R"(derive t = [cat, name], k = len([1, 2, 3]) | )"
                   R"(where contains(["read", "write"], name) | select t, k)"));
        REQUIRE(f.num_rows() == 2);
        CHECK(items(f, 0, "t") == "[POSIX,read]");
        CHECK(items(f, 1, "t") == "[POSIX,write]");
        CHECK(nums(f, "k") == std::vector<double>{3, 3});
        const df::DataFrame e =
            frame(v.duql("derive t = [] | select t | take 1"));
        CHECK(items(e, 0, "t") == "[]");
        CHECK(fails_with(R"(derive t = [cat, ts])", {"one type"}));
        CHECK(fails_with(R"(derive t = [1, true])", {"one type"}));
    }

    TEST_CASE("an index into a derived list reads the derived column") {
        TestEnvironment env(10);
        const View v = arrays_view(env);
        const df::DataFrame f =
            frame(v.duql(R"(derive l = [cat, name] | derive y = l[1], z = l[0])"
                         R"( | select y, z | take 1)"));
        CHECK(bstr(f, 0, "y") == "read");
        CHECK(bstr(f, 0, "z") == "POSIX");
        const View w =
            view_of_lines(env, {event(1, R"("s":"a,b")")}, "split.pfw");
        const df::DataFrame g =
            frame(w.duql(R"(derive l = split(s, ",") | derive y = l[1],)"
                         R"( z = l[-1] | select y, z)"));
        CHECK(bstr(g, 0, "y") == "b");
        CHECK(bstr(g, 0, "z") == "b");
    }

    TEST_CASE("a list with nulls and mixed numbers") {
        TestEnvironment env(10);
        const View v = arrays_view(env);
        const df::DataFrame f =
            frame(v.duql(R"(derive t = [cat, null], u = [cat, nosuch],)"
                         R"( w = [null, nosuch], l = [1, 2.5] | derive)"
                         R"( x = l[i - 2], y = l[i - 1], n = len(l))"
                         R"( | select t, u, w, l, x, y, n | take 1)"));
        CHECK(items(f, 0, "t") == "[POSIX,null]");
        CHECK(items(f, 0, "u") == "[POSIX,null]");
        CHECK(items(f, 0, "w") == "[null,null]");
        CHECK(items(f, 0, "l") == "[1.000000,2.500000]");
        CHECK(bnum(f, 0, "x") == 1);
        CHECK(bnum(f, 0, "y") == 2.5);
        CHECK(bnum(f, 0, "n") == 2);
        for (const char* cond :
             {"len([cat, null]) == 2", "len([1, 2.5, nosuch]) == 3",
              "contains([1, 2.5], dur - 9)"}) {
            INFO(cond);
            CHECK(frame(v.duql(std::string("where ") + cond)).num_rows() == 3);
            CHECK(frame(v.duql(std::string("derive z = 1 | where ") + cond))
                      .num_rows() == 3);
        }
    }

    TEST_CASE("array functions") {
        TestEnvironment env(10);
        const View v = arrays_view(env);
        const df::DataFrame f =
            frame(v.duql(R"(derive s = sort(tags), u = unique(tags),)"
                         R"( j = join(tags, ","), p = index_of(tags, "a"))"
                         R"( | select s, u, j, p)"));
        REQUIRE(f.num_rows() == 3);
        CHECK(items(f, 0, "s") == "[a,b,b,null]");
        CHECK(items(f, 0, "u") == "[b,a,null]");
        CHECK(bstr(f, 0, "j") == "b,a,b");
        CHECK(bnum(f, 0, "p") == 1);
        CHECK(items(f, 1, "s") == "[c]");
        CHECK(items(f, 2, "u") == "[]");
        CHECK(bstr(f, 2, "j").empty());
        CHECK_FALSE(
            f.columns[static_cast<std::size_t>(bcol(f, "j"))].is_null(2));
        CHECK(f.columns[static_cast<std::size_t>(bcol(f, "p"))].is_null(1));
        CHECK(f.columns[static_cast<std::size_t>(bcol(f, "p"))].is_null(2));
        const df::DataFrame g =
            frame(v.duql(R"(derive a = index_of(xs, 20.0), b = sort(xs),)"
                         R"( c = join(xs, ","), d = join(tags, "-"))"
                         R"( | select a, b, c, d)"));
        CHECK(bnum(g, 0, "a") == 1);
        CHECK(items(g, 0, "b") == "[10,20,30]");
        CHECK(g.columns[static_cast<std::size_t>(bcol(g, "c"))].is_null(0));
        CHECK(bstr(g, 2, "c").empty());
        CHECK(g.columns[static_cast<std::size_t>(bcol(g, "d"))].is_null(2) ==
              false);
        CHECK(v.explain_duql("derive s = sort(tags), j = join(tags, \",\")")
                  .find("call __duql_c_") != std::string::npos);
    }

    TEST_CASE("each form in the scan filter and after it") {
        TestEnvironment env(10);
        const View v = arrays_view(env);
        const std::vector<std::pair<const char*, std::size_t>> conds = {
            {R"(index_of(tags, "a") == 1)", 1},
            {"len(sort(tags)) == 4", 1},
            {"len(unique(tags)) == 3", 1},
            {R"(join(tags, ",") == "b,a,b")", 1},
            {"xs[i] == 30", 1},
            {"xs[i - 3] == 30", 1},
            {"len([cat, name]) == 2", 3},
            {R"(contains(["read", "write"], name))", 2}};
        for (const auto& [cond, rows] : conds) {
            INFO(cond);
            const std::string scan = std::string("where ") + cond;
            const std::string after =
                std::string("derive z = 1 | where ") + cond;
            CHECK(v.explain_duql(scan).find("with_column") ==
                  std::string::npos);
            CHECK(static_cast<std::size_t>(frame(v.duql(scan)).num_rows()) ==
                  rows);
            CHECK(static_cast<std::size_t>(frame(v.duql(after)).num_rows()) ==
                  rows);
        }
    }

    TEST_CASE("refused forms name the rule") {
        TestEnvironment env(10);
        const View v = arrays_view(env);
        CHECK(fails_with("derive a = xs[i].b", {"index"}));
        duql::Params p;
        p.emplace("n", duql::LiteralValue{std::string("x")});
        try {
            (void)frame(v.duql("derive a = xs[$n]", p));
            FAIL("xs[$n] with a string compiled");
        } catch (const DFTUtilsException& e) {
            CHECK(std::string(e.what()).find("$n") != std::string::npos);
        }
    }

    TEST_CASE("an index inside a quantifier after the scan is an error") {
        TestEnvironment env(10);
        const View v = arrays_view(env);
        CHECK_NOTHROW((void)frame(v.duql("where any(xs, . > 1)")));
        try {
            (void)frame(v.duql("derive z = 1 | where any(tags, xs[i] > 1)"));
            FAIL("an index in a quantifier compiled");
        } catch (const DFTUtilsException& e) {
            CHECK(std::string(e.what()).find("call_column") !=
                  std::string::npos);
        }
    }
}

TEST_SUITE("View duql session") {
    // `events` as a dftracer trace, one event per (pid, tid, ts, dur), in
    // the order given, gzip members of about `member_bytes`.
    static std::string session_trace(
        TestEnvironment & env,
        const std::vector<std::array<std::int64_t, 4>>& events,
        std::size_t member_bytes = 4096) {
        const std::string pfw = env.get_dir() + "/sess.pfw";
        {
            std::ofstream o(pfw);
            for (const auto& [pid, tid, ts, dur] : events)
                o << R"({"ph":"X","name":"read","cat":"POSIX","pid":)" << pid
                  << R"(,"tid":)" << tid << R"(,"ts":)" << ts << R"(,"dur":)"
                  << dur << "}\n";
        }
        const std::string gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip_multimember(pfw, gz,
                                                           member_bytes);
        fs::remove(pfw);
        dftracer::utils::index::Indexer::open({gz}).build();
        return gz;
    }

    static View view_of(const std::string& gz) {
        return View::from_file(gz, determine_index_path(gz, ""));
    }

    TEST_CASE("an idle gap after the latest end starts a session") {
        TestEnvironment env(10);
        // Written newest first; pid 1 ends at 110 before its third event.
        const std::string gz = session_trace(
            env,
            {{1, 1, 500, 5}, {2, 1, 20, 5}, {1, 1, 10, 100}, {1, 1, 0, 5}});
        const df::DataFrame f = frame(
            view_of(gz).duql("session pid gap 100 | select pid, ts, session"));
        CHECK(rows(f) == "1,500,2;2,20,1;1,10,1;1,0,1");
    }

    TEST_CASE("a session longer than max splits") {
        TestEnvironment env(10);
        const std::string gz = session_trace(
            env, {{1, 1, 0, 0}, {1, 1, 50, 0}, {1, 1, 100, 0}, {1, 1, 150, 0}});
        const df::DataFrame f = frame(
            view_of(gz).duql("session gap 60 max 120 as s | select ts, s"));
        CHECK(rows(f) == "0,1;50,1;100,1;150,2");
    }

    TEST_CASE("one scan reads the keys, time and duration") {
        TestEnvironment env(10);
        const std::string gz = session_trace(env, {{1, 1, 0, 5}});
        const std::string plan = view_of(gz).explain_duql(
            "session pid gap 1s | group pid, session { n = count() }");
        CHECK(plan.find("scan select: pid, ts, dur\n") != std::string::npos);
        CHECK(plan.find("session keys pid; time ts end ts + dur; gap "
                        "1000000; max none; as session\n") !=
              std::string::npos);
        std::string what;
        try {
            (void)frame(view_of(gz).duql("session pid gap 1 as ts"));
        } catch (const std::exception& e) {
            what = e.what();
        }
        CHECK(what.find("already a column") != std::string::npos);
    }

    TEST_CASE("sessions do not depend on workers or checkpoints") {
        // 4000 events of 4 threads, written out of time order.
        std::vector<std::array<std::int64_t, 4>> events;
        std::uint64_t x = 88172645463325252ULL;
        for (int i = 0; i < 4000; ++i) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            events.push_back({1 + (i % 2), 1 + (i % 4) / 2,
                              static_cast<std::int64_t>(x % 400000),
                              static_cast<std::int64_t>(x % 90)});
        }
        TestEnvironment big(10);
        TestEnvironment small(10);
        const std::vector<std::string> files = {
            session_trace(big, events, 20000),
            session_trace(small, events, 2048)};
        for (const char* q :
             {"session pid, tid gap 100us | group pid, tid, session "
              "{ n = count(), t = sum(dur) } | sort pid, tid, session",
              "session pid gap 300 max 5000 | select ts, pid, session"}) {
            INFO("query: ", q);
            std::optional<std::string> want;
            for (const auto& gz : files)
                for (std::size_t workers : {1, 2, 8}) {
                    dftracer::utils::Runtime rt(workers);
                    const std::string got =
                        rows(rt.submit(view_of(gz).duql(q).collect()).get());
                    if (!want) want = got;
                    CHECK(got == *want);
                }
            CHECK_FALSE(want->empty());
        }
    }
}

TEST_SUITE("View duql plugin functions") {
    TEST_CASE("a scalar op equals the built-in expression") {
        PluginOps ops;
        CHECK(text(pipe("derive d = myplug.twice(dur) + 1 | select d")) ==
              text(pipe("derive d = dur * 2 + 1 | select d")));
        CHECK(text(pipe("derive d = myplug.twice(dur) * 2 | select d")) ==
              text(pipe("derive d = dur * 4 | select d")));
        CHECK(text(pipe("derive d = myplug.twice(dur) | select ts, d")) ==
              text(pipe("derive d = dur * 2 | select ts, d")));
    }

    TEST_CASE("a second column and a scalar operand") {
        PluginOps ops;
        CHECK(text(pipe("derive d = myplug.add2(dur, ts) | select d")) ==
              text(pipe("derive d = dur + ts | select d")));
        const df::DataFrame got =
            pipe("derive d = myplug.scale(dur, 1.5) | select d");
        CHECK(got.columns[0].type() == df::TypeId::Float64);
        CHECK(text(got) == text(pipe("derive d = dur * 1.5 | select d")));
    }

    TEST_CASE("a table op with an integer operand") {
        PluginOps ops;
        const df::DataFrame got = pipe("sort ts | call myplug.head(3)");
        CHECK(got.num_rows() == 3);
        CHECK(text(got) == text(pipe("sort ts | take 3")));
    }

    TEST_CASE("an argument the op does not take names its signature") {
        PluginOps ops;
        CHECK(fails_with("derive d = myplug.scale(dur, \"x\")",
                         {"myplug.scale", "takes"}));
        CHECK(fails_with("derive d = myplug.twice(dur, 5)",
                         {"myplug.twice", "takes"}));
        CHECK(fails_with("call myplug.head(\"x\")", {"myplug.head", "takes"}));
        CHECK(fails_with("call myplug.head(1, 2)", {"myplug.head", "takes"}));
    }

    TEST_CASE("a scalar op on a schemaless view") {
        PluginOps ops;
        TestEnvironment env(10);
        const View v =
            view_of_lines(env, {event(1, R"("x":3)"), event(2, R"("x":5)")});
        const df::DataFrame f =
            frame(v.duql("derive d = myplug.twice(x) + 1 | select d"));
        CHECK(nums(f, "d") == std::vector<double>{7, 11});
    }
}

TEST_SUITE("View duql plugin reducers") {
    TEST_CASE("a reducer per group equals the built-in") {
        PluginReducers ops;
        const df::DataFrame got = pipe(
            "group name { t = myplug.total(dur), n = myplug.count_i(dur),"
            " s = myplug.scaled(dur, 1.5) }");
        const df::DataFrame want = pipe(
            "group name { t = sum(dur), n = count(), s = sum(dur) * 1.5 }");
        CHECK(got.num_rows() == want.num_rows());
        CHECK(nums(got, "t") == nums(want, "t"));
        CHECK(nums(got, "n") == nums(want, "n"));
        CHECK(nums(got, "s") == nums(want, "s"));
        CHECK(got.columns[static_cast<std::size_t>(bcol(got, "t"))].type() ==
              df::TypeId::Float64);
        CHECK(got.columns[static_cast<std::size_t>(bcol(got, "n"))].type() ==
              df::TypeId::Int64);
    }

    TEST_CASE("a reducer inside an aggregate expression") {
        PluginReducers ops;
        const auto got =
            nums(pipe("group name { r = myplug.total(dur) / count() }"), "r");
        const auto want = nums(pipe("group name { r = mean(dur) }"), "r");
        REQUIRE(got.size() == want.size());
        for (std::size_t i = 0; i < got.size(); ++i)
            CHECK(got[i] == doctest::Approx(want[i]));
    }

    TEST_CASE("nulls, floats and a bool result") {
        PluginReducers ops;
        TestEnvironment env(10);
        const View v = view_of_lines(
            env, {event(1, R"("g":1,"x":1.5)"), event(2, R"("g":1,"x":2.5)"),
                  event(3, R"("g":2)"), event(4, R"("g":3,"x":1.0)")});
        const df::DataFrame f =
            frame(v.duql("group g { t = myplug.total(x), b = myplug.big(x, 2),"
                         " m = myplug.total(missing_field) }"));
        REQUIRE(f.num_rows() == 3);
        const df::Series t =
            f.columns[static_cast<std::size_t>(bcol(f, "t"))].materialize();
        CHECK(t.data<double>()[0] == 4.0);
        CHECK(t.is_null(1));
        CHECK(t.data<double>()[2] == 1.0);
        const auto& b = f.columns[static_cast<std::size_t>(bcol(f, "b"))];
        CHECK(b.type() == df::TypeId::Bool);
        CHECK(!b.is_null(0));
        CHECK(b.is_null(1));
        const df::Series flags = b.cast(df::TypeId::Int64).materialize();
        CHECK(flags.data<std::int64_t>()[0] == 1);
        CHECK(flags.data<std::int64_t>()[2] == 0);
        const auto& m = f.columns[static_cast<std::size_t>(bcol(f, "m"))];
        for (std::int64_t r = 0; r < 3; ++r) CHECK(m.is_null(r));
    }

    TEST_CASE("arguments the reducer does not take name its signature") {
        PluginReducers ops;
        CHECK(fails_with("group name { t = myplug.scaled(dur) }",
                         {"myplug.scaled", "takes"}));
        CHECK(fails_with("group name { t = myplug.total(dur, 2) }",
                         {"myplug.total", "takes"}));
        CHECK(fails_with("group name { t = myplug.big(dur, 1.5) }",
                         {"myplug.big", "takes"}));
    }

    TEST_CASE("a reducer that fails on a group fails the query") {
        PluginReducers ops;
        CHECK(fails_with("group name { t = myplug.unrun(dur, 1) }",
                         {"myplug.unrun", "takes"}));
    }

    TEST_CASE("agg over no rows gives null") {
        PluginReducers ops;
        const df::DataFrame f =
            pipe("where dur < 0 | agg { t = myplug.total(dur) }");
        REQUIRE(f.num_rows() == 1);
        CHECK(f.columns[0].is_null(0));
    }
}
