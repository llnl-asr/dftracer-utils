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

std::string find_aggregator_binary() {
    const char* env_path = std::getenv("DFTRACER_AGGREGATOR_PATH");
    if (env_path != nullptr && ::access(env_path, X_OK) == 0) return env_path;

    std::vector<std::string> candidates = {
        "./dftracer_aggregator",         "../dftracer_aggregator",
        "../../dftracer_aggregator",     "../bin/dftracer_aggregator",
        "../../bin/dftracer_aggregator",
    };
    for (const auto& path : candidates) {
        if (::access(path.c_str(), X_OK) == 0) return path;
    }
    return "";
}

int run_aggregator(const std::string& binary,
                   const std::vector<std::string>& args) {
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

// Read first non-empty byte from a file to check if it starts with '{' or '['.
char first_byte(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return '\0';
    char c = '\0';
    while (ifs.get(c)) {
        if (c != ' ' && c != '\n' && c != '\r' && c != '\t') return c;
    }
    return '\0';
}

// Read first non-empty byte from a gzip file.
char gz_first_byte(const std::string& gz_path) {
    gzFile gz = gzopen(gz_path.c_str(), "rb");
    if (!gz) return '\0';
    char buf[4096];
    char result = '\0';
    while (gzgets(gz, buf, sizeof(buf)) != nullptr) {
        for (int i = 0; buf[i] != '\0'; ++i) {
            if (buf[i] != ' ' && buf[i] != '\n' && buf[i] != '\r' &&
                buf[i] != '\t') {
                result = buf[i];
                goto done;
            }
        }
    }
done:
    gzclose(gz);
    return result;
}

}  // namespace

// ============================================================================
// Integration tests
// ============================================================================

TEST_SUITE("DFTracerAggregator") {
    TEST_CASE("binary exists") {
        auto binary = find_aggregator_binary();
        if (binary.empty()) {
            MESSAGE(
                "dftracer_aggregator binary not found, skipping. "
                "Set DFTRACER_AGGREGATOR_PATH env to specify location.");
            return;
        }
        CHECK(!binary.empty());
    }

    TEST_CASE("basic aggregation") {
        auto binary = find_aggregator_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_aggregator binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 100, 0);
        REQUIRE(!f.empty());

        std::string output = env.get_dir() + "/agg_output.json";
        int rc = run_aggregator(binary, {"-d", env.get_dir(), "-o", output});
        CHECK(rc == 0);
        REQUIRE(fs::exists(output));

        char c = first_byte(output);
        CHECK((c == '{' || c == '['));
    }

    TEST_CASE("aggregation with time interval") {
        auto binary = find_aggregator_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_aggregator binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 50, 0);
        REQUIRE(!f.empty());

        std::string output = env.get_dir() + "/agg_interval.json";
        int rc = run_aggregator(
            binary, {"-d", env.get_dir(), "-o", output, "-t", "1.0"});
        CHECK(rc == 0);
    }

    TEST_CASE("compressed output") {
        auto binary = find_aggregator_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_aggregator binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 50, 0);
        REQUIRE(!f.empty());

        std::string output = env.get_dir() + "/agg_compressed.json";
        std::string output_gz = output + ".gz";
        int rc = run_aggregator(
            binary, {"-d", env.get_dir(), "-o", output, "--compress"});
        CHECK(rc == 0);
        REQUIRE(fs::exists(output_gz));

        char c = gz_first_byte(output_gz);
        CHECK((c == '{' || c == '['));
    }

    TEST_CASE("multiple files") {
        auto binary = find_aggregator_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_aggregator binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        for (int i = 0; i < 3; ++i) {
            auto f = create_pfw_gz(env, 50, i);
            REQUIRE(!f.empty());
        }

        std::string output = env.get_dir() + "/agg_multi.json";
        int rc = run_aggregator(binary, {"-d", env.get_dir(), "-o", output});
        CHECK(rc == 0);
        REQUIRE(fs::exists(output));
    }
}
