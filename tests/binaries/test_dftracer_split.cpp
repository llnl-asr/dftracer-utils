#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>
#include <zlib.h>

#include <cstdlib>
#include <fstream>
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

std::string find_split_binary() {
    const char* env_path = std::getenv("DFTRACER_SPLIT_PATH");
    if (env_path != nullptr && ::access(env_path, X_OK) == 0) return env_path;

    std::vector<std::string> candidates = {
        "./dftracer_split",         "../dftracer_split",
        "../../dftracer_split",     "../bin/dftracer_split",
        "../../bin/dftracer_split",
    };
    for (const auto& path : candidates) {
        if (::access(path.c_str(), X_OK) == 0) return path;
    }
    return "";
}

int run_split(const std::string& binary, const std::vector<std::string>& args) {
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

// Count non-empty lines across all .pfw.gz files in a directory by
// decompressing each with zlib.
int count_gz_lines_in_dir(const std::string& dir) {
    int total = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        std::string p = entry.path().string();
        if (p.size() < 7 || p.substr(p.size() - 7) != ".pfw.gz") continue;

        gzFile gz = gzopen(p.c_str(), "rb");
        if (!gz) continue;

        char buf[4096];
        while (gzgets(gz, buf, sizeof(buf)) != nullptr) {
            std::string line(buf);
            // strip trailing newline
            while (!line.empty() &&
                   (line.back() == '\n' || line.back() == '\r')) {
                line.pop_back();
            }
            if (!line.empty()) ++total;
        }
        gzclose(gz);
    }
    return total;
}

// Count .pfw.gz files in a directory.
int count_gz_files(const std::string& dir) {
    int n = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        std::string p = entry.path().string();
        if (p.size() >= 7 && p.substr(p.size() - 7) == ".pfw.gz") ++n;
    }
    return n;
}

}  // namespace

// ============================================================================
// Integration tests
// ============================================================================

TEST_SUITE("DFTracerSplit") {
    TEST_CASE("binary exists") {
        auto binary = find_split_binary();
        if (binary.empty()) {
            MESSAGE(
                "dftracer_split binary not found, skipping. "
                "Set DFTRACER_SPLIT_PATH env to specify location.");
            return;
        }
        CHECK(!binary.empty());
    }

    TEST_CASE("basic split produces output files") {
        auto binary = find_split_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_split binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 100, 0);
        REQUIRE(!f.empty());

        std::string out_dir = env.get_dir() + "/split_out";
        int rc = run_split(binary, {"-d", env.get_dir(), "-o", out_dir, "-f",
                                    "--disable-watchdog"});
        CHECK(rc == 0);
        REQUIRE(fs::exists(out_dir));
        CHECK(count_gz_files(out_dir) > 0);
    }

    TEST_CASE("split with verify flag") {
        auto binary = find_split_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_split binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 50, 0);
        REQUIRE(!f.empty());

        std::string out_dir = env.get_dir() + "/split_verify";
        int rc = run_split(binary, {"-d", env.get_dir(), "-o", out_dir, "-f",
                                    "--verify", "--disable-watchdog"});
        CHECK(rc == 0);
    }

    TEST_CASE("split preserves event lines") {
        auto binary = find_split_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_split binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        const int num_events = 80;
        auto f = create_pfw_gz(env, num_events, 0);
        REQUIRE(!f.empty());

        std::string out_dir = env.get_dir() + "/split_lines";
        int rc = run_split(binary, {"-d", env.get_dir(), "-o", out_dir, "-f",
                                    "--disable-watchdog"});
        CHECK(rc == 0);
        REQUIRE(fs::exists(out_dir));

        int total_lines = count_gz_lines_in_dir(out_dir);
        // Split output contains all events plus possible metadata lines.
        // Must have at least as many lines as events.
        CHECK(total_lines >= num_events);
    }

    TEST_CASE("split multiple input files") {
        auto binary = find_split_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_split binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        REQUIRE(!create_pfw_gz(env, 30, 0).empty());
        REQUIRE(!create_pfw_gz(env, 30, 1).empty());

        std::string out_dir = env.get_dir() + "/split_multi";
        int rc = run_split(binary, {"-d", env.get_dir(), "-o", out_dir, "-f",
                                    "--disable-watchdog"});
        CHECK(rc == 0);
        REQUIRE(fs::exists(out_dir));
        CHECK(count_gz_files(out_dir) > 0);
    }

    TEST_CASE("empty directory returns non-zero") {
        auto binary = find_split_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_split binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        std::string out_dir = env.get_dir() + "/split_empty";
        int rc = run_split(binary, {"-d", env.get_dir(), "-o", out_dir, "-f",
                                    "--disable-watchdog"});
        CHECK(rc != 0);
    }
}
