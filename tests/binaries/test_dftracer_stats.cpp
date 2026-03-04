#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>
#include <zlib.h>

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

std::string find_stats_binary() {
    const char* env_path = std::getenv("DFTRACER_STATS_PATH");
    if (env_path != nullptr && ::access(env_path, X_OK) == 0) return env_path;

    std::vector<std::string> candidates = {
        "./dftracer_stats",         "../dftracer_stats",
        "../../dftracer_stats",     "../bin/dftracer_stats",
        "../../bin/dftracer_stats",
    };
    for (const auto& path : candidates) {
        if (::access(path.c_str(), X_OK) == 0) return path;
    }
    return "";
}

int run_stats(const std::string& binary, const std::vector<std::string>& args) {
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
    return -1;
}

std::string run_stats_capture(const std::string& binary,
                              const std::vector<std::string>& args,
                              int* exit_code = nullptr) {
    int pipefd[2];
    if (::pipe(pipefd) < 0) return "";

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return "";
    }
    if (pid == 0) {
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[1]);
        std::vector<const char*> argv;
        argv.push_back(binary.c_str());
        for (const auto& arg : args) argv.push_back(arg.c_str());
        argv.push_back(nullptr);
        ::execv(binary.c_str(), const_cast<char* const*>(argv.data()));
        ::_exit(127);
    }
    ::close(pipefd[1]);
    std::string output;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0)
        output.append(buf, static_cast<std::size_t>(n));
    ::close(pipefd[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (exit_code) *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return output;
}

}  // namespace

// ============================================================================
// Integration tests
// ============================================================================

TEST_SUITE("DFTracerStats") {
    TEST_CASE("binary exists") {
        auto binary = find_stats_binary();
        if (binary.empty()) {
            MESSAGE(
                "dftracer_stats binary not found, skipping. "
                "Set DFTRACER_STATS_PATH env to specify location.");
            return;
        }
        CHECK(!binary.empty());
    }

    TEST_CASE("basic stats from directory") {
        auto binary = find_stats_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_stats binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 100, 0);
        REQUIRE(!f.empty());

        // dftracer_stats auto-builds the .bidx when missing.
        int rc = 0;
        auto output = run_stats_capture(binary, {"-d", env.get_dir()}, &rc);
        CHECK(rc == 0);
        // Text summary always prints "Total Events:".
        CHECK(output.find("Total Events") != std::string::npos);
    }

    TEST_CASE("json output") {
        auto binary = find_stats_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_stats binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 50, 0);
        REQUIRE(!f.empty());

        int rc = 0;
        auto output =
            run_stats_capture(binary, {"-d", env.get_dir(), "--json"}, &rc);
        CHECK(rc == 0);
        // JSON output is a JSON array; each element is an object.
        CHECK(output.find('{') != std::string::npos);
        // The JSON object always contains "total_events".
        CHECK(output.find("total_events") != std::string::npos);
    }

    TEST_CASE("multiple files") {
        auto binary = find_stats_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_stats binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        for (int i = 0; i < 3; ++i) {
            auto f = create_pfw_gz(env, 30, i);
            REQUIRE(!f.empty());
        }

        int rc = run_stats(binary, {"-d", env.get_dir()});
        CHECK(rc == 0);
    }
}
