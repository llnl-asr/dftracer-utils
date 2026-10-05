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
std::string create_plain_pfw(dftu_utils_test::TestEnvironment& env,
                             int num_events, int id) {
    auto trace = env.create_dft_test_file(num_events);
    if (trace.empty()) return "";

    std::string pfw_path =
        env.get_dir() + "/trace_" + std::to_string(id) + ".pfw";
    fs::rename(trace, pfw_path);
    return pfw_path;
}

std::string find_pgzip_binary() {
    return dftu_utils_test::find_binary_by_name("DFTRACER_PGZIP_PATH",
                                                "dftracer_pgzip");
}

int run_pgzip(const std::string& binary, const std::vector<std::string>& args) {
    return dftu_utils_test::run_process(binary, args);
}

std::string gunzip_all(const std::string& path) {
    gzFile gz = gzopen(path.c_str(), "rb");
    std::string out;
    if (!gz) return out;
    char b[65536];
    int n;
    while ((n = gzread(gz, b, sizeof b)) > 0) out.append(b, n);
    gzclose(gz);
    return out;
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

        dftu_utils_test::TestEnvironment env(100);
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

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto pfw = create_plain_pfw(env, 10, 0);
        REQUIRE(!pfw.empty());

        int rc = run_pgzip(binary, {"-d", env.get_dir(), "--disable-watchdog"});
        CHECK(rc == 0);

        std::string gz_path = pfw + ".gz";
        REQUIRE(fs::exists(gz_path));

        auto first = dftu_utils_test::gz_first_line(gz_path);
        REQUIRE(!first.empty());
        CHECK(first.front() == '[');
    }

    TEST_CASE("compression level 1 (fast) succeeds") {
        auto binary = find_pgzip_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_pgzip binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
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

        dftu_utils_test::TestEnvironment env(100);
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

        dftu_utils_test::TestEnvironment env(100);
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

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        // No .pfw files -- binary treats this as success (nothing to do).
        int rc = run_pgzip(binary, {"-d", env.get_dir(), "--disable-watchdog"});
        CHECK(rc == 0);
    }

    TEST_CASE("last line without trailing newline round trips") {
        auto binary = find_pgzip_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_pgzip binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        std::string pfw = env.get_dir() + "/tail.pfw";
        std::string content;
        for (int i = 0; i < 2000; ++i)
            content += "{\"id\":" + std::to_string(i) + ",\"name\":\"e\"}\n";
        content += "{\"id\":-1,\"name\":\"no newline\"}";
        {
            std::ofstream f(pfw, std::ios::binary);
            f << content;
        }

        int rc = run_pgzip(binary, {"-d", env.get_dir(), "--chunk-size", "4KB",
                                    "--disable-watchdog"});
        CHECK(rc == 0);
        CHECK(gunzip_all(pfw + ".gz") == content);
        CHECK(!fs::exists(pfw));
    }

    TEST_CASE("empty file fails and is left in place") {
        auto binary = find_pgzip_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_pgzip binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        std::string pfw = env.get_dir() + "/empty.pfw";
        {
            std::ofstream f(pfw, std::ios::binary);
        }

        int rc = run_pgzip(binary, {"-d", env.get_dir(), "--disable-watchdog"});
        CHECK(rc != 0);
        CHECK(fs::exists(pfw));
        CHECK(!fs::exists(pfw + ".gz"));
    }
}
