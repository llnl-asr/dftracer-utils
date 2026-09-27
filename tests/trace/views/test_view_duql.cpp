#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/index/indexer.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "test_view_common.h"

namespace df = dftracer::utils::dataframe;

namespace {

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

    TEST_CASE("parameters bind counts and values") {
        duql::Params p;
        p.emplace("min", duql::LiteralValue{std::uint64_t{30}});
        p.emplace("n", duql::LiteralValue{std::uint64_t{3}});
        CHECK(pipe("where dur > $min | take $n", p).num_rows() == 3);
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
                         {"call", "12h"}));
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
        CHECK(fails_with("group name { t = sum(dur) / 1000 }",
                         {"aggregate call"}));
        CHECK(fails_with("group name { d = dur }", {"aggregate call"}));
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
    std::string reshape_trace(TestEnvironment & env, std::size_t member_bytes) {
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
    View groups_view(TestEnvironment & env) {
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
    View functions_view(TestEnvironment & env) {
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

TEST_SUITE("View duql session") {
    // `events` as a dftracer trace, one event per (pid, tid, ts, dur), in
    // the order given, gzip members of about `member_bytes`.
    std::string session_trace(
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

    View view_of(const std::string& gz) {
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
