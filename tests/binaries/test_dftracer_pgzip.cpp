// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>
#include <zlib.h>

#include <cstdlib>
#include <fstream>

using namespace dftracer::utils;

// ============================================================================
// Helpers
// ============================================================================

namespace {

// Create a plain (uncompressed) .pfw file with DFTracer events.
dstd::String create_plain_pfw(dftu_utils_test::TestEnvironment& env,
                              int event_count, int id) {
    auto trace = env.create_dft_test_file(event_count);
    if (trace.empty()) return "";

    dstd::String pfw_path =
        env.dir() + "/trace_" + dstd::to_string(id) + ".pfw";
    REQUIRE(dstd::fs::rename(trace, pfw_path).has_value());
    return pfw_path;
}

dstd::String find_pgzip_binary() {
    return dftu_utils_test::find_binary_by_name("DFTRACER_UTILS_PGZIP_PATH",
                                                "dftracer_pgzip");
}

int run_pgzip(const dstd::String& binary,
              const dstd::Vector<dstd::String>& args) {
    return dftu_utils_test::run_process(binary, args);
}

dstd::String gunzip_all(const dstd::String& path) {
    gzFile gz = gzopen(path.c_str(), "rb");
    dstd::String out;
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
                "Set DFTRACER_UTILS_PGZIP_PATH env to specify location.");
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

        int rc = run_pgzip(binary, {env.dir(), "--disable-watchdog"});
        CHECK(rc == 0);

        dstd::String gz_path = pfw + ".gz";
        CHECK(dstd::fs::exists(gz_path).value());
        // Original plain file must be removed after successful compression.
        CHECK(!dstd::fs::exists(pfw).value());
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

        int rc = run_pgzip(binary, {env.dir(), "--disable-watchdog"});
        CHECK(rc == 0);

        dstd::String gz_path = pfw + ".gz";
        REQUIRE(dstd::fs::exists(gz_path).value());

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

        int rc = run_pgzip(binary, {env.dir(), "--compression-level", "1",
                                    "--disable-watchdog"});
        CHECK(rc == 0);
        CHECK(dstd::fs::exists(pfw + ".gz").value());
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

        int rc = run_pgzip(binary, {env.dir(), "--compression-level", "9",
                                    "--disable-watchdog"});
        CHECK(rc == 0);
        CHECK(dstd::fs::exists(pfw + ".gz").value());
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

        int rc = run_pgzip(binary, {env.dir(), "--disable-watchdog"});
        CHECK(rc == 0);
        CHECK(dstd::fs::exists(pfw0 + ".gz").value());
        CHECK(dstd::fs::exists(pfw1 + ".gz").value());
        CHECK(!dstd::fs::exists(pfw0).value());
        CHECK(!dstd::fs::exists(pfw1).value());
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
        int rc = run_pgzip(binary, {env.dir(), "--disable-watchdog"});
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

        dstd::String pfw = env.dir() + "/tail.pfw";
        dstd::String content;
        for (int i = 0; i < 2000; ++i)
            content += "{\"id\":" + dstd::to_string(i) + ",\"name\":\"e\"}\n";
        content += "{\"id\":-1,\"name\":\"no newline\"}";
        {
            dftu_utils_test::FileOut f(pfw);
            f << content;
        }

        int rc = run_pgzip(
            binary, {env.dir(), "--chunk-size", "4KB", "--disable-watchdog"});
        CHECK(rc == 0);
        CHECK(gunzip_all(pfw + ".gz") == content);
        CHECK(!dstd::fs::exists(pfw).value());
    }

    TEST_CASE("empty file fails and is left in place") {
        auto binary = find_pgzip_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_pgzip binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        dstd::String pfw = env.dir() + "/empty.pfw";
        {
            dftu_utils_test::FileOut f(pfw);
        }

        int rc = run_pgzip(binary, {env.dir(), "--disable-watchdog"});
        CHECK(rc != 0);
        CHECK(dstd::fs::exists(pfw).value());
        CHECK(!dstd::fs::exists(pfw + ".gz").value());
    }
}
