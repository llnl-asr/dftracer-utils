#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/trace_config.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <fstream>
#include <string>

using namespace dftracer::utils::trace;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;

namespace {

std::string complete(int i) {
    return R"({"name":"read","cat":"POSIX","type":3,"pid":1,"tid":1,"ts":)" +
           std::to_string(1000 + i) +
           R"(,"dur":5,"ph":1,"args":{"hhash":"h"}})";
}

std::string end_event(std::uint64_t pid, bool with_args) {
    std::string s =
        R"({"id":99,"name":"end","cat":"dftracer","type":1,"pid":)" +
        std::to_string(pid) + R"(,"tid":)" + std::to_string(pid) +
        R"(,"ts":9000,"dur":0,"ph":1)";
    if (with_args)
        s += R"(,"args":{"hhash":"h","num_events":41,"cfg":{"compression":1},)"
             R"("used":{"MPI":1},"app":{"model":"resnet50"}}})";
    else
        s += R"(})";
    return s;
}

std::string make_gz(TestEnvironment& env, const std::string& body) {
    std::string pfw = env.get_dir() + "/tc.pfw";
    {
        std::ofstream(pfw) << body;
    }
    std::string gz = pfw + ".gz";
    compress_file_to_gzip_multimember(pfw, gz, 512);
    fs::remove(pfw);
    return gz;
}

}  // namespace

TEST_SUITE("trace-config") {
    TEST_CASE("reads end-event config from the last member") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string body;
        for (int i = 0; i < 40; ++i) body += complete(i) + "\n";
        body += end_event(1, true) + "\n";

        auto cfgs = read_trace_config(make_gz(env, body));
        REQUIRE(cfgs.size() == 1);
        CHECK(cfgs[0].pid == 1);
        CHECK(cfgs[0].args["num_events"].get<std::uint64_t>() == 41);
        CHECK(cfgs[0].args["cfg.compression"].get<std::uint64_t>() == 1);
        CHECK(cfgs[0].args["used.MPI"].get<std::uint64_t>() == 1);
        CHECK(cfgs[0].args["app.model"].get<std::string_view>() == "resnet50");
    }

    TEST_CASE("multiple end events -> one config per process") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string body;
        for (int i = 0; i < 20; ++i) body += complete(i) + "\n";
        body += end_event(1, true) + "\n";
        body += end_event(2, true) + "\n";

        auto cfgs = read_trace_config(make_gz(env, body));
        REQUIRE(cfgs.size() == 2);
        CHECK(cfgs[0].pid == 1);
        CHECK(cfgs[1].pid == 2);
    }

    TEST_CASE("end without args -> entry present, args empty") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string body;
        for (int i = 0; i < 20; ++i) body += complete(i) + "\n";
        body += end_event(7, false) + "\n";

        auto cfgs = read_trace_config(make_gz(env, body));
        REQUIRE(cfgs.size() == 1);
        CHECK(cfgs[0].pid == 7);
        CHECK_FALSE(cfgs[0].args.exists());
    }

    TEST_CASE("no end event -> empty (caller falls back)") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string body;
        for (int i = 0; i < 40; ++i) body += complete(i) + "\n";

        CHECK(read_trace_config(make_gz(env, body)).empty());
    }

    // View::config() tail path: end in the last member, no index needed.
    TEST_CASE("View::config reads end from the tail") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string body;
        for (int i = 0; i < 40; ++i) body += complete(i) + "\n";
        body += end_event(1, true) + "\n";
        std::string gz = make_gz(env, body);

        auto cfgs = View::from_file(gz, determine_index_path(gz, "")).config();
        REQUIRE(cfgs.size() == 1);
        CHECK(cfgs[0].args["app.model"].get<std::string_view>() == "resnet50");
    }

    // Scattered end (not in the last member): tail probe misses it, so
    // View::config falls back to a bloom-pruned scan over the index.
    TEST_CASE("View::config finds a scattered end via the index") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string body;
        for (int i = 0; i < 20; ++i) body += complete(i) + "\n";
        body += end_event(5, true) + "\n";
        for (int i = 20; i < 60; ++i) body += complete(i) + "\n";  // end buried
        std::string gz = make_gz(env, body);
        std::string idx = determine_index_path(gz, "");

        REQUIRE(read_trace_config(gz).empty());  // tail probe alone misses it
        auto cfgs = View::from_file(gz, idx).config();
        REQUIRE(cfgs.size() == 1);
        CHECK(cfgs[0].pid == 5);
        CHECK(cfgs[0].args["num_events"].get<std::uint64_t>() == 41);
    }
}
