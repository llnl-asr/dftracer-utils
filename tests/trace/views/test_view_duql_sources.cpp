// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/macros.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/record_schema.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <trace/views/test_view_common.h>

#include <fstream>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;
using namespace test_view_common;

namespace {

dstd::String write_records(TestEnvironment& env, const dstd::String& name,
                           const dstd::Vector<dstd::String>& lines) {
    const dstd::String plain = env.dir() + "/" + name + ".ndjson";
    {
        dftu_utils_test::FileOut o(plain);
        for (const auto& l : lines) o << l << "\n";
    }
    const dstd::String gz = plain + ".gz";
    dftu_utils_test::compress_file_to_gzip(plain, gz);
    dstd::fs::remove(plain).discard();
    dftu_utils_test::ok(dftu_utils_test::ok(index::Indexer::open({dstd::String(
                                                dstd::StringView(gz))}))
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

dstd::String cell(const dataframe::DataFrame& f, std::int64_t r,
                  dstd::StringView name) {
    const auto i = bcol(f, name);
    REQUIRE(i >= 0);
    const auto& c = f.columns[static_cast<std::size_t>(i)];
    if (c.is_null(r)) return "null";
    if (c.type() == dataframe::TypeId::String) return bstr(f, r, name);
    return dstd::to_string(static_cast<long long>(bnum(f, r, name)));
}

dstd::Vector<dstd::String> column(const dataframe::DataFrame& f,
                                  dstd::StringView name) {
    dstd::Vector<dstd::String> out;
    for (std::int64_t r = 0; r < f.num_rows(); ++r)
        out.push_back(cell(f, r, name));
    return out;
}

dstd::String meta(const dstd::String& name, const dstd::String& value,
                  const dstd::String& key) {
    return R"({"ph":"M","name":")" + name +
           R"(","pid":1,"tid":1,"args":{"name":")" + key + R"(","value":")" +
           value + R"("}})";
}

dstd::String event(int ts, const dstd::String& args, int dur = 10) {
    return R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)" +
           dstd::to_string(ts) + R"(,"dur":)" + dstd::to_string(dur) +
           R"(,"args":{)" + args + "}}";
}

dstd::Vector<dstd::String> dftracer_lines() {
    return {meta("FH", "h1", "/data/a"),
            meta("HH", "n1", "node1"),
            meta("SH", "s1", "laghos"),
            meta("PR", "3", "rank"),
            event(10, R"("fhash":"h1","hhash":"n1","exec_hash":"s1")", 1),
            event(20, R"("fhash":"h2","hhash":"n1")", 2),
            meta("FH", "h2", "/data/b"),
            event(30, R"("fhash":"h1","hhash":"n1")", 3)};
}

// The message of the exception `fn` throws, or of the error it returns.
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
        const auto result = fn();
        if (result.has_value()) return {};
        return dstd::String(result.error().message());
    }
}

}  // namespace

TEST_SUITE("View duql sources") {
    TEST_CASE("dftracer row sets") {
        TestEnvironment env(10);
        const View v = view_of(write_records(env, "t", dftracer_lines()));
        const dataframe::DataFrame files =
            frame(v.duql("from files | sort fhash"));
        CHECK(files.names == dstd::Vector<dstd::String>{"fhash", "path"});
        CHECK(column(files, "path") ==
              dstd::Vector<dstd::String>{"/data/a", "/data/b"});
        const dataframe::DataFrame hosts = frame(v.duql("from hosts"));
        CHECK(hosts.names == dstd::Vector<dstd::String>{"hhash", "name"});
        CHECK(column(hosts, "name") == dstd::Vector<dstd::String>{"node1"});
        const dataframe::DataFrame strings = frame(v.duql("from strings"));
        CHECK(strings.names == dstd::Vector<dstd::String>{"shash", "value"});
        const dataframe::DataFrame ranks = frame(v.duql("from ranks"));
        CHECK(ranks.names == dstd::Vector<dstd::String>{"pid", "rank"});
        CHECK(column(ranks, "rank") == dstd::Vector<dstd::String>{"3"});
        // Metadata rows keep their args, which the index does not list.
        const dataframe::DataFrame pr =
            frame(v.duql("from all | where name == \"PR\" | take 1"));
        CHECK(column(pr, "args.value") == dstd::Vector<dstd::String>{"3"});
    }

    TEST_CASE("files that share a missing index read a schema tree") {
        TestEnvironment env(10);
        dstd::Vector<dstd::String> gzs;
        for (const char* n : {"a", "b"}) {
            const dstd::String plain = env.dir() + "/" + n + ".pfw";
            dftu_utils_test::FileOut(plain) << event(10, R"("x":1)") << "\n";
            gzs.push_back(plain + ".gz");
            dftu_utils_test::compress_file_to_gzip(plain, gzs.back());
        }
        const dstd::String idx = env.dir() + "/none/.dftindex";
        const View v =
            View::from_files({{dstd::String(gzs[0]), dstd::String(idx)},
                              {dstd::String(gzs[1]), dstd::String(idx)}});
        CHECK_NOTHROW((void)v.schema_tree());
    }

    TEST_CASE("an indexed row set reads no chunk") {
        TestEnvironment env(10);
        const View v = view_of(write_records(env, "t", dftracer_lines()));
        const dstd::String plan = v.explain_duql("from files");
        CHECK(plan.find("from files (rows stored in the index)") !=
              dstd::String::npos);
        CHECK(plan.find("scan") == dstd::String::npos);
        CHECK(frame(v.duql("from files")).num_rows() == 2);
    }

    TEST_CASE("arrows into source row sets") {
        TestEnvironment env(10);
        const View v = view_of(write_records(env, "t", dftracer_lines()));
        const dataframe::DataFrame f = frame(
            v.duql(R"(where fhash -> files.path == "/data/a" | select ts)"));
        CHECK(column(f, "ts") == dstd::Vector<dstd::String>{"10", "30"});
        CHECK(v.explain_duql(R"(where fhash -> files.path == "/data/a")")
                  .find("fhash in (keys of files) (pushed)") !=
              dstd::String::npos);
        const dataframe::DataFrame d =
            frame(v.duql("where ph != \"M\" | derive p = fhash -> files.path,"
                         " h = hhash -> hosts.name,"
                         " c = exec_hash -> strings(shash).value"
                         " | select ts, p, h, c"));
        CHECK(column(d, "p") ==
              dstd::Vector<dstd::String>{"/data/a", "/data/b", "/data/a"});
        CHECK(column(d, "h") ==
              dstd::Vector<dstd::String>{"node1", "node1", "node1"});
        CHECK(column(d, "c") ==
              dstd::Vector<dstd::String>{"laghos", "null", "null"});
        // A row set column typed only by its data (an args value) keeps it.
        CHECK(column(frame(v.duql("where ph != \"M\" | derive r = pid -> "
                                  "ranks.rank | select r")),
                     "r") == dstd::Vector<dstd::String>{"3", "3", "3"});
        CHECK(column(frame(v.duql("from ranks | derive x = rank | select x")),
                     "x") == dstd::Vector<dstd::String>{"3"});
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
            const dstd::String on = dstd::String("where ") + q + " | select ts";
            const dstd::String off =
                dstd::String("where (") + q + ") or ts < 0 | select ts";
            CHECK(v.explain_duql(off).find("keys of") == dstd::String::npos);
            CHECK(column(frame(v.duql(on)), "ts") ==
                  column(frame(v.duql(off)), "ts"));
        }
        CHECK(column(
                  frame(v.duql(
                      R"(where fhash -> files.path == "/data/a" | select ts)")),
                  "ts") == dstd::Vector<dstd::String>{"10", "30"});
    }

    TEST_CASE("a macro in a filter pushes down") {
        TestEnvironment env(10);
        const View v = view_of(write_records(env, "t", dftracer_lines()));
        const dstd::String q =
            "def slow(t) = dur > t; where slow(1) | select ts";
        CHECK(column(frame(v.duql(q)), "ts") ==
              dstd::Vector<dstd::String>{"20", "30"});
        CHECK(v.explain_duql(q).find("scan filter: dur > 1 (pushed)") !=
              dstd::String::npos);
    }

    TEST_CASE("macros from a file") {
        TestEnvironment env(10);
        const dstd::String path = env.dir() + "/ms.duql";
        {
            dftu_utils_test::FileOut o(path);
            o << "def ms(x) = x / 1000;\ndef long_io(t) = ms(dur) > t;\n";
        }
        REQUIRE(dftracer::utils::duql::load_macros(path).has_value());
        const View v = view_of(write_records(env, "t", dftracer_lines()));
        CHECK(column(frame(v.duql("where long_io(0.0015) | select ts")),
                     "ts") == dstd::Vector<dstd::String>{"20", "30"});
        CHECK(column(frame(v.duql("def ms(x) = x; where ms(dur) > 2"
                                  " | select ts")),
                     "ts") == dstd::Vector<dstd::String>{"30"});
    }

    TEST_CASE("a macro file that is missing gives an error") {
        TestEnvironment env(10);
        const auto loaded =
            dftracer::utils::duql::load_macros(env.dir() + "/missing.duql");
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error().is_code(dftracer::utils::common::ErrorCode::Io));
    }

    TEST_CASE("pipeline macros run as stages") {
        TestEnvironment env(10);
        const View v = view_of(write_records(env, "t", dftracer_lines()));
        const dstd::String rate =
            "def io_rate(d) = where cat == \"POSIX\" | bucket d | agg { b = "
            "sum(dur) }; ";
        const dstd::String hand =
            "where cat == \"POSIX\" | bucket 10us | agg { b = sum(dur) } | "
            "sort b";
        const dataframe::DataFrame want = frame(v.duql(hand));
        REQUIRE(want.num_rows() == 3);
        const dataframe::DataFrame stage =
            frame(v.duql(rate + "where ts > 0 | io_rate(10us) | sort b"));
        CHECK(stage.names == want.names);
        CHECK(column(stage, "b") == column(want, "b"));
        CHECK(column(stage, "bucket") == column(want, "bucket"));

        const dstd::String leading = rate + "io_rate(10us) | sort b";
        CHECK(column(frame(v.duql(leading)), "b") == column(want, "b"));
        const dstd::String pushed = "scan filter: cat == \"POSIX\" (pushed)";
        REQUIRE(v.explain_duql(hand).find(pushed) != dstd::String::npos);
        CHECK(v.explain_duql(leading).find(pushed) != dstd::String::npos);

        CHECK(column(frame(v.duql("def slow(t) = dur > t; slow(1) | select "
                                  "ts")),
                     "ts") ==
              column(frame(v.duql("where dur > 1 | select ts")), "ts"));
        CHECK(error_of([&] {
                  (void)v.duql(rate + "where io_rate(1ms)");
              }).find("is a pipeline; call it as a stage") !=
              dstd::String::npos);
        CHECK(error_of([&] {
                  (void)v.duql(
                      "def slow(t) = dur > t; where ts > 0 | "
                      "slow(1)");
              }).find("write 'where slow(...)'") != dstd::String::npos);
    }

    TEST_CASE("pipeline macros from a source and a file") {
        dftu_utils_test::ok_ref(index::register_schema(R"(
id: weblog2
fields:
  status: {type: int}
  request_time: {type: float, role: duration, unit: s}
source: |
  def worst(n) = sort -status | where status > n
)",
                                                       "test"));
        TestEnvironment env(10);
        const View w =
            view_of(write_records(env, "w2",
                                  {R"({"status":200,"request_time":0.5})",
                                   R"({"status":503,"request_time":2.5})",
                                   R"({"status":404,"request_time":1.5})"}))
                .record_schema("weblog2");
        CHECK(column(frame(w.duql("where status > 0 | worst(300) | select "
                                  "status")),
                     "status") == dstd::Vector<dstd::String>{"503", "404"});

        const dstd::String path = env.dir() + "/pm.duql";
        {
            dftu_utils_test::FileOut o(path);
            o << "def posix_rate(d) = where cat == \"POSIX\" | bucket d | agg "
                 "{ b = sum(dur) };\n";
        }
        REQUIRE(dftracer::utils::duql::load_macros(path).has_value());
        const View v = view_of(write_records(env, "t", dftracer_lines()));
        const dataframe::DataFrame want = frame(v.duql(
            "where cat == \"POSIX\" | bucket 10us | agg { b = sum(dur) } | "
            "sort b"));
        CHECK(column(frame(v.duql("where ts > 0 | posix_rate(10us) | sort b")),
                     "b") == column(want, "b"));
        CHECK(column(frame(v.duql("def posix_rate(d) = where dur > 1 | "
                                  "take 1; where ts > 0 | posix_rate(10us)"
                                  " | select ts")),
                     "ts") == dstd::Vector<dstd::String>{"20"});
    }

    TEST_CASE("a user schema's source") {
        dftu_utils_test::ok_ref(index::register_schema(R"(
id: weblog
fields:
  status: {type: int}
  request_time: {type: float, role: duration, unit: s}
source: |
  slow = where request_time > 1;
  def failed = status >= 500
)",
                                                       "test"));
        TestEnvironment env(10);
        const View v =
            view_of(write_records(env, "w",
                                  {R"({"status":200,"request_time":0.5})",
                                   R"({"status":503,"request_time":2.5})",
                                   R"({"status":200,"request_time":1.5})"}));
        CHECK(column(frame(v.duql("from slow | select status")), "status") ==
              dstd::Vector<dstd::String>{"503", "200"});
        CHECK(column(frame(v.duql("where failed() | select status")),
                     "status") == dstd::Vector<dstd::String>{"503"});
    }

    TEST_CASE("sources that do not compile reject registration") {
        CHECK(error_of([] {
                  return index::register_schema(
                      "id: bad1\nsource: \"slow = where x >\"", "test");
              }).find("bad1") != dstd::String::npos);
        CHECK(error_of([] {
                  return index::register_schema(
                      "id: bad2\nsource: \"data = where x > 1 | take 1\"",
                      "test");
              }).find("'data' must be one 'where'") != dstd::String::npos);
        CHECK(error_of([] {
                  return index::register_schema(
                      "id: bad3\nsource: \"all = where x\"", "test");
              }).find("'all' is every record") != dstd::String::npos);
    }

    TEST_CASE("only a source with args_fallback reads bare names under args") {
        CHECK(dftu_utils_test::ok_ref(index::get_schema("dftracer"))
                  .args_fallback);
        CHECK_FALSE(dftu_utils_test::ok_ref(index::get_schema("generic"))
                        .args_fallback);
        TestEnvironment env(10);
        const View d = view_of(write_records(env, "t", dftracer_lines()));
        CHECK(column(frame(d.duql(R"(where fhash == "h2" | select ts)")),
                     "ts") == dstd::Vector<dstd::String>{"20"});
        const View g = view_of(write_records(env, "g",
                                             {R"({"k":1,"args":{"y":2}})",
                                              R"({"k":2,"args":{"y":3}})"}))
                           .record_schema("generic");
        CHECK(frame(g.duql("where y == 2 | select k")).num_rows() == 0);
        CHECK(column(frame(g.duql("where args.y == 2 | select k")), "k") ==
              dstd::Vector<dstd::String>{"1"});
        CHECK(frame(g.duql("where k > 0 | derive z = y | where z == 2"))
                  .num_rows() == 0);
        const View a = view_of(write_records(
            env, "a", {event(10, R"("y":2)"), event(20, R"("y":3)")}));
        CHECK(column(frame(a.duql("where y == 2 | select ts")), "ts") ==
              dstd::Vector<dstd::String>{"10"});
        CHECK(column(frame(a.duql("where ts > 0 | derive z = y | where z == 3"
                                  " | select ts")),
                     "ts") == dstd::Vector<dstd::String>{"20"});
    }

    TEST_CASE("a let cannot take a row set's name") {
        TestEnvironment env(10);
        const View v = view_of(write_records(env, "t", dftracer_lines()));
        CHECK(error_of([&] {
                  (void)v.duql("let files = where x > 1; from files");
              }).find("names a row set of the source") != dstd::String::npos);
        CHECK(error_of([&] {
                  (void)v.duql("from nothing");
              }).find("not a let or a row set of the source") !=
              dstd::String::npos);
    }
}
