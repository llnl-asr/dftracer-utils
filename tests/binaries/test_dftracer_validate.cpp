// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>

using namespace dftracer::utils;

// ============================================================================
// Helpers
// ============================================================================

namespace {

// Move a freshly generated valid pfw.gz into `dir`.
dstd::String make_valid_pfw(dftu_utils_test::TestEnvironment& env,
                            const dstd::Path& dir, int num_events, int id) {
    auto gz = env.create_dft_test_gzip_file(num_events);
    if (gz.empty()) return "";
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    dstd::Path dest = dir / ("trace_" + dstd::to_string(id) + ".pfw.gz");
    if (!dstd::fs::rename(gz, dest).has_value()) return "";
    return dstd::String(dest.str());
}

// Write a plain .pfw whose second event line is not valid JSON.
dstd::String make_bad_pfw(const dstd::Path& dir, int id) {
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    dstd::Path p = dir / ("bad_" + dstd::to_string(id) + ".pfw");
    dftu_utils_test::FileOut ofs(p);
    if (!ofs) return "";
    ofs << "[\n";
    ofs << "{\"name\":\"ok\",\"cat\":\"POSIX\",\"pid\":1,\"tid\":1,\"ts\":1,"
           "\"dur\":1,\"ph\":\"X\"}\n";
    ofs << "{\"name\": this is not valid json\n";
    return dstd::String(p.str());
}

dstd::String find_validate_binary() {
    return dftu_utils_test::find_binary_by_name("DFTRACER_UTILS_VALIDATE_PATH",
                                                "dftracer_validate");
}

// Run the binary, capture stdout, and report the exit code via *rc.
dstd::String run_validate_capture(const dstd::String& binary,
                                  const dstd::Vector<dstd::String>& args,
                                  int* rc) {
    return dftu_utils_test::run_process_capture(binary, args, false, rc);
}

bool contains(const dstd::String& hay, const char* needle) {
    return hay.find(needle) != dstd::String::npos;
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
        dstd::Path work = dstd::Path(env.get_dir()) / "valid";
        REQUIRE(!make_valid_pfw(env, work, 30, 0).empty());
        REQUIRE(!make_valid_pfw(env, work, 20, 1).empty());

        int rc = -1;
        auto out =
            run_validate_capture(binary, {dstd::String(work.str())}, &rc);
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
        dstd::Path root = dstd::Path(env.get_dir()) / "nested";
        REQUIRE(!make_valid_pfw(env, root / "a", 10, 0).empty());
        REQUIRE(!make_valid_pfw(env, root / "b" / "c", 10, 1).empty());

        int rc = -1;
        auto out =
            run_validate_capture(binary, {dstd::String(root.str())}, &rc);
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
        auto bad = make_bad_pfw(dstd::Path(env.get_dir()) / "bad", 0);
        REQUIRE(!bad.empty());

        int rc = -1;
        auto out = run_validate_capture(binary, {bad}, &rc);
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
        dstd::Path empty = dstd::Path(env.get_dir()) / "empty";
        REQUIRE(dstd::fs::create_directories(empty).has_value());

        int rc = -1;
        run_validate_capture(binary, {dstd::String(empty.str())}, &rc);
        CHECK(rc == 1);
    }

    TEST_CASE("a file path validates a single file") {
        auto binary = find_validate_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_validate binary not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        auto f =
            make_valid_pfw(env, dstd::Path(env.get_dir()) / "single", 25, 0);
        REQUIRE(!f.empty());

        int rc = -1;
        auto out = run_validate_capture(binary, {f}, &rc);
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
        auto f =
            make_valid_pfw(env, dstd::Path(env.get_dir()) / "loglvl", 10, 0);
        REQUIRE(!f.empty());

        int rc = -1;
        run_validate_capture(binary, {f, "--log-level", "debug"}, &rc);
        CHECK(rc == 0);
    }

    TEST_CASE("a path that holds a space reaches the tool whole") {
        auto binary = find_validate_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_validate binary not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        auto f = make_valid_pfw(env, dstd::Path(env.get_dir()) / "with space",
                                10, 0);
        REQUIRE(!f.empty());

        int rc = -1;
        auto out = run_validate_capture(binary, {f}, &rc);
        CHECK(rc == 0);
        CHECK(contains(out, "1 passed"));
    }

    TEST_CASE("an unknown flag exits with 1") {
        auto binary = find_validate_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_validate binary not found, skipping.");
            return;
        }
        int rc = -1;
        run_validate_capture(binary, {"--no-such-flag"}, &rc);
        CHECK(rc == 1);
    }

    TEST_CASE("a bad number exits with 1") {
        auto binary = find_validate_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_validate binary not found, skipping.");
            return;
        }
        int rc = -1;
        run_validate_capture(binary, {"--executor-threads", "many", "."}, &rc);
        CHECK(rc == 1);
    }

    TEST_CASE("--help and -v print and exit with 0") {
        auto binary = find_validate_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_validate binary not found, skipping.");
            return;
        }
        int rc = -1;
        auto help = run_validate_capture(binary, {"--help"}, &rc);
        CHECK(rc == 0);
        CHECK(contains(help, "Usage: dftracer_validate"));
        auto version = run_validate_capture(binary, {"-v"}, &rc);
        CHECK(rc == 0);
        CHECK(!version.empty());
    }
}
