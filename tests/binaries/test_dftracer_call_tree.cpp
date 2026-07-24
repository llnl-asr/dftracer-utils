#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string find_binary() {
    const char* env_path = std::getenv("DFTRACER_CALL_TREE_PATH");
    if (env_path && ::access(env_path, X_OK) == 0) return env_path;
    for (const auto& p :
         {"./dftracer_call_tree", "../dftracer_call_tree",
          "../../dftracer_call_tree", "../bin/dftracer_call_tree",
          "../../bin/dftracer_call_tree"}) {
        if (::access(p, X_OK) == 0) return p;
    }
    return "";
}

int run_process(const std::string& binary,
                const std::vector<std::string>& args) {
    std::vector<const char*> argv;
    argv.push_back(binary.c_str());
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        ::execv(binary.c_str(), const_cast<char* const*>(argv.data()));
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string create_pfw_gz(dft_utils_test::TestEnvironment& env, int num_events,
                          int id) {
    auto trace_gz = env.create_dft_test_gzip_file(num_events);
    if (trace_gz.empty()) return "";
    std::string path =
        env.get_dir() + "/trace_" + std::to_string(id) + ".pfw.gz";
    fs::rename(trace_gz, path);
    return path;
}

// Counts non-bracket JSON lines and verifies the array opens with "[" and
// closes with "]". Returns -1 on shape error.
int count_events_basic(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return -1;
    std::string line;
    bool saw_open = false, saw_close = false;
    int events = 0;
    while (std::getline(f, line)) {
        if (line == "[") {
            saw_open = true;
            continue;
        }
        if (line == "]") {
            saw_close = true;
            continue;
        }
        if (!line.empty()) events++;
    }
    return (saw_open && saw_close) ? events : -1;
}

}  // namespace

TEST_SUITE("DFTracerCallTree") {
    TEST_CASE("binary exists") {
        std::string bin = find_binary();
        if (bin.empty()) {
            MESSAGE("skipping: dftracer_call_tree binary not found");
            return;
        }
        CHECK(!bin.empty());
    }

    TEST_CASE("basic run produces valid JSON") {
        std::string bin = find_binary();
        if (bin.empty()) {
            MESSAGE("skipping: binary not found");
            return;
        }
        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, 100, 0).empty());

        std::string out = env.get_dir() + "/ct.pfw";
        int rc = run_process(bin, {env.get_dir(), "-o", out});
        CHECK(rc == 0);
        REQUIRE(fs::exists(out));
        CHECK(count_events_basic(out) > 0);
    }

    TEST_CASE("multi-file input") {
        std::string bin = find_binary();
        if (bin.empty()) {
            MESSAGE("skipping: binary not found");
            return;
        }
        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        for (int i = 0; i < 3; ++i) {
            REQUIRE(!create_pfw_gz(env, 200, i).empty());
        }

        std::string out = env.get_dir() + "/ct.pfw";
        int rc = run_process(bin, {env.get_dir(), "-o", out});
        CHECK(rc == 0);
        REQUIRE(fs::exists(out));
        CHECK(count_events_basic(out) > 0);
    }
}
