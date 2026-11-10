// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/plan/condition.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <trace/views/test_view_common.h>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;
using namespace test_view_common;

namespace {

constexpr int FILES = 6000;
constexpr int EVENTS = 2 * FILES;

dstd::String path_of(int f) { return "/data/f" + dstd::to_string(f); }

// FH/HH/SH metadata records, a start event keyed to its executable, then
// two reads per file in file order, so each chunk holds its own files.
dstd::String write_trace(TestEnvironment& env) {
    const dstd::String plain = env.dir() + "/r.pfw";
    {
        dftu_utils_test::FileOut out(plain);
        for (int f = 0; f < FILES; ++f)
            out << R"({"name":"FH","ph":"M","pid":1,"tid":1,"args":{"name":")"
                << path_of(f) << R"(","value":"k)" << f << "\"}}\n";
        out << R"({"name":"HH","ph":"M","pid":1,"tid":1,"args":{"name":"node0","value":"h0"}})"
            << "\n";
        out << R"({"name":"SH","ph":"M","pid":1,"tid":1,"args":{"name":"/opt/laghos","value":"e0"}})"
            << "\n";
        out << R"({"name":"start","cat":"dftracer","ph":"X","pid":1,"tid":1,"ts":1,"dur":0,"args":{"exec_hash":"e0"}})"
            << "\n";
        for (int i = 0; i < EVENTS; ++i)
            out << R"({"name":"read","cat":"POSIX","ph":"X","pid":1,"tid":1,"ts":)"
                << 10 + i << R"(,"dur":1,"args":{"fhash":"k)" << i / 2
                << R"(","hhash":"h0"}})" << "\n";
    }
    const dstd::String gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               16 * 1024));
    dstd::fs::remove(plain).discard();
    dftu_utils_test::ok(
        dftu_utils_test::ok(dftracer::utils::index::Indexer::open(
                                {dstd::String(dstd::StringView(gz))}))
            .build());
    return gz;
}

View view_of(const dstd::String& gz) {
    return View::from_file(
        dstd::String(gz),
        dstd::String(dstd::String(determine_index_path(gz, ""))));
}

}  // namespace

TEST_SUITE("Row set arrows") {
    TEST_CASE("arrows filter, derive and group through row sets") {
        TestEnvironment env(10);
        const auto gz = write_trace(env);

        const auto rows = dftu_utils_test::ok(
            view_of(gz)
                .duql(R"(where fhash -> files.path == "/data/f10")"
                      " | select name, p = fhash -> files.path,"
                      " h = hhash -> hosts.name")
                .collect()
                .get());
        REQUIRE(rows.num_rows() == 2);
        for (std::int64_t r = 0; r < 2; ++r) {
            CHECK(bstr(rows, r, "p") == "/data/f10");
            CHECK(bstr(rows, r, "h") == "node0");
        }

        const auto grouped = dftu_utils_test::ok(
            view_of(gz)
                .duql(R"(where fhash -> files.path == "/data/f10")")
                .group_by({GroupKey::file_path()})
                .agg({{AggOp::Count, "", "n"}})
                .collect()
                .get());
        REQUIRE(grouped.num_rows() == 1);
        CHECK(bstr(grouped, 0, "file_path") == "/data/f10");
        CHECK(bnum(grouped, 0, "n") == 2);

        const auto exec = dftu_utils_test::ok(
            view_of(gz)
                .duql(R"(where exec_hash -> strings(shash).value like)"
                      R"( "%laghos%")")
                .collect()
                .get());
        REQUIRE(exec.num_rows() == 1);
        CHECK(bstr(exec, 0, "name") == "start");
    }

    TEST_CASE("a selective path pushes its keys and a wide pattern does not") {
        TestEnvironment env(10);
        const auto gz = write_trace(env);
        const dstd::String narrow =
            R"(where fhash -> files.path == "/data/f10")";
        CHECK(dftu_utils_test::ok(view_of(gz).explain_duql(narrow))
                  .find("fhash in (keys of files) (pushed)") !=
              dstd::String::npos);

        const dstd::String wide =
            R"(where fhash -> files.path like "/data/f%")";
        static_assert(FILES > dftracer::utils::index::plan::SEMI_JOIN_CAP);
        CHECK(dftu_utils_test::ok(view_of(gz).duql(wide).collect().get())
                  .num_rows() == EVENTS);
    }

    TEST_CASE("a resolved name fails and names the arrow") {
        TestEnvironment env(10);
        const auto gz = write_trace(env);
        const auto result =
            view_of(gz)
                .duql(R"(where resolved.fhash.path == "/data/f10")")
                .collect()
                .get();
        REQUIRE_FALSE(result.has_value());
        const dstd::String message(result.error().message());
        CAPTURE(message);
        CHECK(message.find("fhash -> files.path") != dstd::String::npos);
    }
}
