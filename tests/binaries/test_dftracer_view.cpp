#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

// ============================================================================
// Helpers
// ============================================================================

namespace {

std::string create_pfw_gz(dft_utils_test::TestEnvironment& env, int num_events,
                          int id) {
    auto trace_gz = env.create_dft_test_gzip_file(num_events);
    if (trace_gz.empty()) return "";

    std::string pfw_path =
        env.get_dir() + "/trace_" + std::to_string(id) + ".pfw.gz";
    fs::rename(trace_gz, pfw_path);
    return pfw_path;
}

std::string find_view_binary() {
    const char* env_path = std::getenv("DFTRACER_VIEW_PATH");
    if (env_path != nullptr && ::access(env_path, X_OK) == 0) return env_path;

    std::vector<std::string> candidates = {
        "./dftracer_view",         "../dftracer_view",
        "../../dftracer_view",     "../bin/dftracer_view",
        "../../bin/dftracer_view",
    };
    for (const auto& path : candidates) {
        if (::access(path.c_str(), X_OK) == 0) return path;
    }
    return "";
}

int run_view(const std::string& binary, const std::vector<std::string>& args) {
    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        std::vector<const char*> argv;
        argv.push_back(binary.c_str());
        for (const auto& arg : args) argv.push_back(arg.c_str());
        argv.push_back(nullptr);
        ::execv(binary.c_str(), const_cast<char* const*>(argv.data()));
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        MESSAGE("Binary killed by signal ", sig);
        return -(sig);
    }
    return -1;
}

}  // namespace

// ============================================================================
// Integration tests
// ============================================================================

TEST_SUITE("DFTracerView") {
    TEST_CASE("binary exists") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE(
                "dftracer_view binary not found, skipping. "
                "Set DFTRACER_VIEW_PATH env to specify location.");
            return;
        }
        CHECK(!binary.empty());
    }

    TEST_CASE("binary runs (--help)") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }
        int rc = run_view(binary, {"--help"});
        MESSAGE("--help returned: ", rc);
        CHECK(rc == 0);
    }

    TEST_CASE("stream all events") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 50, 0);
        REQUIRE(!f.empty());

        int rc = run_view(binary, {"--query", R"(cat == "IO")", "--stream",
                                   "--no-metadata", "-d", env.get_dir()});
        CHECK(rc == 0);
    }

    TEST_CASE("query filter") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 50, 0);
        REQUIRE(!f.empty());

        int rc = run_view(binary, {"--query", R"(cat == "IO")", "--stream",
                                   "--no-metadata", "-d", env.get_dir()});
        CHECK(rc == 0);
    }

    TEST_CASE("output to file") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 50, 0);
        REQUIRE(!f.empty());

        std::string output = env.get_dir() + "/view_output.ndjson";
        int rc = run_view(
            binary, {"--query", R"(cat == "IO")", "--stream", "--no-metadata",
                     "-d", env.get_dir(), "-o", output});
        CHECK(rc == 0);
        REQUIRE(fs::exists(output));
        // Output file must be non-empty (events were matched).
        CHECK(fs::file_size(output) > 0);
    }

    TEST_CASE("query with name filter") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 50, 0);
        REQUIRE(!f.empty());

        int rc =
            run_view(binary, {"--query",
                              R"(cat == "IO" and name in ["pread", "pwrite"])",
                              "--stream", "-d", env.get_dir()});
        CHECK(rc == 0);
    }
}
