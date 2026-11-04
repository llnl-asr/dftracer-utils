// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/time_metric_event.h>
#include <dftracer/utils/trace/trace_config.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>
#include <refused_lines.h>
#include <testing_utilities.h>

#include <fstream>

using namespace dftracer::utils;

using namespace dftracer::utils::trace;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;

namespace {

dstd::String complete(int i) {
    return R"({"name":"read","cat":"POSIX","type":3,"pid":1,"tid":1,"ts":)" +
           dstd::to_string(1000 + i) +
           R"(,"dur":5,"ph":1,"args":{"hhash":"h"}})";
}

dstd::String end_event(std::uint64_t pid, bool with_args) {
    dstd::String s =
        R"({"id":99,"name":"end","cat":"dftracer","type":1,"pid":)" +
        dstd::to_string(pid) + R"(,"tid":)" + dstd::to_string(pid) +
        R"(,"ts":9000,"dur":0,"ph":1)";
    if (with_args)
        s += R"(,"args":{"hhash":"h","num_events":41,"cfg":{"compression":1},)"
             R"("used":{"MPI":1},"app":{"model":"resnet50"}}})";
    else
        s += R"(})";
    return s;
}

dstd::String make_gz(TestEnvironment& env, const dstd::String& body) {
    dstd::String pfw = env.dir() + "/tc.pfw";
    {
        dftu_utils_test::FileOut(pfw) << body;
    }
    dstd::String gz = pfw + ".gz";
    compress_file_to_gzip_multimember(pfw, gz, 512);
    (void)dstd::fs::remove(pfw);
    return gz;
}

}  // namespace

TEST_SUITE("trace-config") {
    TEST_CASE("reads end-event config from the last member") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String body;
        for (int i = 0; i < 40; ++i) body += complete(i) + "\n";
        body += end_event(1, true) + "\n";

        auto cfgs = read_trace_config(dstd::String(make_gz(env, body)));
        REQUIRE(cfgs.size() == 1);
        CHECK(cfgs[0].pid == 1);
        CHECK(cfgs[0].args["num_events"].get<std::uint64_t>() == 41);
        CHECK(cfgs[0].args["cfg.compression"].get<std::uint64_t>() == 1);
        CHECK(cfgs[0].args["used.MPI"].get<std::uint64_t>() == 1);
        CHECK(cfgs[0].args["app.model"].get<dstd::StringView>() == "resnet50");
    }

    TEST_CASE("multiple end events -> one config per process") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String body;
        for (int i = 0; i < 20; ++i) body += complete(i) + "\n";
        body += end_event(1, true) + "\n";
        body += end_event(2, true) + "\n";

        auto cfgs = read_trace_config(dstd::String(make_gz(env, body)));
        REQUIRE(cfgs.size() == 2);
        CHECK(cfgs[0].pid == 1);
        CHECK(cfgs[1].pid == 2);
    }

    TEST_CASE("end without args -> entry present, args empty") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String body;
        for (int i = 0; i < 20; ++i) body += complete(i) + "\n";
        body += end_event(7, false) + "\n";

        auto cfgs = read_trace_config(dstd::String(make_gz(env, body)));
        REQUIRE(cfgs.size() == 1);
        CHECK(cfgs[0].pid == 7);
        CHECK_FALSE(cfgs[0].args.exists());
    }

    TEST_CASE("no end event -> empty (caller falls back)") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String body;
        for (int i = 0; i < 40; ++i) body += complete(i) + "\n";

        CHECK(read_trace_config(dstd::String(make_gz(env, body))).empty());
    }

    // View::config() tail path: end in the last member, no index needed.
    TEST_CASE("View::config reads end from the tail") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String body;
        for (int i = 0; i < 40; ++i) body += complete(i) + "\n";
        body += end_event(1, true) + "\n";
        dstd::String gz = make_gz(env, body);

        auto cfgs =
            View::from_file(
                dstd::String(gz),
                dstd::String(dstd::String(determine_index_path(gz, ""))))
                .config();
        REQUIRE(cfgs.size() == 1);
        CHECK(cfgs[0].args["app.model"].get<dstd::StringView>() == "resnet50");
    }

    // Scattered end (not in the last member): tail probe misses it, so
    // View::config falls back to a bloom-pruned scan over the index.
    TEST_CASE("View::config finds a scattered end via the index") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String body;
        for (int i = 0; i < 20; ++i) body += complete(i) + "\n";
        body += end_event(5, true) + "\n";
        for (int i = 20; i < 60; ++i) body += complete(i) + "\n";  // end buried
        dstd::String gz = make_gz(env, body);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));

        REQUIRE(read_trace_config(dstd::String(gz))
                    .empty());  // tail probe alone misses it
        auto cfgs =
            View::from_file(dstd::String(gz), dstd::String(idx)).config();
        REQUIRE(cfgs.size() == 1);
        CHECK(cfgs[0].pid == 5);
        CHECK(cfgs[0].args["num_events"].get<std::uint64_t>() == 41);
    }

    TEST_CASE("an end event and a time metric the DOM parser refuses") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        for (int k = 0; k < REFUSED_KINDS; ++k) {
            INFO("kind: ", k);
            dstd::String body;
            const dstd::String member = refused_member(k);
            body += R"({"name":"CM","cat":"dftracer","pid":1,"tid":1,"ts":1,)"
                    R"("ph":"M","args":{"name":"time_metric","value":"NS",)" +
                    member + "}}\n";
            for (int i = 0; i < 20; ++i) body += complete(i) + "\n";
            body += R"({"id":99,"name":"end","cat":"dftracer","pid":1,)"
                    R"("tid":1,"ts":9000,"dur":0,"ph":1,"args":{"hhash":"h",)"
                    R"("num_events":41,"cfg":{"compression":1},)" +
                    member + "}}\n";
            const dstd::String gz = make_gz(env, body);
            CHECK(read_time_metric(gz) == TimeMetric::Ns);
            auto cfgs = read_trace_config(gz);
            REQUIRE(cfgs.size() == 1);
            CHECK(cfgs[0].pid == 1);
            CHECK(cfgs[0].args["num_events"].get<std::uint64_t>() == 41);
            CHECK(cfgs[0].args["cfg.compression"].get<std::uint64_t>() == 1);
        }
    }
}
