#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/index/indexer.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "test_view_common.h"

namespace df = dftracer::utils::dataframe;

namespace {

struct NullSink : ExportSink {
    void write(std::string_view) override {}
};

// Sets an environment variable for one scope.
struct Env {
    std::string name;
    Env(std::string n, const std::string& v) : name(std::move(n)) {
        setenv(name.c_str(), v.c_str(), 1);
    }
    ~Env() { unsetenv(name.c_str()); }
};

// `lines` as an indexed NDJSON trace; `member_bytes` > 0 frames it into
// gzip members of about that size, so the index holds several chunks.
std::string write_records(TestEnvironment& env, const std::string& name,
                          const std::vector<std::string>& lines,
                          std::size_t member_bytes = 0,
                          std::size_t checkpoint_size = 0,
                          std::vector<std::string> extensions = {}) {
    const std::string plain = env.get_dir() + "/" + name + ".ndjson";
    {
        std::ofstream o(plain);
        for (const auto& l : lines) o << l << "\n";
    }
    const std::string gz = plain + ".gz";
    if (member_bytes)
        REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(
            plain, gz, member_bytes));
    else
        dftu_utils_test::compress_file_to_gzip(plain, gz);
    fs::remove(plain);
    dftracer::utils::index::IndexerOptions o;
    if (checkpoint_size) o.checkpoint_size = checkpoint_size;
    if (!extensions.empty()) o.extensions = std::move(extensions);
    dftracer::utils::index::Indexer::open({gz}, o).build();
    return gz;
}

View view_of(const std::string& gz) {
    return View::from_file(gz, determine_index_path(gz, ""));
}

df::DataFrame frame(const View& v) {
    df::DataFrame f = run(v.lazy().collect());
    for (auto& c : f.columns)
        if (!c.is_flat()) c = c.materialize();
    return f;
}

// Row `r` of column `name` as text: `null`, a string, or a number.
std::string cell(const df::DataFrame& f, std::int64_t r,
                 std::string_view name) {
    const auto i = bcol(f, name);
    REQUIRE(i >= 0);
    const auto& c = f.columns[static_cast<std::size_t>(i)];
    if (c.is_null(r)) return "null";
    if (c.type() == df::TypeId::String) return bstr(f, r, name);
    const double d = bnum(f, r, name);
    if (d == static_cast<double>(static_cast<long long>(d)))
        return std::to_string(static_cast<long long>(d));
    return std::to_string(d);
}

std::vector<std::string> column(const df::DataFrame& f, std::string_view name) {
    std::vector<std::string> out;
    for (std::int64_t r = 0; r < f.num_rows(); ++r)
        out.push_back(cell(f, r, name));
    return out;
}

std::vector<std::string> sorted(std::vector<std::string> v) {
    std::sort(v.begin(), v.end());
    return v;
}

std::string error_of(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const std::exception& e) {
        return e.what();
    }
    return {};
}

bool has(const std::string& text, std::initializer_list<const char*> parts) {
    for (const char* p : parts)
        if (text.find(p) == std::string::npos) {
            MESSAGE(text);
            return false;
        }
    return true;
}

// Runs 1 and 2, then events of runs 1, 2 and 3 in order.
std::vector<std::string> runs_and_events() {
    std::vector<std::string> out = {
        R"({"type":"run","run":1,"app":"laghos"})",
        R"({"type":"run","run":2,"app":"amg"})",
    };
    for (int i = 0; i < 9; ++i)
        out.push_back(R"({"type":"ev","i":)" + std::to_string(i) +
                      R"(,"run":)" + std::to_string(i % 3 + 1) + R"(,"dur":)" +
                      std::to_string(10 * (i + 1)) + "}");
    return out;
}

}  // namespace

TEST_SUITE("View duql joins") {
    TEST_CASE("a let reads data unless it says from all") {
        TestEnvironment env(10);
        const std::string pfw = env.get_dir() + "/meta.pfw";
        {
            std::ofstream o(pfw);
            for (int i = 0; i < 3; ++i)
                o << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)"
                  << 100 * (i + 1) << R"(,"dur":5,"args":{}})" << "\n";
            for (int i = 0; i < 2; ++i)
                o << R"({"ph":"M","name":"FH","pid":1,"tid":1,"args":{"name":"/f)"
                  << i << R"(","value":")" << i << R"("}})" << "\n";
        }
        const std::string gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        fs::remove(pfw);
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

    TEST_CASE("unknown names, self and forward references fail to compile") {
        TestEnvironment env(10);
        const View v = view_of(write_records(env, "r", runs_and_events()));
        CHECK(has(error_of([&] { (void)v.duql("from runs | take 1"); }),
                  {"runs", "not a let or a row set"}));
        CHECK(has(error_of([&] { (void)v.duql("let a = from a; from a"); }),
                  {"'a'", "itself"}));
        CHECK(has(error_of([&] {
                      (void)v.duql(
                          "let a = from b; let b = where x > 1; from a");
                  }),
                  {"'b'", "declared after"}));
        CHECK(has(error_of([&] { (void)v.duql("where run -> nope.app == 1"); }),
                  {"nope", "not a let or a row set"}));
        CHECK(has(
            error_of([&] { (void)v.duql("def f(a) = a; where f(1, 2) > 0"); }),
            {"'f'", "argument"}));
        CHECK(has(error_of([&] {
                      (void)v.duql(
                          "where dur > (from data | where name == ^.name | agg "
                          "{ m = mean(dur) })");
                  }),
                  {"^.", "->"}));
    }

    TEST_CASE("a semi-join equals the two-step query") {
        TestEnvironment env(10);
        std::vector<std::string> lines;
        for (int i = 0; i < 200; ++i)
            lines.push_back(R"({"i":)" + std::to_string(i) + R"(,"name":"n)" +
                            std::to_string(i % 17) + R"(","dur":)" +
                            std::to_string((i * 37) % 101) + "}");
        const View v = view_of(write_records(env, "s", lines, 1024));
        const auto got = frame(
            v.duql("where name in (from data | where dur > 90 | select name)"));
        const auto names = frame(v.duql("where dur > 90 | select name"));
        std::set<std::string> keys;
        for (std::int64_t r = 0; r < names.num_rows(); ++r)
            keys.insert(bstr(names, r, "name"));
        REQUIRE(!keys.empty());
        std::string list;
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
        CHECK(column(f, "k") == std::vector<std::string>{"2"});
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
                      (void)v.duql(
                          "where run in (from data | select run, app)");
                  }),
                  {"2 columns", "compares 1"}));
        const auto t = frame(v.duql(
            "where type == \"ev\" and (run, i) in (from data | where i < 3 | "
            "select run, i)"));
        CHECK(column(t, "i") == std::vector<std::string>{"0", "1", "2"});
    }

    TEST_CASE("an arrow in a filter, under not, and with several matches") {
        TestEnvironment env(10);
        const View v = view_of(write_records(env, "a", runs_and_events()));
        const std::string let = "let runs = where type == \"run\"; ";
        const auto f = frame(v.duql(
            let + "where type == \"ev\" and run -> runs.app == \"laghos\""));
        CHECK(column(f, "i") == std::vector<std::string>{"0", "3", "6"});

        // Joined by hand: run 1 is laghos, run 2 amg, run 3 has no row.
        const auto n = frame(v.duql(
            let +
            "where type == \"ev\" and not (run -> runs.app == \"laghos\")"));
        CHECK(column(n, "i") == std::vector<std::string>{"1", "4", "7"});
        const auto o = frame(v.duql(
            let +
            "where type == \"ev\" and (run -> runs.app == \"amg\" or i == 2)"));
        CHECK(column(o, "i") == std::vector<std::string>{"1", "2", "4", "7"});

        const auto d =
            frame(v.duql(
                let + "where type == \"ev\" | derive app = run -> runs.app | "
                      "select i, app"));
        CHECK(column(d, "app") ==
              std::vector<std::string>{"laghos", "amg", "null", "laghos", "amg",
                                       "null", "laghos", "amg", "null"});

        auto lines = runs_and_events();
        lines.push_back(R"({"type":"run","run":1,"app":"amg"})");
        const View dup = view_of(write_records(env, "dup", lines));
        CHECK(has(error_of([&] {
                      (void)frame(
                          dup.duql(let + "derive app = run -> runs.app"));
                  }),
                  {"'runs'", "key 1"}));
        const auto any = frame(dup.duql(
            let + "where type == \"ev\" and run -> runs.app == \"amg\""));
        CHECK(column(any, "i") ==
              std::vector<std::string>{"0", "1", "3", "4", "6", "7"});
    }

    TEST_CASE("arrow forms: a named key, a tuple and a chain") {
        TestEnvironment env(10);
        const View v = view_of(
            write_records(env, "f",
                          {R"({"type":"run","rid":1,"sys":"x","app":"laghos"})",
                           R"({"type":"sys","sys":"x","cpu":"epyc"})",
                           R"({"type":"ev","run":1,"i":0})",
                           R"({"type":"ev","run":2,"i":1})"}));
        const std::string lets =
            "let runs = where type == \"run\"; let systems = where type == "
            "\"sys\"; ";
        const auto a = frame(
            v.duql(lets +
                   "where type == \"ev\" | derive app = run -> runs(rid).app | "
                   "select i, app"));
        CHECK(column(a, "app") == std::vector<std::string>{"laghos", "null"});
        const auto c = frame(v.duql(
            lets +
            "where type == \"ev\" | derive cpu = run -> runs(rid).sys -> "
            "systems.cpu | select i, cpu"));
        CHECK(column(c, "cpu") == std::vector<std::string>{"epyc", "null"});
        const auto t = frame(v.duql(
            lets +
            "where type == \"run\" | derive app = (rid, sys) -> runs.app "
            "| select app"));
        CHECK(column(t, "app") == std::vector<std::string>{"laghos"});
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
        CHECK(has(error_of([&] {
                      (void)frame(v.duql(
                          "derive x = (from data | where type == \"ev\" | "
                          "select dur)"));
                  }),
                  {"__sub_0", "one row"}));
        const auto none = frame(v.duql(
            "where type == \"run\" | derive x = (from data | where type == "
            "\"none\" | agg { m = max(dur) }) | select x"));
        CHECK(column(none, "x") == std::vector<std::string>{"null", "null"});
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
        CHECK(column(f, "i") == std::vector<std::string>{"0", "1", "2", "3",
                                                         "4", "5", "6", "7",
                                                         "8"});
        CHECK(column(f, "app") ==
              std::vector<std::string>{"laghos", "amg", "null", "laghos", "amg",
                                       "null", "laghos", "amg", "null"});

        const auto e = frame(v.duql(
            "let hosts = where type == \"host\" | select run, host; where type "
            "== \"ev\" and run == 1 | lookup hosts on run into hs | expand hs "
            "| select i, hs.host"));
        CHECK(column(e, "i") ==
              std::vector<std::string>{"0", "0", "3", "3", "6", "6"});
        CHECK(column(e, "hs.host") ==
              std::vector<std::string>{"a", "b", "a", "b", "a", "b"});
        const auto empty = frame(v.duql(
            "let hosts = where type == \"host\" | select run, host; where type "
            "== \"ev\" and run == 3 | lookup hosts on run into hs | derive n = "
            "len(hs) | select n"));
        CHECK(column(empty, "n") == std::vector<std::string>{"0", "0", "0"});

        CHECK(has(error_of([&] {
                      (void)frame(v.duql(
                          "let hosts = where type == \"host\" | select run, "
                          "host; where type == \"ev\" | lookup hosts on run"));
                  }),
                  {"hosts", "key 1"}));
        CHECK(has(error_of([&] {
                      (void)frame(v.duql(
                          "let runs = where type == \"run\" | select run, "
                          "type; where type == \"ev\" | lookup runs on run"));
                  }),
                  {"'type'", "already"}));
        CHECK(has(error_of([&] {
                      (void)v.duql(
                          "let runs = where type == \"run\" | select run, "
                          "k = 1; where type == \"ev\" | derive k = 2 | "
                          "lookup runs on run");
                  }),
                  {"'k'", "already"}));
        const auto k2 = frame(v.duql(
            "let runs = where type == \"run\" | select rid = run, app; where "
            "type == \"ev\" and i < 2 | lookup runs on run == rid | select "
            "app"));
        CHECK(column(k2, "app") == std::vector<std::string>{"laghos", "amg"});
    }

    // Samples `{"type":"s","k":..,"t":..,"v":..}` and events
    // `{"type":"ev","i":..,"k":..,"ts":..}` with equal times, missing keys
    // and missing times; `expect` is the nested-loop answer per event.
    static std::string text_of(const std::vector<std::string>& v) {
        std::string out;
        for (const auto& x : v) out += x + ",";
        return out;
    }

    static std::string first_diff(const std::vector<std::string>& got,
                                  const std::vector<std::string>& want) {
        for (std::size_t r = 0; r < std::min(got.size(), want.size()); ++r)
            if (got[r] != want[r])
                return "row " + std::to_string(r) + ": got " + got[r] +
                       ", want " + want[r];
        return got.size() == want.size() ? "" : "sizes differ";
    }

    struct AsofData {
        std::vector<std::string> lines;
        std::vector<std::vector<std::string>> expect;
    };

    static AsofData asof_data(int events, int samples, unsigned seed) {
        struct Sample {
            int k, t, v;
            bool has_k, has_t;
        };
        std::vector<Sample> smp;
        std::vector<std::string> lines;
        unsigned x = seed;
        auto next = [&](unsigned m) {
            x = x * 1664525u + 1013904223u;
            return (x >> 8) % m;
        };
        for (int j = 0; j < samples; ++j) {
            Sample s{static_cast<int>(next(3)), static_cast<int>(next(120)), j,
                     next(10) != 0, next(10) != 0};
            smp.push_back(s);
            std::string l = R"({"type":"s","v":)" + std::to_string(j);
            if (s.has_k) l += R"(,"k":)" + std::to_string(s.k);
            if (s.has_t) l += R"(,"t":)" + std::to_string(s.t);
            lines.push_back(l + R"(,"pad":"xxxxxxxxxxxxxxxxxxxxxxxx"})");
        }
        AsofData d;
        struct Ev {
            int i, k, ts;
            bool has_k, has_ts;
        };
        std::vector<Ev> evs;
        for (int i = 0; i < events; ++i) {
            Ev e{i, static_cast<int>(next(3)), static_cast<int>(next(120)),
                 next(10) != 0, next(10) != 0};
            evs.push_back(e);
            std::string l = R"({"type":"ev","i":)" + std::to_string(i);
            if (e.has_k) l += R"(,"k":)" + std::to_string(e.k);
            if (e.has_ts) l += R"(,"ts":)" + std::to_string(e.ts);
            lines.push_back(l + R"(,"pad":"xxxxxxxxxxxxxxxxxxxxxxxx"})");
        }
        d.lines = std::move(lines);
        // backward, forward, nearest, each without and with `within 4`.
        for (int mode = 0; mode < 6; ++mode) {
            std::vector<std::string> col;
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
                col.push_back(pick < 0 ? "null" : std::to_string(pick));
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
                std::string("let s = where type == \"s\" | select k, t, v; "
                            "where type == \"ev\" | lookup s on k asof ts == "
                            "t ") +
                clauses[m] + " | select i, v"));
            REQUIRE(f.num_rows() == 300);
            CHECK(first_diff(column(f, "v"), d.expect[m]) == "");
            std::vector<std::string> order;
            for (int i = 0; i < 300; ++i) order.push_back(std::to_string(i));
            CHECK(text_of(column(f, "i")) == text_of(order));
        }
    }

    TEST_CASE("an as-of lookup does not depend on pushdown or layout") {
        TestEnvironment env(10);
        const AsofData d = asof_data(300, 80, 5);
        std::vector<std::string> want;
        for (const std::size_t checkpoint :
             {std::size_t{2048}, std::size_t{65536}}) {
            const std::string gz =
                write_records(env, "b" + std::to_string(checkpoint), d.lines,
                              4096, checkpoint);
            for (const char* where :
                 {"type == \"ev\"", "(type == \"ev\") or false"}) {
                CAPTURE(where);
                const auto f = frame(view_of(gz).duql(
                    std::string("let s = where type == \"s\" | select k, t, "
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
        const std::string plan = v.explain_duql(
            "let runs = where type == \"run\" | select run, when = run, app; "
            "where type == \"ev\" | lookup runs on run asof i == when "
            "forward within 3");
        CHECK(has(plan, {"lookup runs on run == run asof i forward within 3",
                         "dftu.frame.asof"}));
        const auto filled = frame(v.duql(
            "let runs = where type == \"run\" | select run, when = run, app; "
            "where type == \"ev\" | lookup runs on run asof i == when | "
            "select i, app"));
        CHECK(column(filled, "app") ==
              std::vector<std::string>{"null", "null", "null", "laghos", "amg",
                                       "null", "laghos", "amg", "null"});
        CHECK(has(error_of([&] {
                      (void)v.duql(
                          "let runs = where type == \"run\" | select run, "
                          "app; where type == \"ev\" | lookup runs on run "
                          "asof i == app");
                  }),
                  {"asof", "app", "string"}));
        CHECK(has(error_of([&] {
                      (void)v.duql(
                          "let runs = where type == \"run\" | select run, "
                          "app; where type == \"ev\" | lookup runs on run "
                          "asof i == run into m");
                  }),
                  {"cannot be combined"}));
    }

    struct OverlapData {
        std::vector<std::string> lines;
        std::vector<std::string> pairs;
        std::vector<std::string> nested;
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
        std::vector<Span> ph;
        std::vector<Span> cl;
        OverlapData d;
        auto line = [&](const char* name, int id, const Span& s) {
            return std::string(R"({"ph":"X","name":")") + name +
                   R"(","cat":"c","pid":)" + std::to_string(s.pid) +
                   R"(,"tid":)" + std::to_string(id) + R"(,"ts":)" +
                   std::to_string(s.ts) + R"(,"dur":)" + std::to_string(s.dur) +
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
            std::string list;
            int found = 0;
            for (int j = 0; j < phases; ++j) {
                const Span& p = ph[static_cast<std::size_t>(j)];
                if (p.pid != c.pid || !(p.ts < c.ts + c.dur) ||
                    !(c.ts < p.ts + p.dur))
                    continue;
                d.pairs.push_back(std::to_string(1000 + i) + ":" +
                                  std::to_string(j));
                list += std::to_string(j) + ",";
                ++found;
            }
            if (!found) d.pairs.push_back(std::to_string(1000 + i) + ":null");
            d.nested.push_back(list);
        }
        return d;
    }

    TEST_CASE("an overlap lookup equals a nested loop") {
        TestEnvironment env(10);
        const OverlapData d = overlap_data(200, 60, 3);
        for (const std::size_t checkpoint :
             {std::size_t{2048}, std::size_t{65536}}) {
            const std::string gz =
                write_records(env, "o" + std::to_string(checkpoint), d.lines,
                              4096, checkpoint);
            for (const char* where :
                 {"name == \"call\"", "(name == \"call\") or false"}) {
                CAPTURE(checkpoint);
                CAPTURE(where);
                const auto f = frame(view_of(gz).duql(
                    std::string("let ph = where name == \"phase\" | select "
                                "pid, ts, dur, ptid = tid; where ") +
                    where + " | lookup ph on pid overlap | select tid, ptid"));
                std::vector<std::string> got;
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
        std::vector<std::string> counts;
        for (const auto& l : d.nested) {
            int n = 0;
            for (const char c : l) n += c == ',';
            counts.push_back(std::to_string(n));
        }
        CHECK(text_of(column(f, "n")) == text_of(counts));
    }

    TEST_CASE("overlap lookup errors and explain") {
        TestEnvironment env(10);
        const OverlapData d = overlap_data(20, 10, 1);
        const View v = view_of(write_records(env, "e2", d.lines));
        const std::string plan = v.explain_duql(
            "let ph = where name == \"phase\" | select pid, ts, dur; where "
            "name == \"call\" | lookup ph on pid overlap");
        CHECK(has(plan, {"lookup ph on pid == pid overlap", "sweep over ph"}));
        CHECK(has(error_of([&] {
                      (void)v.duql(
                          "let ph = where name == \"phase\" | select pid, "
                          "ts; where name == \"call\" | lookup ph on pid "
                          "overlap");
                  }),
                  {"overlap", "'dur'"}));
        const View plain = view_of(write_records(env, "e3", runs_and_events()));
        CHECK(has(error_of([&] {
                      (void)plain.duql(
                          "let runs = where type == \"run\" | select run; "
                          "where type == \"ev\" | lookup runs on run "
                          "overlap");
                  }),
                  {"overlap", "role"}));
    }

    TEST_CASE("union and from a, b") {
        TestEnvironment env(10);
        const std::string a = write_records(
            env, "ua", {R"({"x":1,"y":"p"})", R"({"x":2,"y":"q"})"});
        const std::string b =
            write_records(env, "ub", {R"({"x":2.5,"z":true})"});
        const auto n = frame(view_of(a).duql("union (from \"" + b +
                                             "\") | agg { n = count() }"));
        CHECK(bnum(n, 0, "n") == 3);
        const auto u = frame(view_of(a).duql("select x, y | union (from \"" +
                                             b + "\" | select x, z)"));
        CHECK(u.names == std::vector<std::string>{"x", "y", "z"});
        CHECK(column(u, "x") == std::vector<std::string>{"1", "2", "2.500000"});
        CHECK(column(u, "y") == std::vector<std::string>{"p", "q", "null"});
        const auto both =
            frame(view_of(a).duql("let one = where x == 1; let two = from \"" +
                                  b + "\"; from one, two | select x"));
        CHECK(column(both, "x") == std::vector<std::string>{"1", "2.500000"});
        CHECK(has(error_of([&] {
                      (void)view_of(a).duql("select y | union (from \"" + b +
                                            "\" | select y = z)");
                  }),
                  {"'y'", "union"}));
    }

    TEST_CASE("caps fail loudly, only when the query runs") {
        TestEnvironment env(10);
        std::vector<std::string> lines;
        for (int i = 0; i < 11; ++i)
            lines.push_back(R"({"k":)" + std::to_string(i) + "}");
        const View v = view_of(write_records(env, "cap", lines));
        const std::string q =
            "let ks = where k >= 0 | select k; where k in (from ks)";
        {
            Env cap("DUQL_LOOKUP_MAX_ROWS", "10");
            const View planned = v.duql(q);
            (void)v.explain_duql(q);
            CHECK(has(error_of([&] { (void)frame(planned); }),
                      {"'ks'", "11", "10", "DUQL_LOOKUP_MAX_ROWS"}));
        }
        {
            Env cap("DUQL_LOOKUP_MAX_BYTES", "16");
            CHECK(has(error_of([&] { (void)frame(v.duql(q)); }),
                      {"'ks'", "DUQL_LOOKUP_MAX_BYTES", "16"}));
        }
        CHECK(frame(v.duql(q)).num_rows() == 11);
    }

    TEST_CASE("key-set pushdown prunes chunks and keeps the rows") {
        TestEnvironment env(10);
        std::vector<std::string> lines;
        for (const char* name : {"a", "b", "c", "d"})
            for (int i = 0; i < 400; ++i)
                lines.push_back(R"({"name":")" + std::string(name) +
                                R"(","i":)" + std::to_string(i) +
                                R"(,"pad":"xxxxxxxxxxxxxxxxxxxxxxxx"})");
        const View v = view_of(write_records(env, "p", lines, 4096, 4096));
        const std::string sub =
            "name in (from data | where name == \"a\" | select name)";
        NullSink sink;
        const auto pushed = v.duql("where " + sub).sink_json(sink).get();
        const auto kept =
            v.duql("where (" + sub + ") or false").sink_json(sink).get();
        CHECK(pushed.events_matched == 400);
        CHECK(kept.events_matched == 400);
        CHECK(pushed.chunks_skipped > 0);
        CHECK(pushed.chunks_skipped > kept.chunks_skipped);
        CHECK(
            sorted(column(frame(v.duql("where " + sub)), "i")) ==
            sorted(column(frame(v.duql("where (" + sub + ") or false")), "i")));

        const std::string arrow =
            "let first = where i == 0 | select name, tag = name; where name -> "
            "first.tag == \"b\"";
        const auto a = v.duql(arrow).sink_json(sink).get();
        CHECK(a.events_matched == 400);
        CHECK(a.chunks_skipped > 0);
    }

    TEST_CASE("a key set prunes on min and max alone") {
        TestEnvironment env(10);
        std::vector<std::string> lines;
        for (int i = 0; i < 2000; ++i)
            lines.push_back(R"({"blk":)" + std::to_string(i / 100) +
                            R"(,"i":)" + std::to_string(i) +
                            R"(,"pad":"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"})");
        const View v =
            view_of(write_records(env, "z", lines, 4096, 4096, {"zonemap"}));
        NullSink sink;
        const auto listed = v.duql("where blk in [3, 4]").sink_json(sink).get();
        CHECK(listed.events_matched == 200);
        CHECK(listed.chunks_skipped > 0);
        const auto joined =
            v.duql(
                 "where blk in (from data | where i == 350 or i == 450 | "
                 "select blk)")
                .sink_json(sink)
                .get();
        CHECK(joined.events_matched == 200);
        CHECK(joined.chunks_skipped == listed.chunks_skipped);
    }

    TEST_CASE("a semi-join over more keys than the pushdown cap") {
        TestEnvironment env(10);
        std::vector<std::string> lines;
        for (int i = 0; i < 5000; ++i)
            lines.push_back(R"({"k":)" + std::to_string(i) + "}");
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
        const std::string plan = v.explain_duql(
            "let runs = where type == \"run\"; where run -> runs.app == "
            "\"laghos\"");
        CHECK(has(plan, {"side runs: from data; not cached",
                         "run in (keys of runs) (pushed)"}));
        const std::string not_pushed = v.explain_duql(
            "let runs = where type == \"run\"; where not (run -> runs.app == "
            "\"laghos\")");
        CHECK(not_pushed.find("keys of runs") == std::string::npos);
        const std::string stages = v.explain_duql(
            "let runs = where type == \"run\" | select run, app; lookup runs "
            "on run | union (from data)");
        CHECK(has(stages, {"lookup runs on run", "union from data"}));
    }

    TEST_CASE("lookup sides are cached and invalidated by a re-index") {
        TestEnvironment env(10);
        auto lines = runs_and_events();
        const std::string gz = write_records(env, "c", lines);
        const std::string q =
            "let runs = where type == \"run\"; where type == \"ev\" and run "
            "-> runs.app == \"laghos\"";
        CHECK(has(view_of(gz).explain_duql(q),
                  {"side runs: from data; not cached"}));
        const auto first = column(frame(view_of(gz).duql(q)), "i");
        CHECK(first == std::vector<std::string>{"0", "3", "6"});
        CHECK(has(view_of(gz).explain_duql(q),
                  {"side runs: from data; cached, 2 rows"}));
        CHECK(column(frame(view_of(gz).duql(q)), "i") == first);

        lines.push_back(R"({"type":"run","run":3,"app":"laghos"})");
        fs::remove(gz);
        const std::string again = write_records(env, "c", lines);
        REQUIRE(again == gz);
        CHECK(has(view_of(gz).explain_duql(q), {"not cached"}));
        CHECK(column(frame(view_of(gz).duql(q)), "i") ==
              std::vector<std::string>{"0", "2", "3", "5", "6", "8"});

        const fs::path cache =
            fs::path(determine_index_path(gz, "")).parent_path() /
            ".dftindex-cache";
        REQUIRE(fs::exists(cache));
        fs::remove_all(cache);
        CHECK(column(frame(view_of(gz).duql(q)), "i") ==
              std::vector<std::string>{"0", "2", "3", "5", "6", "8"});
    }

    TEST_CASE("results do not depend on checkpoint size or cache state") {
        TestEnvironment env(10);
        std::vector<std::string> lines;
        for (int r = 1; r <= 4; ++r)
            lines.push_back(R"({"type":"run","run":)" + std::to_string(r) +
                            R"(,"app":"a)" + std::to_string(r % 2) + "\"}");
        for (int i = 0; i < 600; ++i)
            lines.push_back(R"({"type":"ev","i":)" + std::to_string(i) +
                            R"(,"run":)" + std::to_string(i % 5 + 1) +
                            R"(,"dur":)" + std::to_string((i * 13) % 97) +
                            R"(,"pad":"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"})");
        const std::vector<std::string> queries = {
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
        std::vector<std::vector<std::string>> want;
        for (const std::size_t checkpoint :
             {std::size_t{2048}, std::size_t{65536}}) {
            CAPTURE(checkpoint);
            const std::string gz = write_records(
                env, "d" + std::to_string(checkpoint), lines, 4096, checkpoint);
            const fs::path cache =
                fs::path(determine_index_path(gz, "")).parent_path() /
                ".dftindex-cache";
            for (int pass = 0; pass < 3; ++pass) {
                CAPTURE(pass);
                if (pass == 2) fs::remove_all(cache);
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
}
