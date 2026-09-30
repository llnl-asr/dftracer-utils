#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/duql/macros.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/record_schema.h>
#include <doctest/doctest.h>

#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "test_view_common.h"

namespace df = dftracer::utils::dataframe;
namespace ix = dftracer::utils::index;

namespace {

std::string write_records(TestEnvironment& env, const std::string& name,
                          const std::vector<std::string>& lines) {
    const std::string plain = env.get_dir() + "/" + name + ".ndjson";
    {
        std::ofstream o(plain);
        for (const auto& l : lines) o << l << "\n";
    }
    const std::string gz = plain + ".gz";
    dftu_utils_test::compress_file_to_gzip(plain, gz);
    fs::remove(plain);
    ix::Indexer::open({gz}).build();
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

std::string cell(const df::DataFrame& f, std::int64_t r,
                 std::string_view name) {
    const auto i = bcol(f, name);
    REQUIRE(i >= 0);
    const auto& c = f.columns[static_cast<std::size_t>(i)];
    if (c.is_null(r)) return "null";
    if (c.type() == df::TypeId::String) return bstr(f, r, name);
    return std::to_string(static_cast<long long>(bnum(f, r, name)));
}

std::vector<std::string> column(const df::DataFrame& f, std::string_view name) {
    std::vector<std::string> out;
    for (std::int64_t r = 0; r < f.num_rows(); ++r)
        out.push_back(cell(f, r, name));
    return out;
}

std::string meta(const std::string& name, const std::string& value,
                 const std::string& key) {
    return R"({"ph":"M","name":")" + name +
           R"(","pid":1,"tid":1,"args":{"name":")" + key + R"(","value":")" +
           value + R"("}})";
}

std::string event(int ts, const std::string& args, int dur = 10) {
    return R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)" +
           std::to_string(ts) + R"(,"dur":)" + std::to_string(dur) +
           R"(,"args":{)" + args + "}}";
}

std::vector<std::string> dftracer_lines() {
    return {meta("FH", "h1", "/data/a"),
            meta("HH", "n1", "node1"),
            meta("SH", "s1", "laghos"),
            meta("PR", "3", "rank"),
            event(10, R"("fhash":"h1","hhash":"n1","exec_hash":"s1")", 1),
            event(20, R"("fhash":"h2","hhash":"n1")", 2),
            meta("FH", "h2", "/data/b"),
            event(30, R"("fhash":"h1","hhash":"n1")", 3)};
}

std::string error_of(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const std::exception& e) {
        return e.what();
    }
    return {};
}

}  // namespace

TEST_SUITE("View duql sources") {
    TEST_CASE("dftracer row sets") {
        TestEnvironment env(10);
        const View v = view_of(write_records(env, "t", dftracer_lines()));
        const df::DataFrame files = frame(v.duql("from files | sort fhash"));
        CHECK(files.names == std::vector<std::string>{"fhash", "path"});
        CHECK(column(files, "path") ==
              std::vector<std::string>{"/data/a", "/data/b"});
        const df::DataFrame hosts = frame(v.duql("from hosts"));
        CHECK(hosts.names == std::vector<std::string>{"hhash", "name"});
        CHECK(column(hosts, "name") == std::vector<std::string>{"node1"});
        const df::DataFrame strings = frame(v.duql("from strings"));
        CHECK(strings.names == std::vector<std::string>{"shash", "value"});
        const df::DataFrame ranks = frame(v.duql("from ranks"));
        CHECK(ranks.names == std::vector<std::string>{"pid", "rank"});
        CHECK(column(ranks, "rank") == std::vector<std::string>{"3"});
        // Metadata rows keep their args, which the index does not list.
        const df::DataFrame pr =
            frame(v.duql("from all | where name == \"PR\" | take 1"));
        CHECK(column(pr, "args.value") == std::vector<std::string>{"3"});
    }

    TEST_CASE("files that share a missing index read a schema tree") {
        TestEnvironment env(10);
        std::vector<std::string> gzs;
        for (const char* n : {"a", "b"}) {
            const std::string plain = env.get_dir() + "/" + n + ".pfw";
            std::ofstream(plain) << event(10, R"("x":1)") << "\n";
            gzs.push_back(plain + ".gz");
            dftu_utils_test::compress_file_to_gzip(plain, gzs.back());
        }
        const std::string idx = env.get_dir() + "/none/.dftindex";
        const View v = View::from_files({{gzs[0], idx}, {gzs[1], idx}});
        CHECK_NOTHROW((void)v.schema_tree());
    }

    TEST_CASE("an indexed row set reads no chunk") {
        TestEnvironment env(10);
        const View v = view_of(write_records(env, "t", dftracer_lines()));
        const std::string plan = v.explain_duql("from files");
        CHECK(plan.find("from files (rows stored in the index)") !=
              std::string::npos);
        CHECK(plan.find("scan") == std::string::npos);
        CHECK(frame(v.duql("from files")).num_rows() == 2);
    }

    TEST_CASE("arrows into source row sets") {
        TestEnvironment env(10);
        const View v = view_of(write_records(env, "t", dftracer_lines()));
        const df::DataFrame f = frame(
            v.duql(R"(where fhash -> files.path == "/data/a" | select ts)"));
        CHECK(column(f, "ts") == std::vector<std::string>{"10", "30"});
        CHECK(v.explain_duql(R"(where fhash -> files.path == "/data/a")")
                  .find("fhash in (keys of files) (pushed)") !=
              std::string::npos);
        const df::DataFrame d =
            frame(v.duql("where ph != \"M\" | derive p = fhash -> files.path,"
                         " h = hhash -> hosts.name,"
                         " c = exec_hash -> strings(shash).value"
                         " | select ts, p, h, c"));
        CHECK(column(d, "p") ==
              std::vector<std::string>{"/data/a", "/data/b", "/data/a"});
        CHECK(column(d, "h") ==
              std::vector<std::string>{"node1", "node1", "node1"});
        CHECK(column(d, "c") ==
              std::vector<std::string>{"laghos", "null", "null"});
        // A row set column typed only by its data (an args value) keeps it.
        CHECK(column(frame(v.duql("where ph != \"M\" | derive r = pid -> "
                                  "ranks.rank | select r")),
                     "r") == std::vector<std::string>{"3", "3", "3"});
        CHECK(column(frame(v.duql("from ranks | derive x = rank | select x")),
                     "x") == std::vector<std::string>{"3"});
    }

    TEST_CASE("pushdown on and off give the same rows") {
        TestEnvironment env(10);
        const View v = view_of(write_records(env, "t", dftracer_lines()));
        for (const char* q :
             {R"(fhash -> files.path == "/data/a")",
              R"(fhash -> files.path like "/data/%" and dur > 1)",
              R"(hhash -> hosts.name == "node1")",
              R"(exec_hash -> strings(shash).value == "laghos")",
              R"(fhash -> files.path != name)"}) {
            CAPTURE(q);
            const std::string on = std::string("where ") + q + " | select ts";
            const std::string off =
                std::string("where (") + q + ") or ts < 0 | select ts";
            CHECK(v.explain_duql(off).find("keys of") == std::string::npos);
            CHECK(column(frame(v.duql(on)), "ts") ==
                  column(frame(v.duql(off)), "ts"));
        }
        CHECK(column(
                  frame(v.duql(
                      R"(where fhash -> files.path == "/data/a" | select ts)")),
                  "ts") == std::vector<std::string>{"10", "30"});
    }

    TEST_CASE("a macro in a filter pushes down") {
        TestEnvironment env(10);
        const View v = view_of(write_records(env, "t", dftracer_lines()));
        const std::string q =
            "def slow(t) = dur > t; where slow(1) | select ts";
        CHECK(column(frame(v.duql(q)), "ts") ==
              std::vector<std::string>{"20", "30"});
        CHECK(v.explain_duql(q).find("scan filter: dur > 1 (pushed)") !=
              std::string::npos);
    }

    TEST_CASE("macros from a file") {
        TestEnvironment env(10);
        const std::string path = env.get_dir() + "/ms.duql";
        {
            std::ofstream o(path);
            o << "def ms(x) = x / 1000;\ndef long_io(t) = ms(dur) > t;\n";
        }
        dftracer::utils::duql::load_macros(path);
        const View v = view_of(write_records(env, "t", dftracer_lines()));
        CHECK(column(frame(v.duql("where long_io(0.0015) | select ts")),
                     "ts") == std::vector<std::string>{"20", "30"});
        CHECK(column(frame(v.duql("def ms(x) = x; where ms(dur) > 2"
                                  " | select ts")),
                     "ts") == std::vector<std::string>{"30"});
    }

    TEST_CASE("pipeline macros run as stages") {
        TestEnvironment env(10);
        const View v = view_of(write_records(env, "t", dftracer_lines()));
        const std::string rate =
            "def io_rate(d) = where cat == \"POSIX\" | bucket d | agg { b = "
            "sum(dur) }; ";
        const std::string hand =
            "where cat == \"POSIX\" | bucket 10us | agg { b = sum(dur) } | "
            "sort b";
        const df::DataFrame want = frame(v.duql(hand));
        REQUIRE(want.num_rows() == 3);
        const df::DataFrame stage =
            frame(v.duql(rate + "where ts > 0 | io_rate(10us) | sort b"));
        CHECK(stage.names == want.names);
        CHECK(column(stage, "b") == column(want, "b"));
        CHECK(column(stage, "bucket") == column(want, "bucket"));

        const std::string leading = rate + "io_rate(10us) | sort b";
        CHECK(column(frame(v.duql(leading)), "b") == column(want, "b"));
        const std::string pushed = "scan filter: cat == \"POSIX\" (pushed)";
        REQUIRE(v.explain_duql(hand).find(pushed) != std::string::npos);
        CHECK(v.explain_duql(leading).find(pushed) != std::string::npos);

        CHECK(column(frame(v.duql("def slow(t) = dur > t; slow(1) | select "
                                  "ts")),
                     "ts") ==
              column(frame(v.duql("where dur > 1 | select ts")), "ts"));
        CHECK(error_of([&] {
                  (void)v.duql(rate + "where io_rate(1ms)");
              }).find("is a pipeline; call it as a stage") !=
              std::string::npos);
        CHECK(error_of([&] {
                  (void)v.duql(
                      "def slow(t) = dur > t; where ts > 0 | "
                      "slow(1)");
              }).find("write 'where slow(...)'") != std::string::npos);
    }

    TEST_CASE("pipeline macros from a source and a file") {
        ix::register_schema(R"(
id: weblog2
fields:
  status: {type: int}
  request_time: {type: float, role: duration, unit: s}
source: |
  def worst(n) = sort -status | where status > n
)",
                            "test");
        TestEnvironment env(10);
        const View w =
            view_of(write_records(env, "w2",
                                  {R"({"status":200,"request_time":0.5})",
                                   R"({"status":503,"request_time":2.5})",
                                   R"({"status":404,"request_time":1.5})"}))
                .record_schema("weblog2");
        CHECK(column(frame(w.duql("where status > 0 | worst(300) | select "
                                  "status")),
                     "status") == std::vector<std::string>{"503", "404"});

        const std::string path = env.get_dir() + "/pm.duql";
        {
            std::ofstream o(path);
            o << "def posix_rate(d) = where cat == \"POSIX\" | bucket d | agg "
                 "{ b = sum(dur) };\n";
        }
        dftracer::utils::duql::load_macros(path);
        const View v = view_of(write_records(env, "t", dftracer_lines()));
        const df::DataFrame want = frame(v.duql(
            "where cat == \"POSIX\" | bucket 10us | agg { b = sum(dur) } | "
            "sort b"));
        CHECK(column(frame(v.duql("where ts > 0 | posix_rate(10us) | sort b")),
                     "b") == column(want, "b"));
        CHECK(column(frame(v.duql("def posix_rate(d) = where dur > 1 | "
                                  "take 1; where ts > 0 | posix_rate(10us)"
                                  " | select ts")),
                     "ts") == std::vector<std::string>{"20"});
    }

    TEST_CASE("a user schema's source") {
        ix::register_schema(R"(
id: weblog
fields:
  status: {type: int}
  request_time: {type: float, role: duration, unit: s}
source: |
  slow = where request_time > 1;
  def failed = status >= 500
)",
                            "test");
        TestEnvironment env(10);
        const View v =
            view_of(write_records(env, "w",
                                  {R"({"status":200,"request_time":0.5})",
                                   R"({"status":503,"request_time":2.5})",
                                   R"({"status":200,"request_time":1.5})"}));
        CHECK(column(frame(v.duql("from slow | select status")), "status") ==
              std::vector<std::string>{"503", "200"});
        CHECK(column(frame(v.duql("where failed() | select status")),
                     "status") == std::vector<std::string>{"503"});
    }

    TEST_CASE("sources that do not compile reject registration") {
        CHECK(error_of([] {
                  ix::register_schema("id: bad1\nsource: \"slow = where x >\"",
                                      "test");
              }).find("bad1") != std::string::npos);
        CHECK(error_of([] {
                  ix::register_schema(
                      "id: bad2\nsource: \"data = where x > 1 | take 1\"",
                      "test");
              }).find("'data' must be one 'where'") != std::string::npos);
        CHECK(error_of([] {
                  ix::register_schema("id: bad3\nsource: \"all = where x\"",
                                      "test");
              }).find("'all' is every record") != std::string::npos);
    }

    TEST_CASE("only a source with args_fallback reads bare names under args") {
        CHECK(ix::get_schema("dftracer").args_fallback);
        CHECK_FALSE(ix::get_schema("generic").args_fallback);
        TestEnvironment env(10);
        const View d = view_of(write_records(env, "t", dftracer_lines()));
        CHECK(column(frame(d.duql(R"(where fhash == "h2" | select ts)")),
                     "ts") == std::vector<std::string>{"20"});
        const View g = view_of(write_records(env, "g",
                                             {R"({"k":1,"args":{"y":2}})",
                                              R"({"k":2,"args":{"y":3}})"}))
                           .record_schema("generic");
        CHECK(frame(g.duql("where y == 2 | select k")).num_rows() == 0);
        CHECK(column(frame(g.duql("where args.y == 2 | select k")), "k") ==
              std::vector<std::string>{"1"});
        CHECK(frame(g.duql("where k > 0 | derive z = y | where z == 2"))
                  .num_rows() == 0);
        const View a = view_of(write_records(
            env, "a", {event(10, R"("y":2)"), event(20, R"("y":3)")}));
        CHECK(column(frame(a.duql("where y == 2 | select ts")), "ts") ==
              std::vector<std::string>{"10"});
        CHECK(column(frame(a.duql("where ts > 0 | derive z = y | where z == 3"
                                  " | select ts")),
                     "ts") == std::vector<std::string>{"20"});
    }

    TEST_CASE("a let cannot take a row set's name") {
        TestEnvironment env(10);
        const View v = view_of(write_records(env, "t", dftracer_lines()));
        CHECK(error_of([&] {
                  (void)v.duql("let files = where x > 1; from files");
              }).find("names a row set of the source") != std::string::npos);
        CHECK(error_of([&] {
                  (void)v.duql("from nothing");
              }).find("not a let or a row set of the source") !=
              std::string::npos);
    }
}
