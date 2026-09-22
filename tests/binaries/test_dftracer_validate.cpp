#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

// ============================================================================
// Helpers
// ============================================================================

namespace {

// Move a freshly generated valid pfw.gz into `dir`.
std::string make_valid_pfw(dftu_utils_test::TestEnvironment& env,
                           const fs::path& dir, int num_events, int id) {
    auto gz = env.create_dft_test_gzip_file(num_events);
    if (gz.empty()) return "";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path dest = dir / ("trace_" + std::to_string(id) + ".pfw.gz");
    fs::rename(gz, dest, ec);
    if (ec) return "";
    return dest.string();
}

// Write a plain .pfw whose second event line is not valid JSON.
std::string make_bad_pfw(const fs::path& dir, int id) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path p = dir / ("bad_" + std::to_string(id) + ".pfw");
    std::ofstream ofs(p);
    if (!ofs) return "";
    ofs << "[\n";
    ofs << "{\"name\":\"ok\",\"cat\":\"POSIX\",\"pid\":1,\"tid\":1,\"ts\":1,"
           "\"dur\":1,\"ph\":\"X\"}\n";
    ofs << "{\"name\": this is not valid json\n";
    return p.string();
}

std::string find_validate_binary() {
    return dftu_utils_test::find_binary_by_name("DFTRACER_VALIDATE_PATH",
                                                "dftracer_validate");
}

// Run the binary, capture stdout, and report the exit code via *rc.
std::string run_validate_capture(const std::string& binary,
                                 const std::vector<std::string>& args,
                                 int* rc) {
    return dftu_utils_test::run_process_capture(binary, args, false, rc);
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ============================================================================
// Integration tests
// ============================================================================

TEST_SUITE("dftracer_validate") {
    TEST_CASE("binary exists") {
        auto binary = find_validate_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_validate binary not found, skipping.");
            return;
        }
        CHECK(!binary.empty());
    }

    TEST_CASE("valid files pass") {
        auto binary = find_validate_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_validate binary not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        fs::path work = fs::path(env.get_dir()) / "valid";
        REQUIRE(!make_valid_pfw(env, work, 30, 0).empty());
        REQUIRE(!make_valid_pfw(env, work, 20, 1).empty());

        int rc = -1;
        auto out = run_validate_capture(binary, {"-d", work.string()}, &rc);
        CHECK(rc == 0);
        CHECK(contains(out, "2 passed"));
        CHECK(contains(out, "0 failed"));
    }

    TEST_CASE("recursive parallel scan finds nested files") {
        auto binary = find_validate_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_validate binary not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        fs::path root = fs::path(env.get_dir()) / "nested";
        REQUIRE(!make_valid_pfw(env, root / "a", 10, 0).empty());
        REQUIRE(!make_valid_pfw(env, root / "b" / "c", 10, 1).empty());

        int rc = -1;
        auto out = run_validate_capture(binary, {"-d", root.string()}, &rc);
        CHECK(rc == 0);
        CHECK(contains(out, "2 passed"));
    }

    TEST_CASE("malformed file fails") {
        auto binary = find_validate_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_validate binary not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(1);
        REQUIRE(env.is_valid());
        auto bad = make_bad_pfw(fs::path(env.get_dir()) / "bad", 0);
        REQUIRE(!bad.empty());

        int rc = -1;
        auto out = run_validate_capture(binary, {"--files", bad}, &rc);
        CHECK(rc == 1);
        CHECK(contains(out, "1 failed"));
    }

    TEST_CASE("no files is an error") {
        auto binary = find_validate_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_validate binary not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(1);
        REQUIRE(env.is_valid());
        fs::path empty = fs::path(env.get_dir()) / "empty";
        std::error_code ec;
        fs::create_directories(empty, ec);

        int rc = -1;
        run_validate_capture(binary, {"-d", empty.string()}, &rc);
        CHECK(rc == 1);
    }

    TEST_CASE("--files flag validates a single file") {
        auto binary = find_validate_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_validate binary not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        auto f = make_valid_pfw(env, fs::path(env.get_dir()) / "single", 25, 0);
        REQUIRE(!f.empty());

        int rc = -1;
        auto out = run_validate_capture(binary, {"--files", f}, &rc);
        CHECK(rc == 0);
        CHECK(contains(out, "1 passed"));
    }

    TEST_CASE("--log-level flag accepted") {
        auto binary = find_validate_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_validate binary not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        auto f = make_valid_pfw(env, fs::path(env.get_dir()) / "loglvl", 10, 0);
        REQUIRE(!f.empty());

        int rc = -1;
        run_validate_capture(binary, {"--files", f, "--log-level", "debug"},
                             &rc);
        CHECK(rc == 0);
    }
}
