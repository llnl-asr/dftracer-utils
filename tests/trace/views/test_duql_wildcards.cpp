#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "test_view_common.h"

namespace {

constexpr int RECORDS = 3000;

// Object siblings under args.counters (a, b and c with p50, a also with p99
// and p75, c with a scalar `meta` one segment short of the pattern) and an
// array of 12 positions. a.p50 climbs with the record, so only the last
// chunks hold a value above 92.
std::string write_counters(TestEnvironment& env, const std::string& dir = "",
                           bool build = true) {
    if (!dir.empty()) fs::create_directories(env.get_dir() + "/" + dir);
    const std::string plain = env.get_dir() + "/" +
                              (dir.empty() ? "" : dir + "/") +
                              "counters.ndjson";
    {
        std::ofstream out(plain);
        for (int i = 0; i < RECORDS; ++i) {
            out << R"({"name":"ev)" << i << R"(","args":{"counters":{"a":)"
                << R"({"p50":)" << i / 30 << R"(,"p99":)" << i / 30 + 5
                << R"(,"p75":)" << i % 9 << R"(},"b":{"p50":)" << i % 7
                << R"(,"p99":1},)"
                << R"("c":{"p50":)" << i % 50 << R"(},"meta":"m"},"pos":[)";
            for (int k = 0; k < 12; ++k) out << (k ? "," : "") << i + k;
            out << "]}}\n";
        }
    }
    const std::string gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               16 * 1024));
    fs::remove(plain);
    if (build) dftracer::utils::index::Indexer::open({gz}).build();
    return gz;
}

View view_of(const std::string& gz) {
    return View::from_file(gz, determine_index_path(gz, ""));
}

dataframe::DataFrame frame(const View& v) { return v.collect().get(); }

std::vector<std::string> names(const dataframe::DataFrame& f) {
    return f.names;
}

void same(const View& v, const std::string& wild, const std::string& spelled) {
    CAPTURE(wild);
    CAPTURE(spelled);
    const auto a = frame(v.duql(wild));
    const auto b = frame(v.duql(spelled));
    REQUIRE(names(a) == names(b));
    REQUIRE(a.num_rows() == b.num_rows());
    for (const auto& n : names(a))
        for (std::int64_t r = 0; r < a.num_rows(); ++r) {
            const auto& ca = a.columns[static_cast<std::size_t>(bcol(a, n))];
            if (ca.type() == dataframe::TypeId::String)
                CHECK(bstr(a, r, n) == bstr(b, r, n));
            else if (!ca.is_null(r))
                CHECK(bnum(a, r, n) == bnum(b, r, n));
        }
}

std::string error_of(const View& v, const std::string& q) {
    try {
        (void)v.duql(q);
    } catch (const std::exception& e) {
        return e.what();
    }
    return {};
}

struct NullSink : ExportSink {
    void write(std::string_view) override {}
};

}  // namespace

TEST_SUITE("DuqlWildcards") {
    TEST_CASE("select, drop and unpivot expand like the spelled-out paths") {
        TestEnvironment env(10);
        const auto v = view_of(write_counters(env));
        same(v, "select name, args.counters.*.p50 | take 40",
             "select name, args.counters.a.p50, args.counters.b.p50, "
             "args.counters.c.p50 | take 40");
        same(v, "select name, args.counters.a.* | take 40",
             "select name, args.counters.a.p50, args.counters.a.p75, "
             "args.counters.a.p99 | take 40");
        same(v,
             "select name, args.counters.*.p50, args.counters.a.p99 | "
             "drop args.counters.*.p50 | take 40",
             "select name, args.counters.a.p99 | take 40");
        same(v,
             "select name, args.counters.*.p50 | "
             "unpivot args.counters.*.p50 as k, v | take 60",
             "select name, args.counters.a.p50, args.counters.b.p50, "
             "args.counters.c.p50 | unpivot args.counters.a.p50, "
             "args.counters.b.p50, args.counters.c.p50 as k, v | take 60");
    }

    TEST_CASE("array positions expand in numeric order") {
        TestEnvironment env(10);
        const auto v = view_of(write_counters(env));
        const auto f = frame(v.duql("select args.pos.* | take 3"));
        std::vector<std::string> want;
        for (int k = 0; k < 12; ++k)
            want.push_back("args.pos." + std::to_string(k));
        CHECK(names(f) == want);
    }

    TEST_CASE("any and all over a pattern are the OR and AND of the matches") {
        TestEnvironment env(10);
        const auto v = view_of(write_counters(env));
        same(v, "where any(args.counters.*.p50) > 92 | select name",
             "args.counters.a.p50 > 92 or args.counters.b.p50 > 92 or "
             "args.counters.c.p50 > 92 | select name");
        same(v, "where all(args.counters.*.p50) < 3 | select name",
             "where args.counters.a.p50 < 3 and args.counters.b.p50 < 3 and "
             "args.counters.c.p50 < 3 | select name");
        same(v, "where any(args.counters.*.p75) == 8 | select name",
             "where args.counters.a.p75 == 8 | select name");
        same(v, "where not any(args.counters.*.p50) > 92 | select name",
             "where not (args.counters.a.p50 > 92 or args.counters.b.p50 > 92 "
             "or args.counters.c.p50 > 92) | select name");
        same(v,
             "select name, args.counters.*.p50 | "
             "where any(args.counters.*.p50) > 92",
             "select name, args.counters.a.p50, args.counters.b.p50, "
             "args.counters.c.p50 | where args.counters.a.p50 > 92 or "
             "args.counters.b.p50 > 92 or args.counters.c.p50 > 92");
    }

    TEST_CASE("a pattern with no match names itself") {
        TestEnvironment env(10);
        const auto v = view_of(write_counters(env));
        for (const auto& [q, pattern] :
             std::vector<std::pair<std::string, std::string>>{
                 {"select args.nope.*", "args.nope.*"},
                 {"drop args.counters.*.p42", "args.counters.*.p42"},
                 {"where any(args.counters.*.zz) > 1", "args.counters.*.zz"},
                 {"unpivot args.nope.* as k, v", "args.nope.*"}}) {
            CAPTURE(q);
            const std::string e = error_of(v, q);
            CHECK(e.find("no field matches the pattern '" + pattern + "'") !=
                  std::string::npos);
        }
    }

    TEST_CASE("the expanded comparisons prune chunks") {
        TestEnvironment env(10);
        const auto gz = write_counters(env);
        const std::string wild = "any(args.counters.*.p50) > 92";
        const std::string spelled =
            "args.counters.a.p50 > 92 or args.counters.b.p50 > 92 or "
            "args.counters.c.p50 > 92";
        const std::string plan = view_of(gz).explain_duql(wild);
        CHECK(plan.find("args.counters.a.p50") != std::string::npos);
        CHECK(plan.find("args.counters.c.p50") != std::string::npos);
        CHECK(plan.find("*") == std::string::npos);
        NullSink a;
        NullSink b;
        const auto sw = view_of(gz).duql(wild).sink_json(a).get();
        const auto ss = view_of(gz).duql(spelled).sink_json(b).get();
        CHECK(sw.events_matched == ss.events_matched);
        CHECK(sw.events_matched == 210);
        CHECK(sw.events_scanned < RECORDS);
        CHECK(sw.events_scanned == ss.events_scanned);
    }

    TEST_CASE("a wildcard on a trace with no catalog builds it first") {
        TestEnvironment env(10);
        const std::string plain = env.get_dir() + "/first.ndjson";
        {
            std::ofstream out(plain);
            for (int i = 0; i < 300; ++i)
                out << R"({"name":"e","args":{"counters":{"a":{"p50":)" << i
                    << R"(},"b":{"p50":)" << i % 5 << "}}}}\n";
        }
        const std::string gz = plain + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                                   2048));
        const View v = View::from_file(gz, determine_index_path(gz, ""));
        CHECK(error_of(v, "select args.counters.*.p50").empty());
        same(v, "select name, args.counters.*.p50 | take 40",
             "select name, args.counters.a.p50, args.counters.b.p50 | "
             "take 40");
    }

    TEST_CASE(
        "a pattern as the first query on a fresh trace builds the catalog") {
        TestEnvironment env(10);
        const auto full = view_of(write_counters(env, "full"));
        int n = 0;
        // Each query is the first on its own trace, indexed only by a query.
        const auto fresh = [&] {
            const auto gz = write_counters(env, "f" + std::to_string(n++),
                                           /*build=*/false);
            (void)view_of(gz).duql("name == \"ev1\"").collect().get();
            return view_of(gz);
        };
        for (const auto& [wild, spelled] :
             std::vector<std::pair<std::string, std::string>>{
                 {"select name, args.counters.*.p50 | take 40",
                  "select name, args.counters.a.p50, args.counters.b.p50, "
                  "args.counters.c.p50 | take 40"},
                 {"where any(args.counters.*.p50) > 92 | select name",
                  "where args.counters.a.p50 > 92 or args.counters.b.p50 > 92 "
                  "or args.counters.c.p50 > 92 | select name"},
                 {"select name, args.counters.*.p50 | "
                  "unpivot args.counters.*.p50 as k, v | take 60",
                  "select name, args.counters.a.p50, args.counters.b.p50, "
                  "args.counters.c.p50 | unpivot args.counters.a.p50, "
                  "args.counters.b.p50, args.counters.c.p50 as k, v | "
                  "take 60"}}) {
            CAPTURE(wild);
            const auto a = frame(fresh().duql(wild));
            const auto b = frame(full.duql(spelled));
            REQUIRE(names(a) == names(b));
            REQUIRE(a.num_rows() == b.num_rows());
            for (const auto& c : names(a))
                for (std::int64_t r = 0; r < a.num_rows(); ++r) {
                    const auto& ca =
                        a.columns[static_cast<std::size_t>(bcol(a, c))];
                    if (ca.type() == dataframe::TypeId::String)
                        CHECK(bstr(a, r, c) == bstr(b, r, c));
                    else if (!ca.is_null(r))
                        CHECK(bnum(a, r, c) == bnum(b, r, c));
                }
        }
    }

    TEST_CASE("a pattern on a fresh genesis trace builds the catalog") {
        TestEnvironment env(10);
        const auto mk = [&](const std::string& dir, bool build) {
            fs::create_directories(env.get_dir() + "/" + dir);
            const std::string plain = env.get_dir() + "/" + dir + "/g.ndjson";
            {
                std::ofstream out(plain);
                for (int i = 0; i < 600; ++i)
                    out << R"({"gtype":"func","run":"ab","ts":)" << i
                        << R"(,"v":{"p50":)" << i / 30 << R"(,"p99":)"
                        << i / 30 + 5 << R"(},"count":)" << i % 7 << "}\n";
            }
            const std::string gz = plain + ".gz";
            REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(
                plain, gz, 2048));
            fs::remove(plain);
            if (build) dftracer::utils::index::Indexer::open({gz}).build();
            return view_of(gz);
        };
        const auto full = mk("full", true);
        const auto fresh = mk("fresh", false);
        const auto a = frame(fresh.duql("select run, v.* | take 50"));
        const auto b = frame(full.duql("select run, v.p50, v.p99 | take 50"));
        REQUIRE(names(a) == names(b));
        REQUIRE(a.num_rows() == b.num_rows());
        for (const auto& c : names(a))
            for (std::int64_t r = 0; r < a.num_rows(); ++r) {
                const auto& ca =
                    a.columns[static_cast<std::size_t>(bcol(a, c))];
                if (ca.type() == dataframe::TypeId::String)
                    CHECK(bstr(a, r, c) == bstr(b, r, c));
                else if (!ca.is_null(r))
                    CHECK(bnum(a, r, c) == bnum(b, r, c));
            }
    }
}
