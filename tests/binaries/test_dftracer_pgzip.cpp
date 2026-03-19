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

// Create a plain (uncompressed) .pfw file with DFTracer events.
std::string create_plain_pfw(dft_utils_test::TestEnvironment& env,
                             int num_events, int id) {
    auto trace = env.create_dft_test_file(num_events);
    if (trace.empty()) return "";

    std::string pfw_path =
        env.get_dir() + "/trace_" + std::to_string(id) + ".pfw";
    fs::rename(trace, pfw_path);
    return pfw_path;
}

std::string find_pgzip_binary() {
    const char* env_path = std::getenv("DFTRACER_PGZIP_PATH");
    if (env_path != nullptr && ::access(env_path, X_OK) == 0) return env_path;

    std::vector<std::string> candidates = {
        "./dftracer_pgzip",         "../dftracer_pgzip",
        "../../dftracer_pgzip",     "../bin/dftracer_pgzip",
        "../../bin/dftracer_pgzip",
    };
    for (const auto& path : candidates) {
        if (::access(path.c_str(), X_OK) == 0) return path;
    }
    return "";
}

int run_pgzip(const std::string& binary, const std::vector<std::string>& args) {
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

// Read the first non-empty line from a gzip file.
std::string gz_first_line(const std::string& gz_path) {
    gzFile gz = gzopen(gz_path.c_str(), "rb");
    if (!gz) return "";

    char buf[4096];
    std::string result;
    while (gzgets(gz, buf, sizeof(buf)) != nullptr) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        if (!line.empty()) {
            result = line;
            break;
        }
    }
    gzclose(gz);
    return result;
}

}  // namespace

// ============================================================================
// Integration tests
// ============================================================================

TEST_SUITE("DFTracerPgzip") {
    TEST_CASE("binary exists") {
        auto binary = find_pgzip_binary();
        if (binary.empty()) {
            MESSAGE(
                "dftracer_pgzip binary not found, skipping. "
                "Set DFTRACER_PGZIP_PATH env to specify location.");
            return;
        }
        CHECK(!binary.empty());
    }

    TEST_CASE("compress single file") {
        auto binary = find_pgzip_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_pgzip binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto pfw = create_plain_pfw(env, 50, 0);
        REQUIRE(!pfw.empty());

        int rc = run_pgzip(binary, {"-d", env.get_dir(), "--disable-watchdog"});
        CHECK(rc == 0);

        std::string gz_path = pfw + ".gz";
        CHECK(fs::exists(gz_path));
        // Original plain file must be removed after successful compression.
        CHECK(!fs::exists(pfw));
    }

    TEST_CASE("compressed file is valid gzip with JSON content") {
        auto binary = find_pgzip_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_pgzip binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto pfw = create_plain_pfw(env, 10, 0);
        REQUIRE(!pfw.empty());

        int rc = run_pgzip(binary, {"-d", env.get_dir(), "--disable-watchdog"});
        CHECK(rc == 0);

        std::string gz_path = pfw + ".gz";
        REQUIRE(fs::exists(gz_path));

        auto first = gz_first_line(gz_path);
        REQUIRE(!first.empty());
        CHECK(first.front() == '[');
    }

    TEST_CASE("compression level 1 (fast) succeeds") {
        auto binary = find_pgzip_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_pgzip binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto pfw = create_plain_pfw(env, 20, 0);
        REQUIRE(!pfw.empty());

        int rc = run_pgzip(
            binary, {"-d", env.get_dir(), "-l", "1", "--disable-watchdog"});
        CHECK(rc == 0);
        CHECK(fs::exists(pfw + ".gz"));
    }

    TEST_CASE("compression level 9 (best) succeeds") {
        auto binary = find_pgzip_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_pgzip binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto pfw = create_plain_pfw(env, 20, 0);
        REQUIRE(!pfw.empty());

        int rc = run_pgzip(
            binary, {"-d", env.get_dir(), "-l", "9", "--disable-watchdog"});
        CHECK(rc == 0);
        CHECK(fs::exists(pfw + ".gz"));
    }

    TEST_CASE("compress multiple files") {
        auto binary = find_pgzip_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_pgzip binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto pfw0 = create_plain_pfw(env, 15, 0);
        auto pfw1 = create_plain_pfw(env, 15, 1);
        REQUIRE(!pfw0.empty());
        REQUIRE(!pfw1.empty());

        int rc = run_pgzip(binary, {"-d", env.get_dir(), "--disable-watchdog"});
        CHECK(rc == 0);
        CHECK(fs::exists(pfw0 + ".gz"));
        CHECK(fs::exists(pfw1 + ".gz"));
        CHECK(!fs::exists(pfw0));
        CHECK(!fs::exists(pfw1));
    }

    TEST_CASE("empty directory succeeds with nothing to do") {
        auto binary = find_pgzip_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_pgzip binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        // No .pfw files -- binary treats this as success (nothing to do).
        int rc = run_pgzip(binary, {"-d", env.get_dir(), "--disable-watchdog"});
        CHECK(rc == 0);
    }
}
