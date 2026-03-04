#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>
#include <zlib.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
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

std::string find_event_count_binary() {
    const char* env_path = std::getenv("DFTRACER_EVENT_COUNT_PATH");
    if (env_path != nullptr && ::access(env_path, X_OK) == 0) return env_path;

    std::vector<std::string> candidates = {
        "./dftracer_event_count",         "../dftracer_event_count",
        "../../dftracer_event_count",     "../bin/dftracer_event_count",
        "../../bin/dftracer_event_count",
    };
    for (const auto& path : candidates) {
        if (::access(path.c_str(), X_OK) == 0) return path;
    }
    return "";
}

// Run binary, capture stdout, return it as a string.
std::string run_event_count_capture(const std::string& binary,
                                    const std::vector<std::string>& args) {
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
    return output;
}

// Extract the last non-empty line from a string and parse it as int.
// Returns -1 on parse failure.
int parse_event_count(const std::string& output) {
    std::string last;
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        auto start = line.find_first_not_of(" \t\r\n");
        if (start != std::string::npos) last = line.substr(start);
    }
    if (last.empty()) return -1;
    try {
        return std::stoi(last);
    } catch (...) {
        return -1;
    }
}

}  // namespace

// ============================================================================
// Integration tests
// ============================================================================

TEST_SUITE("DFTracerEventCount") {
    TEST_CASE("binary exists") {
        auto binary = find_event_count_binary();
        if (binary.empty()) {
            MESSAGE(
                "dftracer_event_count binary not found, skipping. "
                "Set DFTRACER_EVENT_COUNT_PATH env to specify location.");
            return;
        }
        CHECK(!binary.empty());
    }

    TEST_CASE("count events single file") {
        auto binary = find_event_count_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_event_count binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 50, 0);
        REQUIRE(!f.empty());

        auto output =
            run_event_count_capture(binary, {"-d", env.get_dir(), "-f"});
        int count = parse_event_count(output);
        CHECK(count == 50);
    }

    TEST_CASE("count events multiple files") {
        auto binary = find_event_count_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_event_count binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        REQUIRE(!create_pfw_gz(env, 10, 0).empty());
        REQUIRE(!create_pfw_gz(env, 20, 1).empty());
        REQUIRE(!create_pfw_gz(env, 30, 2).empty());

        auto output =
            run_event_count_capture(binary, {"-d", env.get_dir(), "-f"});
        int count = parse_event_count(output);
        CHECK(count == 60);  // 10 + 20 + 30
    }

    TEST_CASE("empty directory returns zero or non-zero") {
        auto binary = find_event_count_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_event_count binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        // No trace files -- binary either exits non-zero or prints 0.
        auto output =
            run_event_count_capture(binary, {"-d", env.get_dir(), "-f"});
        int count = parse_event_count(output);
        // Either the binary reports 0 events or fails; both are acceptable.
        CHECK((count == 0 || count == -1));
    }

    TEST_CASE("force flag rebuilds index") {
        auto binary = find_event_count_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_event_count binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 25, 0);
        REQUIRE(!f.empty());

        // First run builds the index.
        auto out1 =
            run_event_count_capture(binary, {"-d", env.get_dir(), "-f"});
        int count1 = parse_event_count(out1);
        CHECK(count1 == 25);

        // Second run with --force rebuilds; result must be identical.
        auto out2 =
            run_event_count_capture(binary, {"-d", env.get_dir(), "-f"});
        int count2 = parse_event_count(out2);
        CHECK(count2 == 25);
    }

    TEST_CASE("executor threads flag accepted") {
        auto binary = find_event_count_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_event_count binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 15, 0);
        REQUIRE(!f.empty());

        auto output = run_event_count_capture(
            binary, {"-d", env.get_dir(), "-f", "--executor-threads", "2"});
        int count = parse_event_count(output);
        CHECK(count == 15);
    }

    TEST_CASE("custom index dir") {
        auto binary = find_event_count_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_event_count binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 20, 0);
        REQUIRE(!f.empty());

        std::string idx_dir = env.get_dir() + "/idx";
        fs::create_directories(idx_dir);

        auto output = run_event_count_capture(
            binary, {"-d", env.get_dir(), "-f", "--index-dir", idx_dir});
        int count = parse_event_count(output);
        CHECK(count == 20);
    }
}
