// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/indexer.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <trace/views/test_view_common.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;
using namespace test_view_common;

namespace {

struct NullSink : ExportSink {
    void write(dstd::StringView) override {}
};

// Sets an environment variable for one scope.
struct ScopedEnv {
    dstd::String name;
    ScopedEnv(dstd::String n, const dstd::String& v) : name(std::move(n)) {
        setenv(name.c_str(), v.c_str(), 1);
    }
    ~ScopedEnv() { unsetenv(name.c_str()); }
};

// `lines` as an indexed NDJSON trace; `member_bytes` > 0 frames it into
// gzip members of about that size, so the index holds several chunks.
dstd::String write_records(TestEnvironment& env, const dstd::String& name,
                           const dstd::Vector<dstd::String>& lines,
                           std::size_t member_bytes = 0,
                           std::size_t checkpoint_size = 0,
                           dstd::Vector<dstd::String> extensions = {}) {
    const dstd::String plain = env.dir() + "/" + name + ".ndjson";
    {
        dftu_utils_test::FileOut o(plain);
        for (const auto& l : lines) o << l << "\n";
    }
    const dstd::String gz = plain + ".gz";
    if (member_bytes)
        REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(
            plain, gz, member_bytes));
    else
        dftu_utils_test::compress_file_to_gzip(plain, gz);
    dstd::fs::remove(plain).discard();
    dftracer::utils::index::IndexerOptions o;
    if (checkpoint_size) o.checkpoint_size = checkpoint_size;
    if (!extensions.empty()) {
        o.extensions.clear();
        for (const auto& extension : extensions)
            o.extensions.emplace_back(extension);
    }
    dftu_utils_test::ok(
        dftu_utils_test::ok(dftracer::utils::index::Indexer::open(
                                {dstd::String(dstd::StringView(gz))}, o))
            .build());
    return gz;
}

View view_of(const dstd::String& gz) {
    return View::from_file(
        dstd::String(gz),
        dstd::String(dstd::String(determine_index_path(gz, ""))));
}

dataframe::DataFrame frame(const View& v) {
    dataframe::DataFrame f = run(v.lazy().collect());
    for (auto& c : f.columns)
        if (!c.is_flat()) c = c.materialize();
    return f;
}

// Row `r` of column `name` as text: `null`, a string, or a number.
dstd::String cell(const dataframe::DataFrame& f, std::int64_t r,
                  dstd::StringView name) {
    const auto i = bcol(f, name);
    REQUIRE(i >= 0);
    const auto& c = f.columns[static_cast<std::size_t>(i)];
    if (c.is_null(r)) return "null";
    if (c.type() == dataframe::TypeId::String) return bstr(f, r, name);
    const double d = bnum(f, r, name);
    if (d == static_cast<double>(static_cast<long long>(d)))
        return dstd::to_string(static_cast<long long>(d));
    return dstd::to_string(d);
}

dstd::Vector<dstd::String> column(const dataframe::DataFrame& f,
                                  dstd::StringView name) {
    dstd::Vector<dstd::String> out;
    for (std::int64_t r = 0; r < f.num_rows(); ++r)
        out.push_back(cell(f, r, name));
    return out;
}

dstd::Vector<dstd::String> sorted(dstd::Vector<dstd::String> v) {
    std::sort(v.begin(), v.end());
    return v;
}

// The message of the error `fn` throws or the View, plan or result it returns
// kept; empty when there is none.
template <typename Fn>
dstd::String error_of(Fn&& fn) {
    if constexpr (std::is_void_v<std::invoke_result_t<Fn&>>) {
        try {
            fn();
        } catch (const std::exception& e) {
            return e.what();
        }
        return {};
    } else {
        const auto status = test_view_common::status_of(fn());
        if (status.has_value()) return {};
        return dstd::String(status.error().message());
    }
}

// The message of the error a view fails with, from building it or from running
// it; empty when it does not fail.
dstd::String error_of_frame(const dstd::Function<View()>& make) {
    try {
        const auto result = dftracer::utils::default_runtime()
                                .submit(make().lazy().collect())
                                .get();
        if (!result.has_value()) return result.error().c_str();
    } catch (const std::exception& e) {
        return e.what();
    }
    return {};
}

bool has(dstd::StringView text, std::initializer_list<const char*> parts) {
    for (const char* p : parts)
        if (text.find(p) == dstd::StringView::npos) {
            MESSAGE(dstd::String(text));
            return false;
        }
    return true;
}

// Runs 1 and 2, then events of runs 1, 2 and 3 in order.
dstd::Vector<dstd::String> runs_and_events() {
    dstd::Vector<dstd::String> out = {
        R"({"type":"run","run":1,"app":"laghos"})",
        R"({"type":"run","run":2,"app":"amg"})",
    };
    for (int i = 0; i < 9; ++i)
        out.push_back(R"({"type":"ev","i":)" + dstd::to_string(i) +
                      R"(,"run":)" + dstd::to_string(i % 3 + 1) + R"(,"dur":)" +
                      dstd::to_string(10 * (i + 1)) + "}");
    return out;
}

}  // namespace

TEST_SUITE("View duql joins") {
    TEST_CASE("a let reads data unless it says from all") {
        TestEnvironment env(10);
        const dstd::String pfw = env.dir() + "/meta.pfw";
        {
            dftu_utils_test::FileOut o(pfw);
            for (int i = 0; i < 3; ++i)
                o << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)"
                  << 100 * (i + 1) << R"(,"dur":5,"args":{}})" << "\n";
            for (int i = 0; i < 2; ++i)
                o << R"({"ph":"M","name":"FH","pid":1,"tid":1,"args":{"name":"/f)"
                  << i << R"(","value":")" << i << R"("}})" << "\n";
        }
        const dstd::String gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        dstd::fs::remove(pfw).discard();
        const View v = view_of(gz);
        const auto f = frame(v.duql(
            R"(let fh = from all | where ph == "M" and name == "FH"; from fh | agg { n = count() })"));
        REQUIRE(f.num_rows() == 1);
        CHECK(bnum(f, 0, "n") == 2);
        const auto d = frame(v.duql(
            R"(let fh = where ph == "M" and name == "FH"; from fh | agg { n = count() })"));
        REQUIRE(d.num_rows() == 1);
        CHECK(bnum(d, 0, "n") == 0);
        CHECK(frame(v.duql(R"(where ph == "M")")).num_rows() == 0);
    }

    TEST_CASE("a correlated sub-query on a trace with no index yet") {
        const dstd::Vector<dstd::String> outers = {
            "", "ph == \"X\" and ", "cat == \"POSIX\" and ", "pid == 1 and "};
        for (const auto& outer : outers) {
            CAPTURE(outer);
            TestEnvironment env(10);
            const dstd::String pfw = env.dir() + "/fresh.pfw";
            double sum[2] = {0, 0};
            int cnt[2] = {0, 0};
            dstd::Vector<int> name_of;
            dstd::Vector<int> dur_of;
            {
                dftu_utils_test::FileOut o(pfw);
                for (int i = 0; i < 60; ++i) {
                    const int k = (i * 7 + i / 3) % 2;
                    const int dur = (i * 37) % 101;
                    name_of.push_back(k);
                    dur_of.push_back(dur);
                    sum[k] += dur;
                    ++cnt[k];
                    o << R"({"ph":"X","name":")" << (k ? "write" : "read")
                      << R"(","cat":"POSIX","pid":1,"tid":)" << i << R"(,"ts":)"
                      << i << R"(,"dur":)" << dur << R"(,"args":{}})" << "\n";
                }
            }
            const dstd::String gz = pfw + ".gz";
            dftu_utils_test::compress_file_to_gzip(pfw, gz);
            dstd::fs::remove(pfw).discard();
            REQUIRE_FALSE(
                dstd::fs::exists(dstd::String(determine_index_path(gz, "")))
                    .value());
            dstd::Vector<dstd::String> want;
            for (int i = 0; i < 60; ++i) {
                const auto k = static_cast<std::size_t>(name_of[i]);
                if (dur_of[i] > sum[k] / cnt[k])
                    want.push_back(dstd::to_string(i));
            }
            REQUIRE_FALSE(want.empty());
            const auto f = frame(view_of(gz).duql(
                "where " + outer +
                "dur > (from data | where name == ^.name | agg { m = "
                "mean(dur) }) | select tid"));
            auto got = column(f, "tid");
            std::sort(got.begin(), got.end());
            std::sort(want.begin(), want.end());
            CHECK(got == want);
        }
    }

    TEST_CASE("unknown names, self and forward references fail to compile") {
        TestEnvironment env(10);
        const View v = view_of(write_records(env, "r", runs_and_events()));
        CHECK(has(error_of([&] { return v.duql("from runs | take 1"); }),
                  {"runs", "not a let or a row set"}));
        CHECK(has(error_of([&] { return v.duql("let a = from a; from a"); }),
                  {"'a'", "itself"}));
        CHECK(has(error_of([&] {
                      return v.duql(
                          "let a = from b; let b = where x > 1; from a");
                  }),
                  {"'b'", "declared after"}));
        CHECK(
            has(error_of([&] { return v.duql("where run -> nope.app == 1"); }),
                {"nope", "not a let or a row set"}));
        CHECK(has(
            error_of([&] { return v.duql("def f(a) = a; where f(1, 2) > 0"); }),
            {"'f'", "argument"}));
        CHECK(has(error_of([&] {
                      return v.duql(
                          "where dur > (from data | where ts < ^.ts + ts | "
                          "agg { m = max(dur) })");
                  }),
                  {"ts < ^.ts + ts", "mixes"}));
    }

    TEST_CASE("a semi-join equals the two-step query") {
        TestEnvironment env(10);
        dstd::Vector<dstd::String> lines;
        for (int i = 0; i < 200; ++i)
            lines.push_back(R"({"i":)" + dstd::to_string(i) + R"(,"name":"n)" +
                            dstd::to_string(i % 17) + R"(","dur":)" +
                            dstd::to_string((i * 37) % 101) + "}");
        const View v = view_of(write_records(env, "s", lines, 1024));
        const auto got = frame(
            v.duql("where name in (from data | where dur > 90 | select name)"));
        const auto names = frame(v.duql("where dur > 90 | select name"));
        dstd::Set<dstd::String> keys;
        for (std::int64_t r = 0; r < names.num_rows(); ++r)
            keys.insert(bstr(names, r, "name"));
        REQUIRE(!keys.empty());
        dstd::String list;
        for (const auto& k : keys)
            list += (list.empty() ? "" : ", ") + ("\"" + k + "\"");
        const auto want = frame(v.duql("where name in [" + list + "]"));
        CHECK(sorted(column(got, "i")) == sorted(column(want, "i")));

        const auto anti = frame(v.duql(
            "where name not in (from data | where dur > 90 | select name)"));
        const auto anti_want =
            frame(v.duql("where name not in [" + list + "]"));
        CHECK(sorted(column(anti, "i")) == sorted(column(anti_want, "i")));
        CHECK(got.num_rows() + anti.num_rows() == 200);
    }

    TEST_CASE("anti-join and missing keys") {
        TestEnvironment env(10);
        const View v = view_of(
            write_records(env, "k", {R"({"k":1})", R"({"k":2})", R"({})"}));
        const auto f = frame(
            v.duql("where k not in (from data | where k == 1 | select k)"));
        CHECK(column(f, "k") == dstd::Vector<dstd::String>{"2"});
        CHECK(frame(v.duql("where k in (from data | where k == 1 | select k)"))
                  .num_rows() == 1);
    }

    TEST_CASE("keys compare by duql value") {
        TestEnvironment env(10);
        const View v = view_of(write_records(
            env, "eq",
            {R"({"n":4})", R"({"s":"4"})", R"({"d":4.0})",
             R"({"big":9007199254740993})", R"({"near":9007199254740992.0})"}));
        CHECK(
            frame(v.duql("where n in (from data | where exists(s) | select s)"))
                .num_rows() == 0);
        CHECK(
            frame(v.duql("where n in (from data | where exists(d) | select d)"))
                .num_rows() == 1);
        CHECK(frame(v.duql(
                        "where big in (from data | where exists(near) | select "
                        "near)"))
                  .num_rows() == 0);
    }

    TEST_CASE("a sub-query must give the columns 'in' compares") {
        TestEnvironment env(10);
        const View v = view_of(write_records(env, "w", runs_and_events()));
        CHECK(has(error_of([&] {
                      return v.duql(
                          "where run in (from data | select run, app)");
                  }),
                  {"2 columns", "compares 1"}));
        const auto t = frame(v.duql(
            "where type == \"ev\" and (run, i) in (from data | where i < 3 | "
            "select run, i)"));
        CHECK(column(t, "i") == dstd::Vector<dstd::String>{"0", "1", "2"});
    }

    TEST_CASE("an arrow in a filter, under not, and with several matches") {
        TestEnvironment env(10);
        const View v = view_of(write_records(env, "a", runs_and_events()));
        const dstd::String let = "let runs = where type == \"run\"; ";
        const auto f = frame(v.duql(
            let + "where type == \"ev\" and run -> runs.app == \"laghos\""));
        CHECK(column(f, "i") == dstd::Vector<dstd::String>{"0", "3", "6"});

        // Joined by hand: run 1 is laghos, run 2 amg, run 3 has no row.
        const auto n = frame(v.duql(
            let +
            "where type == \"ev\" and not (run -> runs.app == \"laghos\")"));
        CHECK(column(n, "i") == dstd::Vector<dstd::String>{"1", "4", "7"});
        const auto o = frame(v.duql(
            let +
            "where type == \"ev\" and (run -> runs.app == \"amg\" or i == 2)"));
        CHECK(column(o, "i") == dstd::Vector<dstd::String>{"1", "2", "4", "7"});

        const auto d =
            frame(v.duql(
                let + "where type == \"ev\" | derive app = run -> runs.app | "
                      "select i, app"));
        CHECK(column(d, "app") == dstd::Vector<dstd::String>{
                                      "laghos", "amg", "null", "laghos", "amg",
                                      "null", "laghos", "amg", "null"});

        auto lines = runs_and_events();
        lines.push_back(R"({"type":"run","run":1,"app":"amg"})");
        const View dup = view_of(write_records(env, "dup", lines));
        CHECK(has(error_of_frame([&] {
                      return dup.duql(let + "derive app = run -> runs.app");
                  }),
                  {"'runs'", "key 1"}));
        CHECK(has(error_of_frame([&] {
                      return dup.duql(
                          let +
                          "where type == \"ev\" and ((run -> runs.app) "
                          "?? \"\") == \"amg\"");
                  }),
                  {"'runs'", "key 1"}));
        CHECK(has(dftu_utils_test::ok(dup.explain_duql(
                      let + "where type == \"ev\" and ((run -> "
                            "runs.app) ?? \"\") == \"amg\"")),
                  {"scan filter:", "(pushed)"}));
        const auto any = frame(dup.duql(
            let + "where type == \"ev\" and run -> runs.app == \"amg\""));
        CHECK(column(any, "i") ==
              dstd::Vector<dstd::String>{"0", "1", "3", "4", "6", "7"});
    }

    TEST_CASE("arrow forms: a named key, a tuple and a chain") {
        TestEnvironment env(10);
        const View v = view_of(
            write_records(env, "f",
                          {R"({"type":"run","rid":1,"sys":"x","app":"laghos"})",
                           R"({"type":"sys","sys":"x","cpu":"epyc"})",
                           R"({"type":"ev","run":1,"i":0})",
                           R"({"type":"ev","run":2,"i":1})"}));
        const dstd::String lets =
            "let runs = where type == \"run\"; let systems = where type == "
            "\"sys\"; ";
        const auto a = frame(
            v.duql(lets +
                   "where type == \"ev\" | derive app = run -> runs(rid).app | "
                   "select i, app"));
        CHECK(column(a, "app") == dstd::Vector<dstd::String>{"laghos", "null"});
        const auto c = frame(v.duql(
            lets +
            "where type == \"ev\" | derive cpu = run -> runs(rid).sys -> "
            "systems.cpu | select i, cpu"));
        CHECK(column(c, "cpu") == dstd::Vector<dstd::String>{"epyc", "null"});
        const auto t = frame(v.duql(
            lets +
            "where type == \"run\" | derive app = (rid, sys) -> runs.app "
            "| select app"));
        CHECK(column(t, "app") == dstd::Vector<dstd::String>{"laghos"});
    }

    TEST_CASE("a scalar sub-query") {
        TestEnvironment env(10);
        const View v = view_of(write_records(env, "sc", runs_and_events()));
        const auto f = frame(v.duql(
            "where type == \"ev\" | derive share = dur / (from data | where "
            "type == \"ev\" | agg { t = sum(dur) }) | select share"));
        double total = 0;
        for (std::int64_t r = 0; r < f.num_rows(); ++r)
            total += bnum(f, r, "share");
        CHECK(total == doctest::Approx(1.0));
        CHECK(has(error_of_frame([&] {
                      return v.duql(
                          "derive x = (from data | where type == \"ev\" | "
                          "select dur)");
                  }),
                  {"__sub_0", "one row"}));
        const auto none = frame(v.duql(
            "where type == \"run\" | derive x = (from data | where type == "
            "\"none\" | agg { m = max(dur) }) | select x"));
        CHECK(column(none, "x") == dstd::Vector<dstd::String>{"null", "null"});
    }

    TEST_CASE("lookup attaches columns and into gathers every match") {
        TestEnvironment env(10);
        auto lines = runs_and_events();
        lines.push_back(R"({"type":"host","run":1,"host":"a"})");
        lines.push_back(R"({"type":"host","run":1,"host":"b"})");
        const View v = view_of(write_records(env, "l", lines));
        const auto f = frame(v.duql(
            "let runs = where type == \"run\" | select run, app; where type == "
            "\"ev\" | lookup runs on run | select i, app"));
        CHECK(column(f, "i") == dstd::Vector<dstd::String>{"0", "1", "2", "3",
                                                           "4", "5", "6", "7",
                                                           "8"});
        CHECK(column(f, "app") == dstd::Vector<dstd::String>{
                                      "laghos", "amg", "null", "laghos", "amg",
                                      "null", "laghos", "amg", "null"});

        const auto e = frame(v.duql(
            "let hosts = where type == \"host\" | select run, host; where type "
            "== \"ev\" and run == 1 | lookup hosts on run into hs | expand hs "
            "| select i, hs.host"));
        CHECK(column(e, "i") ==
              dstd::Vector<dstd::String>{"0", "0", "3", "3", "6", "6"});
        CHECK(column(e, "hs.host") ==
              dstd::Vector<dstd::String>{"a", "b", "a", "b", "a", "b"});
        const auto empty = frame(v.duql(
            "let hosts = where type == \"host\" | select run, host; where type "
            "== \"ev\" and run == 3 | lookup hosts on run into hs | derive n = "
            "len(hs) | select n"));
        CHECK(column(empty, "n") == dstd::Vector<dstd::String>{"0", "0", "0"});

        CHECK(has(error_of_frame([&] {
                      return v.duql(
                          "let hosts = where type == \"host\" | select run, "
                          "host; where type == \"ev\" | lookup hosts on run");
                  }),
                  {"hosts", "key 1"}));
        CHECK(has(error_of_frame([&] {
                      return v.duql(
                          "let runs = where type == \"run\" | select run, "
                          "type; where type == \"ev\" | lookup runs on run");
                  }),
                  {"'type'", "already"}));
        CHECK(has(error_of([&] {
                      return v.duql(
                          "let runs = where type == \"run\" | select run, "
                          "k = 1; where type == \"ev\" | derive k = 2 | "
                          "lookup runs on run");
                  }),
                  {"'k'", "already"}));
        const auto k2 = frame(v.duql(
            "let runs = where type == \"run\" | select rid = run, app; where "
            "type == \"ev\" and i < 2 | lookup runs on run == rid | select "
            "app"));
        CHECK(column(k2, "app") == dstd::Vector<dstd::String>{"laghos", "amg"});
    }

    TEST_CASE("a lookup column is the needle of a string predicate") {
        TestEnvironment env(10);
        dstd::Vector<dstd::String> lines = {
            R"({"type":"run","run":1,"prefix":"/pfs"})",
            R"({"type":"run","run":2,"prefix":"/ssd"})",
        };
        const char* const FN[] = {"/pfs/a", "/ssd/b", "/pfs/c", "/tmp/d"};
        for (int i = 0; i < 4; ++i)
            lines.push_back(R"({"type":"ev","i":)" + dstd::to_string(i) +
                            R"(,"run":)" + dstd::to_string(i % 3 + 1) +
                            R"(,"fname":")" + FN[i] + R"("})");
        lines.push_back(R"({"type":"ev","i":4,"run":1})");
        const View v = view_of(write_records(env, "pc", lines));
        const dstd::String q =
            "let runs = where type == \"run\" | select run, prefix; where "
            "type == \"ev\" | lookup runs on run | derive p = "
            "starts_with(fname, prefix) | ";
        CHECK(column(frame(v.duql(q + "where p | select i")), "i") ==
              dstd::Vector<dstd::String>{"0", "1"});
        CHECK(column(frame(v.duql(q + "where not p | select i")), "i") ==
              dstd::Vector<dstd::String>{"3"});
        CHECK(column(frame(v.duql(q + "where p is null | select i")), "i") ==
              dstd::Vector<dstd::String>{"2", "4"});
    }

    TEST_CASE("lookup inner and anti keep or drop the rows with a match") {
        TestEnvironment env(10);
        auto lines = runs_and_events();
        lines.push_back(R"({"type":"run","run":3})");
        lines.push_back(R"({"type":"ev","i":9,"run":null})");
        lines.push_back(R"({"type":"ev","i":10,"run":4})");
        const View v = view_of(write_records(env, "ia", lines));
        const dstd::String let =
            "let runs = where type == \"run\" and run < 3 | select run, "
            "sapp = app; ";
        const dstd::String ev = "where type == \"ev\" | ";

        const auto left =
            frame(v.duql(let + ev +
                         "lookup runs on run | where sapp == \"laghos\" or "
                         "sapp == \"amg\" | select i, sapp"));
        const auto inner = frame(
            v.duql(let + ev + "lookup runs on run inner | select i, sapp"));
        CHECK(column(inner, "i") == column(left, "i"));
        CHECK(column(inner, "sapp") == column(left, "sapp"));
        CHECK(column(inner, "i").size() == 6);

        const auto nullable = frame(v.duql(
            "let runs = where type == \"run\" | select run, sapp = app; " + ev +
            "lookup runs on run inner | select i, sapp"));
        CHECK(column(nullable, "i") ==
              dstd::Vector<dstd::String>{"0", "1", "2", "3", "4", "5", "6", "7",
                                         "8"});
        CHECK(column(nullable, "sapp") ==
              dstd::Vector<dstd::String>{"laghos", "amg", "null", "laghos",
                                         "amg", "null", "laghos", "amg",
                                         "null"});

        const auto into = frame(
            v.duql(let + ev + "lookup runs on run inner into m | select i, m"));
        CHECK(column(into, "i") == column(inner, "i"));

        const auto anti =
            frame(v.duql(let + ev + "lookup runs on run anti | select i"));
        CHECK(column(anti, "i") ==
              dstd::Vector<dstd::String>{"2", "5", "8", "9", "10"});
        const auto not_in = frame(v.duql(
            let + ev + "where run not in (from runs | select run) | select i"));
        CHECK(column(not_in, "i") ==
              dstd::Vector<dstd::String>{"2", "5", "8", "10"});
        const auto all_cols =
            frame(v.duql(let + ev + "lookup runs on run anti"));
        CHECK(all_cols.column_index("sapp") < 0);

        CHECK(has(error_of_frame([&] {
                      return v.duql(
                          "let runs = where type == \"run\" | select run, "
                          "type; " +
                          ev + "lookup runs on run inner");
                  }),
                  {"'type'", "already"}));
    }

    TEST_CASE("a lookup side may be inline but not correlated") {
        TestEnvironment env(10);
        const View v = view_of(write_records(env, "il", runs_and_events()));
        const auto a = frame(v.duql(
            "let runs = where type == \"run\" | select run, app; where type "
            "== \"ev\" | lookup runs on run | select i, app"));
        const auto b = frame(
            v.duql("where type == \"ev\" | lookup (from data | where type == "
                   "\"run\" | select run, app) on run | select i, app"));
        CHECK(column(b, "i") == column(a, "i"));
        CHECK(column(b, "app") == column(a, "app"));
        CHECK(has(error_of([&] {
                      return v.duql(
                          "where type == \"ev\" | lookup (from data | where "
                          "run == ^.run | select run, app) on run");
                  }),
                  {"lookup side", "enclosing row"}));
    }

    // Samples `{"type":"s","k":..,"t":..,"v":..}` and events
    // `{"type":"ev","i":..,"k":..,"ts":..}` with equal times, missing keys
    // and missing times; `expect` is the nested-loop answer per event.
    static dstd::String text_of(const dstd::Vector<dstd::String>& v) {
        dstd::String out;
        for (const auto& x : v) out += x + ",";
        return out;
    }

    static dstd::String first_diff(const dstd::Vector<dstd::String>& got,
                                   const dstd::Vector<dstd::String>& want) {
        for (std::size_t r = 0; r < std::min(got.size(), want.size()); ++r)
            if (got[r] != want[r])
                return "row " + dstd::to_string(r) + ": got " + got[r] +
                       ", want " + want[r];
        return got.size() == want.size() ? "" : "sizes differ";
    }

    struct AsofData {
        dstd::Vector<dstd::String> lines;
        dstd::Vector<dstd::Vector<dstd::String>> expect;
    };

    static AsofData asof_data(int events, int samples, unsigned seed) {
        struct Sample {
            int k, t, v;
            bool has_k, has_t;
        };
        dstd::Vector<Sample> smp;
        dstd::Vector<dstd::String> lines;
        unsigned x = seed;
        auto next = [&](unsigned m) {
            x = x * 1664525u + 1013904223u;
            return (x >> 8) % m;
        };
        for (int j = 0; j < samples; ++j) {
            Sample s{static_cast<int>(next(3)), static_cast<int>(next(120)), j,
                     next(10) != 0, next(10) != 0};
            smp.push_back(s);
            dstd::String l = R"({"type":"s","v":)" + dstd::to_string(j);
            if (s.has_k) l += R"(,"k":)" + dstd::to_string(s.k);
            if (s.has_t) l += R"(,"t":)" + dstd::to_string(s.t);
            lines.push_back(l + R"(,"pad":"xxxxxxxxxxxxxxxxxxxxxxxx"})");
        }
        AsofData d;
        struct Ev {
            int i, k, ts;
            bool has_k, has_ts;
        };
        dstd::Vector<Ev> evs;
        for (int i = 0; i < events; ++i) {
            Ev e{i, static_cast<int>(next(3)), static_cast<int>(next(120)),
                 next(10) != 0, next(10) != 0};
            evs.push_back(e);
            dstd::String l = R"({"type":"ev","i":)" + dstd::to_string(i);
            if (e.has_k) l += R"(,"k":)" + dstd::to_string(e.k);
            if (e.has_ts) l += R"(,"ts":)" + dstd::to_string(e.ts);
            lines.push_back(l + R"(,"pad":"xxxxxxxxxxxxxxxxxxxxxxxx"})");
        }
        d.lines = std::move(lines);
        // backward, forward, nearest, each without and with `within 4`.
        for (int mode = 0; mode < 6; ++mode) {
            dstd::Vector<dstd::String> col;
            for (const Ev& e : evs) {
                int back = -1;
                int fwd = -1;
                if (e.has_k && e.has_ts)
                    for (int j = 0; j < samples; ++j) {
                        const Sample& s = smp[static_cast<std::size_t>(j)];
                        if (!s.has_k || !s.has_t || s.k != e.k) continue;
                        if (s.t <= e.ts &&
                            (back < 0 ||
                             s.t >= smp[static_cast<std::size_t>(back)].t))
                            back = j;
                        if (s.t >= e.ts &&
                            (fwd < 0 ||
                             s.t < smp[static_cast<std::size_t>(fwd)].t))
                            fwd = j;
                    }
                auto dist = [&](int j) {
                    return std::abs(smp[static_cast<std::size_t>(j)].t - e.ts);
                };
                int pick = mode % 3 == 0 ? back : fwd;
                if (mode % 3 == 2)
                    pick =
                        back < 0
                            ? fwd
                            : (fwd < 0 || dist(back) <= dist(fwd) ? back : fwd);
                if (pick >= 0 && mode >= 3 && dist(pick) > 4) pick = -1;
                col.push_back(pick < 0 ? "null" : dstd::to_string(pick));
            }
            d.expect.push_back(std::move(col));
        }
        return d;
    }

    TEST_CASE("an as-of lookup equals a nested-loop reference") {
        TestEnvironment env(10);
        const AsofData d = asof_data(300, 80, 11);
        const View v = view_of(write_records(env, "a", d.lines, 4096, 4096));
        const char* clauses[] = {
            "",         "forward",          "nearest",
            "within 4", "forward within 4", "nearest within 4"};
        for (std::size_t m = 0; m < 6; ++m) {
            CAPTURE(clauses[m]);
            const auto f = frame(v.duql(
                dstd::String("let s = where type == \"s\" | select k, t, v; "
                             "where type == \"ev\" | lookup s on k asof ts == "
                             "t ") +
                clauses[m] + " | select i, v"));
            REQUIRE(f.num_rows() == 300);
            CHECK(first_diff(column(f, "v"), d.expect[m]) == "");
            dstd::Vector<dstd::String> order;
            for (int i = 0; i < 300; ++i) order.push_back(dstd::to_string(i));
            CHECK(text_of(column(f, "i")) == text_of(order));
        }
    }

    TEST_CASE("within takes a number, a fraction, a parameter or a duration") {
        TestEnvironment env(10);
        const AsofData d = asof_data(300, 80, 11);
        const View v = view_of(write_records(env, "w", d.lines, 4096, 4096));
        auto query_with = [&](const dstd::String& within,
                              const duql::Params& p = {}) {
            return v.duql(
                "let s = where type == \"s\" | select k, t, v; "
                "where type == \"ev\" | lookup s on k asof ts "
                "== t within " +
                    within + " | select i, v",
                p);
        };
        auto run_with = [&](const dstd::String& within,
                            const duql::Params& p = {}) {
            return column(frame(query_with(within, p)), "v");
        };
        duql::Params p;
        p.emplace("w", duql::LiteralValue{std::uint64_t{4}});
        for (const dstd::String w : {"4", "4.5", "$w"}) {
            CAPTURE(w);
            CHECK(first_diff(run_with(w, p), d.expect[3]) == "");
        }
        for (const dstd::String w : {"-1", "ts", "1 + 3", "4us"}) {
            CAPTURE(w);
            CHECK(
                has(error_of_frame([&] { return query_with(w); }), {"within"}));
        }
    }

    TEST_CASE("an as-of lookup does not depend on pushdown or layout") {
        TestEnvironment env(10);
        const AsofData d = asof_data(300, 80, 5);
        dstd::Vector<dstd::String> want;
        for (const std::size_t checkpoint :
             {std::size_t{2048}, std::size_t{65536}}) {
            const dstd::String gz =
                write_records(env, "b" + dstd::to_string(checkpoint), d.lines,
                              4096, checkpoint);
            for (const char* where :
                 {"type == \"ev\"", "(type == \"ev\") or false"}) {
                CAPTURE(where);
                const auto f = frame(view_of(gz).duql(
                    dstd::String("let s = where type == \"s\" | select k, t, "
                                 "v; where ") +
                    where +
                    " | lookup s on k asof ts == t nearest | select i, v"));
                const auto got = column(f, "v");
                if (want.empty()) want = got;
                CHECK(got == want);
            }
        }
        CHECK(want == d.expect[2]);
    }

    TEST_CASE("as-of lookup errors and explain") {
        TestEnvironment env(10);
        const View v = view_of(write_records(env, "c", runs_and_events()));
        const dstd::String plan = dftu_utils_test::ok(v.explain_duql(
            "let runs = where type == \"run\" | select run, when = run, app; "
            "where type == \"ev\" | lookup runs on run asof i == when "
            "forward within 3"));
        CHECK(has(plan, {"lookup runs on run == run asof i forward within 3",
                         "dftu.frame.asof"}));
        const auto filled = frame(v.duql(
            "let runs = where type == \"run\" | select run, when = run, app; "
            "where type == \"ev\" | lookup runs on run asof i == when | "
            "select i, app"));
        CHECK(column(filled, "app") ==
              dstd::Vector<dstd::String>{"null", "null", "null", "laghos",
                                         "amg", "null", "laghos", "amg",
                                         "null"});
        CHECK(has(error_of([&] {
                      return v.duql(
                          "let runs = where type == \"run\" | select run, "
                          "app; where type == \"ev\" | lookup runs on run "
                          "asof i == app");
                  }),
                  {"asof", "app", "string"}));
        CHECK(has(error_of([&] {
                      return v.duql(
                          "let runs = where type == \"run\" | select run, "
                          "app; where type == \"ev\" | lookup runs on run "
                          "asof i == run into m");
                  }),
                  {"cannot be combined"}));
    }

    struct OverlapData {
        dstd::Vector<dstd::String> lines;
        dstd::Vector<dstd::String> pairs;
        dstd::Vector<dstd::String> nested;
    };

    // dftracer events: phases and calls with a pid, a start and a duration;
    // `pairs` is the nested-loop answer as `call:phase` in call order, a call
    // with no phase as `call:null`, `nested` the phases of each call.
    static OverlapData overlap_data(int calls, int phases, unsigned seed) {
        struct Span {
            int pid, ts, dur;
        };
        unsigned x = seed;
        auto next = [&](unsigned m) {
            x = x * 1664525u + 1013904223u;
            return (x >> 8) % m;
        };
        dstd::Vector<Span> ph;
        dstd::Vector<Span> cl;
        OverlapData d;
        auto line = [&](const char* name, int id, const Span& s) {
            return dstd::String(R"({"ph":"X","name":")") + name +
                   R"(","cat":"c","pid":)" + dstd::to_string(s.pid) +
                   R"(,"tid":)" + dstd::to_string(id) + R"(,"ts":)" +
                   dstd::to_string(s.ts) + R"(,"dur":)" +
                   dstd::to_string(s.dur) +
                   R"(,"pad":"xxxxxxxxxxxxxxxxxxxxxxxx"})";
        };
        for (int j = 0; j < phases; ++j) {
            ph.push_back({static_cast<int>(next(3)),
                          static_cast<int>(next(200)),
                          static_cast<int>(next(30))});
            d.lines.push_back(line("phase", j, ph.back()));
        }
        for (int i = 0; i < calls; ++i) {
            cl.push_back({static_cast<int>(next(3)),
                          static_cast<int>(next(200)),
                          static_cast<int>(next(12))});
            d.lines.push_back(line("call", 1000 + i, cl.back()));
        }
        for (int i = 0; i < calls; ++i) {
            const Span& c = cl[static_cast<std::size_t>(i)];
            dstd::String list;
            int found = 0;
            for (int j = 0; j < phases; ++j) {
                const Span& p = ph[static_cast<std::size_t>(j)];
                if (p.pid != c.pid || !(p.ts < c.ts + c.dur) ||
                    !(c.ts < p.ts + p.dur))
                    continue;
                d.pairs.push_back(dstd::to_string(1000 + i) + ":" +
                                  dstd::to_string(j));
                list += dstd::to_string(j) + ",";
                ++found;
            }
            if (!found) d.pairs.push_back(dstd::to_string(1000 + i) + ":null");
            d.nested.push_back(list);
        }
        return d;
    }

    TEST_CASE("an overlap lookup equals a nested loop") {
        TestEnvironment env(10);
        const OverlapData d = overlap_data(200, 60, 3);
        for (const std::size_t checkpoint :
             {std::size_t{2048}, std::size_t{65536}}) {
            const dstd::String gz =
                write_records(env, "o" + dstd::to_string(checkpoint), d.lines,
                              4096, checkpoint);
            for (const char* where :
                 {"name == \"call\"", "(name == \"call\") or false"}) {
                CAPTURE(checkpoint);
                CAPTURE(where);
                const auto f = frame(view_of(gz).duql(
                    dstd::String("let ph = where name == \"phase\" | select "
                                 "pid, ts, dur, ptid = tid; where ") +
                    where + " | lookup ph on pid overlap | select tid, ptid"));
                dstd::Vector<dstd::String> got;
                for (std::int64_t r = 0; r < f.num_rows(); ++r)
                    got.push_back(cell(f, r, "tid") + ":" + cell(f, r, "ptid"));
                CHECK(text_of(got) == text_of(d.pairs));
            }
        }
    }

    TEST_CASE("an overlap lookup into gathers the matches") {
        TestEnvironment env(10);
        const OverlapData d = overlap_data(120, 40, 9);
        const View v = view_of(write_records(env, "n", d.lines, 4096, 4096));
        const auto f = frame(v.duql(
            "let ph = where name == \"phase\" | select pid, ts, dur, ptid = "
            "tid; where name == \"call\" | lookup ph on pid overlap into m "
            "| select tid, len(m) as n"));
        REQUIRE(f.num_rows() == 120);
        dstd::Vector<dstd::String> counts;
        for (const auto& l : d.nested) {
            int n = 0;
            for (const char c : l) n += c == ',';
            counts.push_back(dstd::to_string(n));
        }
        CHECK(text_of(column(f, "n")) == text_of(counts));
    }

    TEST_CASE("overlap lookup errors and explain") {
        TestEnvironment env(10);
        const OverlapData d = overlap_data(20, 10, 1);
        const View v = view_of(write_records(env, "e2", d.lines));
        const dstd::String plan = dftu_utils_test::ok(v.explain_duql(
            "let ph = where name == \"phase\" | select pid, ts, dur; where "
            "name == \"call\" | lookup ph on pid overlap"));
        CHECK(has(plan, {"lookup ph on pid == pid overlap", "sweep over ph"}));
        CHECK(has(error_of([&] {
                      return v.duql(
                          "let ph = where name == \"phase\" | select pid, "
                          "ts; where name == \"call\" | lookup ph on pid "
                          "overlap");
                  }),
                  {"overlap", "'dur'"}));
        const View plain = view_of(write_records(env, "e3", runs_and_events()));
        CHECK(has(error_of([&] {
                      return plain.duql(
                          "let runs = where type == \"run\" | select run; "
                          "where type == \"ev\" | lookup runs on run "
                          "overlap");
                  }),
                  {"overlap", "role"}));
    }

    TEST_CASE("union and from a, b") {
        TestEnvironment env(10);
        const dstd::String a = write_records(
            env, "ua", {R"({"x":1,"y":"p"})", R"({"x":2,"y":"q"})"});
        const dstd::String b =
            write_records(env, "ub", {R"({"x":2.5,"z":true})"});
        const auto n = frame(view_of(a).duql("union (from \"" + b +
                                             "\") | agg { n = count() }"));
        CHECK(bnum(n, 0, "n") == 3);
        const auto u = frame(view_of(a).duql("select x, y | union (from \"" +
                                             b + "\" | select x, z)"));
        CHECK(u.names == dstd::Vector<dstd::String>{"x", "y", "z"});
        CHECK(column(u, "x") ==
              dstd::Vector<dstd::String>{"1", "2", "2.500000"});
        CHECK(column(u, "y") == dstd::Vector<dstd::String>{"p", "q", "null"});
        const auto both =
            frame(view_of(a).duql("let one = where x == 1; let two = from \"" +
                                  b + "\"; from one, two | select x"));
        CHECK(column(both, "x") == dstd::Vector<dstd::String>{"1", "2.500000"});
        CHECK(has(error_of([&] {
                      return view_of(a).duql("select y | union (from \"" + b +
                                             "\" | select y = z)");
                  }),
                  {"'y'", "union"}));
    }

    TEST_CASE("caps fail loudly, only when the query runs") {
        TestEnvironment env(10);
        dstd::Vector<dstd::String> lines;
        for (int i = 0; i < 11; ++i)
            lines.push_back(R"({"k":)" + dstd::to_string(i) + "}");
        const View v = view_of(write_records(env, "cap", lines));
        const dstd::String q =
            "let ks = where k >= 0 | select k, v = k; where k -> ks.v >= 0";
        {
            ScopedEnv cap("DUQL_LOOKUP_MAX_ROWS", "10");
            const View planned = v.duql(q);
            CHECK(v.explain_duql(q).has_value());
            CHECK(has(error_of_frame([&] { return planned; }),
                      {"'ks'", "11", "10", "DUQL_LOOKUP_MAX_ROWS"}));
            CHECK(has(error_of_frame([&] {
                          return v.duql(
                              "derive m = (from data | where k < ^.k | agg { "
                              "m = max(k) })");
                      }),
                      {"__sub_0", "DUQL_LOOKUP_MAX_ROWS"}));
            // Sub-queries under 'in' and ones keyed by '==' have no cap.
            CHECK(
                frame(v.duql("where k in (from data | select k)")).num_rows() ==
                11);
            CHECK(frame(v.duql("where not (k in (from data | select k))"))
                      .num_rows() == 0);
            CHECK(has(dftu_utils_test::ok(
                          v.explain_duql("where not (k in (from data | select "
                                         "k)) and k > 1")),
                      {"scan filter: k > 1", "lookup join __sub_0 on k"}));
            CHECK(has(dftu_utils_test::ok(
                          v.explain_duql("where k in (from data | select k)")),
                      {"reads the distinct rows of __sub_0",
                       "scan filter: k in (__sub_0)"}));
            CHECK(has(dftu_utils_test::ok(v.explain_duql(
                          "derive n = (from data | where k == ^.k "
                          "| agg { c = count() })")),
                      {"nest join __sub_0 on k"}));
            CHECK(frame(v.duql("derive n = (from data | where k == ^.k | agg "
                               "{ c = count() }) | where n == 1"))
                      .num_rows() == 11);
        }
        {
            ScopedEnv cap("DUQL_LOOKUP_MAX_BYTES", "16");
            CHECK(has(error_of_frame([&] { return v.duql(q); }),
                      {"'ks'", "DUQL_LOOKUP_MAX_BYTES", "16"}));
        }
        CHECK(frame(v.duql(q)).num_rows() == 11);
    }

    TEST_CASE("key-set pushdown prunes chunks and keeps the rows") {
        TestEnvironment env(10);
        dstd::Vector<dstd::String> lines;
        for (const char* name : {"a", "b", "c", "d"})
            for (int i = 0; i < 400; ++i)
                lines.push_back(R"({"name":")" + dstd::String(name) +
                                R"(","i":)" + dstd::to_string(i) +
                                R"(,"pad":"xxxxxxxxxxxxxxxxxxxxxxxx"})");
        const View v = view_of(write_records(env, "p", lines, 4096, 4096));
        const dstd::String sub =
            "name in (from data | where name == \"a\" | select name)";
        NullSink sink;
        const auto pushed =
            dftu_utils_test::ok(v.duql("where " + sub).sink_json(sink).get());
        const auto kept = dftu_utils_test::ok(
            v.duql("where name != \"z\"").sink_json(sink).get());
        CHECK(pushed.events_matched == 400);
        CHECK(kept.events_matched == 1600);
        CHECK(pushed.chunks_skipped > 0);
        CHECK(pushed.chunks_skipped > kept.chunks_skipped);
        CHECK(
            sorted(column(frame(v.duql("where " + sub)), "i")) ==
            sorted(column(frame(v.duql("where (" + sub + ") or false")), "i")));

        const dstd::String arrow =
            "let first = where i == 0 | select name, tag = name; where name -> "
            "first.tag == \"b\"";
        const auto a = dftu_utils_test::ok(v.duql(arrow).sink_json(sink).get());
        CHECK(a.events_matched == 400);
        CHECK(a.chunks_skipped > 0);
    }

    TEST_CASE("a key set prunes on min and max alone") {
        TestEnvironment env(10);
        dstd::Vector<dstd::String> lines;
        for (int i = 0; i < 2000; ++i)
            lines.push_back(R"({"blk":)" + dstd::to_string(i / 100) +
                            R"(,"i":)" + dstd::to_string(i) +
                            R"(,"pad":"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"})");
        const View v =
            view_of(write_records(env, "z", lines, 4096, 4096, {"zonemap"}));
        NullSink sink;
        const auto listed = dftu_utils_test::ok(
            v.duql("where blk in [3, 4]").sink_json(sink).get());
        CHECK(listed.events_matched == 200);
        CHECK(listed.chunks_skipped > 0);
        const auto joined = dftu_utils_test::ok(
            v.duql("where blk in (from data | where i == 350 or i == 450 | "
                   "select blk)")
                .sink_json(sink)
                .get());
        CHECK(joined.events_matched == 200);
        CHECK(joined.chunks_skipped == listed.chunks_skipped);
    }

    TEST_CASE("a semi-join over more keys than the pushdown cap") {
        TestEnvironment env(10);
        dstd::Vector<dstd::String> lines;
        for (int i = 0; i < 5000; ++i)
            lines.push_back(R"({"k":)" + dstd::to_string(i) + "}");
        const View v = view_of(write_records(env, "wide", lines, 8192));
        CHECK(frame(v.duql("where k in (from data | where k >= 10 | select k)"))
                  .num_rows() == 4990);
        CHECK(frame(v.duql("where k not in (from data | where k >= 10 | select "
                           "k)"))
                  .num_rows() == 10);
    }

    TEST_CASE("explain lists sides and pushed key sets without reading") {
        TestEnvironment env(10);
        const View v = view_of(write_records(env, "e", runs_and_events()));
        const dstd::String plan = dftu_utils_test::ok(v.explain_duql(
            "let runs = where type == \"run\"; where run -> runs.app == "
            "\"laghos\""));
        CHECK(has(plan, {"side runs: from data; not cached",
                         "run in (keys of runs) (pushed)"}));
        const dstd::String not_pushed = dftu_utils_test::ok(v.explain_duql(
            "let runs = where type == \"run\"; where not (run -> runs.app == "
            "\"laghos\")"));
        CHECK(not_pushed.find("keys of runs") == dstd::String::npos);
        const dstd::String stages = dftu_utils_test::ok(v.explain_duql(
            "let runs = where type == \"run\" | select run, app; lookup runs "
            "on run | union (from data)"));
        CHECK(has(stages, {"lookup runs on run", "union from data"}));
    }

    TEST_CASE("lookup sides are cached and invalidated by a re-index") {
        TestEnvironment env(10);
        auto lines = runs_and_events();
        const dstd::String gz = write_records(env, "c", lines);
        const dstd::String q =
            "let runs = where type == \"run\"; where type == \"ev\" and run "
            "-> runs.app == \"laghos\"";
        CHECK(has(dftu_utils_test::ok(view_of(gz).explain_duql(q)),
                  {"side runs: from data; not cached"}));
        const auto first = column(frame(view_of(gz).duql(q)), "i");
        CHECK(first == dstd::Vector<dstd::String>{"0", "3", "6"});
        CHECK(has(dftu_utils_test::ok(view_of(gz).explain_duql(q)),
                  {"side runs: from data; cached, 2 rows"}));
        CHECK(column(frame(view_of(gz).duql(q)), "i") == first);

        lines.push_back(R"({"type":"run","run":3,"app":"laghos"})");
        dstd::fs::remove(gz).has_value();
        const dstd::String again = write_records(env, "c", lines);
        REQUIRE(again == gz);
        CHECK(has(dftu_utils_test::ok(view_of(gz).explain_duql(q)),
                  {"not cached"}));
        CHECK(column(frame(view_of(gz).duql(q)), "i") ==
              dstd::Vector<dstd::String>{"0", "2", "3", "5", "6", "8"});

        const dstd::String cache(dftracer::utils::index::lookups_dir(
            dstd::String(determine_index_path(gz, ""))));
        REQUIRE(dstd::fs::exists(cache).value());
        dstd::fs::remove_all(cache).has_value();
        CHECK(column(frame(view_of(gz).duql(q)), "i") ==
              dstd::Vector<dstd::String>{"0", "2", "3", "5", "6", "8"});
    }

    TEST_CASE("results do not depend on checkpoint size or cache state") {
        TestEnvironment env(10);
        dstd::Vector<dstd::String> lines;
        for (int r = 1; r <= 4; ++r)
            lines.push_back(R"({"type":"run","run":)" + dstd::to_string(r) +
                            R"(,"app":"a)" + dstd::to_string(r % 2) + "\"}");
        for (int i = 0; i < 200; ++i)
            lines.push_back(R"({"type":"ev","i":)" + dstd::to_string(i) +
                            R"(,"run":)" + dstd::to_string(i % 5 + 1) +
                            R"(,"dur":)" + dstd::to_string((i * 13) % 97) +
                            R"(,"pad":"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"})");
        const dstd::Vector<dstd::String> queries = {
            "let runs = where type == \"run\"; where type == \"ev\" and run "
            "-> runs.app == \"a1\" | select i",
            "let runs = where type == \"run\"; where type == \"ev\" and not "
            "(run -> runs.app == \"a1\") | select i",
            "where type == \"ev\" and run in (from data | where type == "
            "\"run\" | select run) | select i",
            "let runs = where type == \"run\" | select run, app; where type "
            "== \"ev\" | lookup runs on run | derive k = concat(string(i), "
            "\"-\", app ?? \"none\") | select k",
            "where type == \"ev\" | derive s = dur - (from data | where type "
            "== \"ev\" | agg { m = min(dur) }) | derive k = concat(string(i), "
            "\"-\", string(s)) | select k",
        };
        dstd::Vector<dstd::Vector<dstd::String>> want;
        for (const std::size_t checkpoint :
             {std::size_t{2048}, std::size_t{65536}}) {
            CAPTURE(checkpoint);
            const dstd::String gz =
                write_records(env, "d" + dstd::to_string(checkpoint), lines,
                              4096, checkpoint);
            const dstd::String cache(dftracer::utils::index::lookups_dir(
                dstd::String(determine_index_path(gz, ""))));
            for (int pass = 0; pass < 3; ++pass) {
                CAPTURE(pass);
                if (pass == 2) dstd::fs::remove_all(cache).has_value();
                for (std::size_t k = 0; k < queries.size(); ++k) {
                    CAPTURE(queries[k]);
                    const auto f = frame(view_of(gz).duql(queries[k]));
                    const auto got = sorted(column(f, f.names.front()));
                    if (want.size() <= k)
                        want.push_back(got);
                    else
                        CHECK(got == want[k]);
                }
            }
        }
    }

    struct Rec {
        int i;
        int run_kind;  // 0 missing, 1 null, 2 string, 3 int
        int run;
        int name;
        bool has_dur;
        double dur;
    };

    static dstd::String run_key(const Rec& r) {
        if (r.run_kind == 2) return "s" + dstd::to_string(r.run);
        if (r.run_kind == 3) return "i" + dstd::to_string(r.run);
        return "";
    }

    // Each lookup layout (checkpoint 2048, member 4096) splits this many rows
    // into several chunks; a cap of 10 rows or 1 KiB is far below it.
    constexpr int CORRELATED_ROWS = 120;

    struct CorrelatedData {
        dstd::Vector<dstd::String> lines;
        dstd::Vector<Rec> recs;
    };

    static CorrelatedData correlated_data(int n, unsigned seed) {
        unsigned x = seed;
        auto next = [&](unsigned m) {
            x = x * 1664525u + 1013904223u;
            return static_cast<int>((x >> 8) % m);
        };
        CorrelatedData d;
        for (int i = 0; i < n; ++i) {
            Rec r{i,
                  next(10) < 3 ? next(3) : 3,
                  next(4),
                  next(4),
                  next(10) != 0,
                  next(100) + (next(2) ? 0.5 : 0.0)};
            d.recs.push_back(r);
            dstd::String l = R"({"i":)" + dstd::to_string(i) + R"(,"g":)" +
                             dstd::to_string(i % 5) + R"(,"name":")" +
                             dstd::String(1, static_cast<char>('a' + r.name)) +
                             "\"";
            if (r.run_kind == 1) l += R"(,"run":null)";
            if (r.run_kind == 2)
                l += R"(,"run":")" + dstd::to_string(r.run) + "\"";
            if (r.run_kind == 3) l += R"(,"run":)" + dstd::to_string(r.run);
            if (r.has_dur) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%g", r.dur);
                l += dstd::String(R"(,"dur":)") + buf;
            }
            d.lines.push_back(l + R"(,"pad":"xxxxxxxxxxxxxxxxxxxxxxxx"})");
        }
        return d;
    }

    // The queries of the correlated tests, over `where` as the outer filter,
    // with the rows they must give.
    struct CorrelatedCase {
        dstd::String query;
        dstd::String column;
        dstd::Vector<dstd::String> want;
    };

    static dstd::Vector<CorrelatedCase> correlated_cases(
        const CorrelatedData& d, const dstd::String& where,
        const dstd::String& empty_sum) {
        const auto& rs = d.recs;
        auto count = [](double v) {
            return dstd::to_string(static_cast<long long>(v));
        };
        dstd::Vector<CorrelatedCase> out;
        {
            dstd::Vector<double> sum(4, 0);
            dstd::Vector<int> cnt(4, 0);
            for (const Rec& r : rs)
                if (r.has_dur) {
                    sum[static_cast<std::size_t>(r.name)] += r.dur;
                    ++cnt[static_cast<std::size_t>(r.name)];
                }
            CorrelatedCase c{"where " + where +
                                 " | where dur > (from data | where name == "
                                 "^.name | agg { m = mean(dur) }) | select i",
                             "i",
                             {}};
            for (const Rec& r : rs) {
                const auto k = static_cast<std::size_t>(r.name);
                if (r.has_dur && cnt[k] && r.dur > sum[k] / cnt[k])
                    c.want.push_back(dstd::to_string(r.i));
            }
            out.push_back(std::move(c));
        }
        {
            CorrelatedCase c{"where " + where +
                                 " | derive n = (from data | where run == "
                                 "^.run | agg { c = count() }) | select i, n",
                             "n",
                             {}};
            for (const Rec& r : rs) {
                int n = 0;
                if (!run_key(r).empty())
                    for (const Rec& o : rs) n += run_key(o) == run_key(r);
                c.want.push_back(dstd::to_string(n));
            }
            out.push_back(std::move(c));
        }
        for (const bool negated : {false, true}) {
            CorrelatedCase c{
                "where " + where + " | where g " +
                    dstd::String(negated ? "not " : "") +
                    "in (from data | where run == ^.run and dur > 50 | "
                    "select g) | select i",
                "i",
                {}};
            for (const Rec& r : rs) {
                bool hit = false;
                if (!run_key(r).empty())
                    for (const Rec& o : rs)
                        hit = hit || (run_key(o) == run_key(r) && o.has_dur &&
                                      o.dur > 50 && o.i % 5 == r.i % 5);
                if (hit != negated) c.want.push_back(dstd::to_string(r.i));
            }
            out.push_back(std::move(c));
        }
        {
            CorrelatedCase c{
                "where " + where +
                    " | derive t = (from data | where name == ^.name and run "
                    "== ^.run | agg { t = sum(dur) }) | select i, t",
                "t",
                {}};
            for (const Rec& r : rs) {
                double t = 0;
                bool any = false;
                bool with_dur = false;
                if (!run_key(r).empty())
                    for (const Rec& o : rs)
                        if (o.name == r.name && run_key(o) == run_key(r)) {
                            any = true;
                            if (o.has_dur) {
                                t += o.dur;
                                with_dur = true;
                            }
                        }
                // A sum over rows with no dur is null; over no rows it is
                // `empty_sum`.
                c.want.push_back(any ? "null" : empty_sum);
                if (with_dur) {
                    char buf[32];
                    std::snprintf(buf, sizeof buf, "%g", t);
                    c.want.back() = buf;
                }
            }
            out.push_back(std::move(c));
        }
        {
            CorrelatedCase c{"where " + where +
                                 " | derive x = (from data | where i == ^.i | "
                                 "select dur) | select i, x",
                             "x",
                             {}};
            for (const Rec& r : rs) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%g", r.dur);
                c.want.push_back(r.has_dur ? buf : "null");
            }
            out.push_back(std::move(c));
        }
        {
            CorrelatedCase c{"where " + where +
                                 " | derive c = (from data | where dur * 2 == "
                                 "^.dur * 2 | agg { c = count() }) | select "
                                 "i, c",
                             "c",
                             {}};
            for (const Rec& r : rs) {
                int n = 0;
                if (r.has_dur)
                    for (const Rec& o : rs) n += o.has_dur && o.dur == r.dur;
                c.want.push_back(count(n));
            }
            out.push_back(std::move(c));
        }
        return out;
    }

    // Column `name` of `f` in the order of column `i`; `i` itself sorted.
    static dstd::Vector<dstd::String> by_i(const dataframe::DataFrame& f,
                                           const dstd::String& name) {
        dstd::Vector<dstd::Pair<long, dstd::String>> rows;
        const auto ids = column(f, "i");
        const auto vals = column(f, name);
        for (std::size_t r = 0; r < ids.size(); ++r)
            rows.emplace_back(dftu_utils_test::to_number<long>(ids[r]),
                              vals[r]);
        std::sort(rows.begin(), rows.end());
        dstd::Vector<dstd::String> out;
        for (auto& [id, v] : rows) out.push_back(std::move(v));
        return out;
    }

    static dstd::String as_number_text(const dstd::String& s) {
        if (s == "null" || s.empty()) return s;
        char* end = nullptr;
        const double v = std::strtod(s.c_str(), &end);
        char buf[32];
        std::snprintf(buf, sizeof buf, "%g", v);
        return buf;
    }

    // Checks every query of `cases` against its per-row reference.
    static void check_cases(const View& v,
                            const dstd::Vector<CorrelatedCase>& cases) {
        for (const auto& c : cases) {
            CAPTURE(c.query);
            const auto f = frame(v.duql(c.query));
            dstd::Vector<dstd::String> got;
            for (const auto& x :
                 c.column == "i" ? sorted(column(f, "i")) : by_i(f, c.column))
                got.push_back(as_number_text(x));
            dstd::Vector<dstd::String> want;
            for (const auto& x : c.want) want.push_back(as_number_text(x));
            if (c.column == "i") want = sorted(want);
            CHECK(first_diff(got, want) == "");
        }
    }

    TEST_CASE(
        "a correlated sub-query equals the per-row reference on every layout "
        "and pushdown") {
        TestEnvironment env(10);
        const CorrelatedData d = correlated_data(CORRELATED_ROWS, 21);
        dstd::String empty_sum;
        for (const std::size_t checkpoint :
             {std::size_t{2048}, std::size_t{65536}}) {
            CAPTURE(checkpoint);
            const View v =
                view_of(write_records(env, "cl" + dstd::to_string(checkpoint),
                                      d.lines, 4096, checkpoint));
            if (empty_sum.empty()) {
                const auto none = frame(
                    v.duql("from data | where false | agg { t = sum(dur) }"));
                REQUIRE(none.num_rows() == 1);
                empty_sum = as_number_text(cell(none, 0, "t"));
            }
            for (const char* where : {"i >= 0", "(i >= 0) or false"}) {
                CAPTURE(where);
                check_cases(v, correlated_cases(d, where, empty_sum));
            }
        }
    }

    static dstd::String num_text(double v) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%g", v);
        return buf;
    }

    // Range sub-queries over `where` with the per-row nested-loop answer.
    static dstd::Vector<CorrelatedCase> range_cases(const CorrelatedData& d,
                                                    const dstd::String& where) {
        const auto& rs = d.recs;
        dstd::Vector<CorrelatedCase> out;
        auto add = [&](const dstd::String& body, const dstd::String& column,
                       auto&& per_row) {
            CorrelatedCase c{"where " + where + " | " + body, column, {}};
            for (const Rec& r : rs) c.want.push_back(per_row(r));
            out.push_back(std::move(c));
        };
        // Aggregates of `dur` over the rows `in`: null over none with dur.
        auto agg = [&](const Rec& r, auto&& in, const char* fn) {
            double best = 0;
            double sum = 0;
            int n = 0;
            for (const Rec& o : rs) {
                if (!in(o, r) || !o.has_dur) continue;
                if (n == 0 || (fn[1] == 'a' ? o.dur > best : o.dur < best))
                    best = o.dur;
                sum += o.dur;
                ++n;
            }
            if (n == 0) return dstd::String("null");
            if (fn[0] == 's') return num_text(sum);
            if (fn[0] == 'a') return num_text(sum / n);
            return num_text(best);
        };
        auto name = [](const Rec& r) {
            return static_cast<char>('a' + r.name);
        };
        add("derive m = (from data | where i < ^.i | agg { m = max(dur) }) | "
            "select i, m",
            "m", [&](const Rec& r) {
                return agg(
                    r, [](const Rec& o, const Rec& x) { return o.i < x.i; },
                    "max");
            });
        add("derive m = (from data | where g == ^.g and i < ^.i | agg { m = "
            "min(dur) }) | select i, m",
            "m", [&](const Rec& r) {
                return agg(
                    r,
                    [](const Rec& o, const Rec& x) {
                        return o.i % 5 == x.i % 5 && o.i < x.i;
                    },
                    "min");
            });
        add("derive s = (from data | where name == ^.name and i >= ^.i | agg "
            "{ s = sum(dur) }) | select i, s",
            "s", [&](const Rec& r) {
                return agg(
                    r,
                    [](const Rec& o, const Rec& x) {
                        return o.name == x.name && o.i >= x.i;
                    },
                    "sum");
            });
        add("derive a = (from data | where i > ^.i and i <= ^.i + 5 | agg { "
            "a = mean(dur) }) | select i, a",
            "a", [&](const Rec& r) {
                return agg(
                    r,
                    [](const Rec& o, const Rec& x) {
                        return o.i > x.i && o.i <= x.i + 5;
                    },
                    "avg");
            });
        add("derive c = (from data | where run == ^.run and dur between "
            "^.dur - 10 and ^.dur + 10 | agg { c = count() }) | select i, c",
            "c", [&](const Rec& r) {
                int n = 0;
                if (!run_key(r).empty() && r.has_dur)
                    for (const Rec& o : rs)
                        n += run_key(o) == run_key(r) && o.has_dur &&
                             o.dur >= r.dur - 10 && o.dur <= r.dur + 10;
                return dstd::to_string(n);
            });
        add("derive n = (from data | where dur < ^.dur | agg { n = count(dur) "
            "}) | select i, n",
            "n", [&](const Rec& r) {
                int n = 0;
                if (r.has_dur)
                    for (const Rec& o : rs) n += o.has_dur && o.dur < r.dur;
                return dstd::to_string(n);
            });
        add("derive k = (from data | where name < ^.name and g == ^.g | agg { "
            "k = count_if(dur > 50) }) | select i, k",
            "k", [&](const Rec& r) {
                int n = 0;
                for (const Rec& o : rs)
                    n += name(o) < name(r) && o.i % 5 == r.i % 5 && o.has_dur &&
                         o.dur > 50;
                return dstd::to_string(n);
            });
        add("derive c = (from data | where run <= ^.run | agg { c = count() "
            "}) | select i, c",
            "c", [&](const Rec& r) {
                int n = 0;
                if (r.run_kind >= 2)
                    for (const Rec& o : rs)
                        n += o.run_kind == r.run_kind && o.run <= r.run;
                return dstd::to_string(n);
            });
        add("derive x = (from data | where i >= ^.i and i < ^.i + 1 | select "
            "dur) | select i, x",
            "x", [&](const Rec& r) {
                return r.has_dur ? num_text(r.dur) : dstd::String("null");
            });
        for (const char* tail : {"select g", "group g {}"})
            for (const bool negated : {false, true}) {
                CorrelatedCase c{
                    "where " + where + " | where g " + (negated ? "not " : "") +
                        "in (from data | where name == \"a\" and i < ^.i | " +
                        tail + ") | select i",
                    "i",
                    {}};
                for (const Rec& r : rs) {
                    bool hit = false;
                    for (const Rec& o : rs)
                        hit = hit ||
                              (o.name == 0 && o.i < r.i && o.i % 5 == r.i % 5);
                    if (hit != negated) c.want.push_back(dstd::to_string(r.i));
                }
                out.push_back(std::move(c));
            }
        return out;
    }

    TEST_CASE(
        "a range sub-query equals the per-row reference on every layout and "
        "pushdown") {
        TestEnvironment env(10);
        const CorrelatedData d = correlated_data(CORRELATED_ROWS, 29);
        for (const std::size_t checkpoint :
             {std::size_t{2048}, std::size_t{65536}}) {
            CAPTURE(checkpoint);
            const View v =
                view_of(write_records(env, "rl" + dstd::to_string(checkpoint),
                                      d.lines, 4096, checkpoint));
            for (const char* where : {"i >= 0", "(i >= 0) or false"}) {
                CAPTURE(where);
                check_cases(v, range_cases(d, where));
            }
        }
    }

    TEST_CASE("a correlated sub-query refuses what it cannot key") {
        TestEnvironment env(10);
        const CorrelatedData d = correlated_data(60, 3);
        const View v = view_of(write_records(env, "ce", d.lines, 4096, 4096));
        CHECK(has(error_of_frame([&] {
                      return v.duql(
                          "derive x = (from data | where run == ^.run | "
                          "select dur)");
                  }),
                  {"__sub_", "rows for key", "one row"}));
        CHECK(has(error_of([&] {
                      return v.duql(
                          "where dur > (from data | where run == ^.run and "
                          "dur + ^.dur > 1 | agg { m = max(dur) })");
                  }),
                  {"dur + ^.dur > 1", "mixes"}));
        CHECK(has(error_of([&] {
                      return v.duql(
                          "where dur > (from data | where i < ^.i and dur < "
                          "^.dur | agg { m = max(dur) })");
                  }),
                  {"dur < ^.dur", "overlap"}));
        CHECK(has(error_of([&] {
                      return v.duql(
                          "where dur > (from data | where i < ^.i | agg { m "
                          "= first(dur) })");
                  }),
                  {"'first'", "count, count_if"}));
        CHECK(has(error_of_frame([&] {
                      return v.duql(
                          "derive x = (from data | where i < ^.i | "
                          "select dur)");
                  }),
                  {"__sub_", "rows in the range", "one row"}));
        CHECK(has(dftu_utils_test::ok(v.explain_duql(
                      "derive m = (from data | where run == ^.run "
                      "and i between ^.i - 5 and ^.i | agg { m = "
                      "max(dur) })")),
                  {"__sub_0 on run, range >= (i - 5), range <= i"}));
        CHECK(has(error_of([&] {
                      return v.duql(
                          "where dur > (from data | where run == ^.run | "
                          "take 2 | agg { m = max(dur) })");
                  }),
                  {"take", "correlated"}));
        CHECK(has(error_of([&] {
                      return v.duql(
                          "where dur > (from data | where run == ^.run | "
                          "derive z = ^.dur | agg { m = max(dur) })");
                  }),
                  {"^.dur", "other than 'where'"}));
        CHECK(has(error_of([&] {
                      return v.duql(
                          "where dur > (from data | where run == ^.run)");
                  }),
                  {"select", "group", "agg"}));
        CHECK(has(error_of([&] {
                      return v.duql(
                          "where dur > (from data | where run == ^.run | "
                          "group name { m = max(dur) })");
                  }),
                  {"not 'group'"}));
        CHECK(has(error_of([&] {
                      return v.duql(
                          "where dur > (from data | where run == ^.run | "
                          "agg { m = max(dur), n = count() })");
                  }),
                  {"one column", "2"}));
        CHECK(has(dftu_utils_test::ok(
                      v.explain_duql("where dur > (from data | where name == "
                                     "^.name | agg { m = mean(dur) })")),
                  {"__sub_0", "__ck0"}));
    }

    // A top-level `in` reads its side's distinct rows under the memory
    // budget; the other sub-queries join theirs, which spills.
    TEST_CASE("sub-queries over the caps and the memory budget") {
        TestEnvironment env(10);
        const CorrelatedData d = correlated_data(CORRELATED_ROWS, 13);
        ScopedEnv rows("DUQL_LOOKUP_MAX_ROWS", "10");
        ScopedEnv bytes("DUQL_LOOKUP_MAX_BYTES", "1024");
        // Layout and pushdown are covered on their own above; these four runs
        // pair every value of checkpoint, budget and filter with every other.
        struct Config {
            std::size_t checkpoint;
            std::uint64_t budget;
            const char* where;
        };
        for (const Config& cfg : {Config{2048, 1024, "i >= 0"},
                                  Config{2048, 0, "(i >= 0) or false"},
                                  Config{65536, 1024, "(i >= 0) or false"},
                                  Config{65536, 0, "i >= 0"}}) {
            const dstd::String gz =
                write_records(env, "cs" + dstd::to_string(cfg.checkpoint),
                              d.lines, 4096, cfg.checkpoint);
            const View v = cfg.budget ? view_of(gz).memory_budget(cfg.budget)
                                      : view_of(gz);
            CAPTURE(cfg.checkpoint);
            CAPTURE(cfg.budget);
            CAPTURE(cfg.where);
            auto cases = correlated_cases(d, cfg.where, "null");
            {
                CorrelatedCase c{
                    dstd::String("where ") + cfg.where +
                        " | where (g in (from data | where dur > 50 | "
                        "select g)) or i < 0 | select i",
                    "i",
                    {}};
                for (const Rec& r : d.recs) {
                    bool hit = false;
                    for (const Rec& o : d.recs)
                        hit = hit ||
                              (o.has_dur && o.dur > 50 && o.i % 5 == r.i % 5);
                    if (hit) c.want.push_back(dstd::to_string(r.i));
                }
                cases.push_back(std::move(c));
            }
            for (const auto& c : cases) {
                CAPTURE(c.query);
                const bool key_set =
                    c.query.find("| where g in") != dstd::String::npos;
                if (key_set && cfg.budget) {
                    CHECK(has(error_of_frame([&] { return v.duql(c.query); }),
                              {"memory budget", "__sub_"}));
                    continue;
                }
                const auto f = frame(v.duql(c.query));
                dstd::Vector<dstd::String> got;
                for (const auto& x : by_i(f, c.column))
                    got.push_back(as_number_text(x));
                dstd::Vector<dstd::String> want;
                for (const auto& x : c.want) want.push_back(as_number_text(x));
                CHECK(first_diff(got, want) == "");
            }
        }
    }

    struct SpillData {
        dstd::Vector<dstd::String> lines;
        dstd::Vector<dstd::String> plain;
        dstd::Vector<dstd::String> sizes;
    };

    // Events with a key that may be missing, null or a double, and a row set
    // of keys 0..SPILL_KEYS-1 (a fifth of them written as doubles) with a value
    // each, plus a second copy of some keys for `into`.
    constexpr int SPILL_KEYS = 60;
    constexpr int SPILL_EVENTS = 600;

    static SpillData spill_data(int events, unsigned seed) {
        unsigned x = seed;
        auto next = [&](unsigned m) {
            x = x * 1664525u + 1013904223u;
            return static_cast<int>((x >> 8) % m);
        };
        SpillData d;
        dstd::Vector<int> copies(SPILL_KEYS, 1);
        for (int j = 0; j < SPILL_KEYS; ++j) {
            const bool dup = next(4) == 0;
            for (int c = 0; c < 1 + dup; ++c) {
                const dstd::String k =
                    dstd::to_string(j) + (j % 5 == 0 ? ".0" : "");
                d.lines.push_back(R"({"type":"s","k":)" + k + R"(,"sv":)" +
                                  dstd::to_string(j * 3) +
                                  R"(,"pad":"xxxxxxxxxxxxxxxx"})");
            }
            copies[static_cast<std::size_t>(j)] = 1 + dup;
        }
        for (int i = 0; i < events; ++i) {
            const int kind = next(10);
            const int k = next(SPILL_KEYS * 4 / 3);
            dstd::String l = R"({"type":"ev","i":)" + dstd::to_string(i);
            if (kind == 0)
                l += R"(,"k":null)";
            else if (kind != 1)
                l += R"(,"k":)" + dstd::to_string(k) + (k % 2 == 0 ? ".0" : "");
            d.lines.push_back(l + R"(,"pad":"xxxxxxxxxxxxxxxx"})");
            const bool keyed = kind >= 2;
            d.plain.push_back(keyed && k < SPILL_KEYS ? dstd::to_string(k * 3)
                                                      : "null");
            d.sizes.push_back(
                keyed && k < SPILL_KEYS
                    ? dstd::to_string(copies[static_cast<std::size_t>(k)])
                    : "0");
        }
        return d;
    }

    TEST_CASE("a lookup joins a side over the caps and the memory budget") {
        TestEnvironment env(10);
        const SpillData d = spill_data(SPILL_EVENTS, 5);
        dstd::Vector<dstd::String> base;
        for (const std::size_t checkpoint :
             {std::size_t{2048}, std::size_t{65536}}) {
            const View v =
                view_of(write_records(env, "sp" + dstd::to_string(checkpoint),
                                      d.lines, 4096, checkpoint));
            ScopedEnv rows("DUQL_LOOKUP_MAX_ROWS", "10");
            ScopedEnv bytes("DUQL_LOOKUP_MAX_BYTES", "1024");
            for (const std::uint64_t budget :
                 {std::uint64_t{1024}, std::uint64_t{0}}) {
                CAPTURE(checkpoint);
                CAPTURE(budget);
                const View b = budget ? v.memory_budget(budget) : v;
                const dstd::String let =
                    "let s = where type == \"s\" | select k, sv; ";
                const auto f = frame(b.duql(
                    let +
                    "where type == \"ev\" | lookup s on k | select i, sv"));
                REQUIRE(f.num_rows() == SPILL_EVENTS);
                dstd::Vector<dstd::String> sv(SPILL_EVENTS);
                const auto ids = column(f, "i");
                const auto vals = column(f, "sv");
                for (std::size_t r = 0; r < ids.size(); ++r)
                    sv[static_cast<std::size_t>(
                        dftu_utils_test::to_number<long>(ids[r]))] = vals[r];
                const auto n =
                    frame(b.duql(
                        let + "where type == \"ev\" | lookup s on k into m | "
                              "select i, len(m) as n"));
                dstd::Vector<dstd::String> sizes(SPILL_EVENTS);
                const auto nid = column(n, "i");
                const auto nn = column(n, "n");
                for (std::size_t r = 0; r < nid.size(); ++r)
                    sizes[static_cast<std::size_t>(
                        dftu_utils_test::to_number<long>(nid[r]))] = nn[r];
                CHECK(sizes == d.sizes);
                if (base.empty()) base = sizes;
                CHECK(sizes == base);
                CHECK(sv == d.plain);
            }
        }
    }
}
