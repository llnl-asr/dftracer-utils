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

using namespace dftracer::utils;

// ============================================================================
// Helpers
// ============================================================================

namespace {

dstd::String create_pfw_gz(dftu_utils_test::TestEnvironment& env,
                           int event_count, int id) {
    auto trace_gz = env.create_dft_test_gzip_file(event_count);
    if (trace_gz.empty()) return "";

    dstd::String pfw_path =
        env.dir() + "/trace_" + dstd::to_string(id) + ".pfw.gz";
    REQUIRE(dstd::fs::rename(trace_gz, pfw_path).has_value());
    return pfw_path;
}

dstd::String find_info_binary() {
    return dftu_utils_test::find_binary_by_name("DFTRACER_UTILS_INFO_PATH",
                                                "dftracer_info");
}

int run_info(const dstd::String& binary,
             const dstd::Vector<dstd::String>& args) {
    dftu_utils_test::set_test_library_path(binary);
    return dftu_utils_test::run_process(binary, args);
}

dstd::String run_info_capture(const dstd::String& binary,
                              const dstd::Vector<dstd::String>& args,
                              int* exit_code = nullptr) {
    dftu_utils_test::set_test_library_path(binary);
    return dftu_utils_test::run_process_capture(binary, args, true, exit_code);
}

}  // namespace

// ============================================================================
// Integration tests
// ============================================================================

TEST_SUITE("DFTracerInfo") {
    TEST_CASE("binary exists") {
        auto binary = find_info_binary();
        if (binary.empty()) {
            MESSAGE(
                "dftracer_info binary not found, skipping. "
                "Set DFTRACER_UTILS_INFO_PATH env to specify location.");
            return;
        }
        CHECK(!binary.empty());
    }

    TEST_CASE("info single file") {
        auto binary = find_info_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_info binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 50, 0);
        REQUIRE(!f.empty());

        int rc = 0;
        auto output = run_info_capture(binary, {f}, &rc);
        CHECK(rc == 0);
        // Default summary mode prints aggregate totals.
        CHECK(output.find("Total Files") != dstd::String::npos);
    }

    TEST_CASE("info with directory") {
        auto binary = find_info_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_info binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        for (int i = 0; i < 2; ++i) {
            auto f = create_pfw_gz(env, 20, i);
            REQUIRE(!f.empty());
        }

        int rc = run_info(binary, {env.dir()});
        CHECK(rc == 0);
    }

    TEST_CASE("info detailed query produces more output") {
        auto binary = find_info_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_info binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 50, 0);
        REQUIRE(!f.empty());

        int rc_plain = 0;
        auto plain = run_info_capture(binary, {f}, &rc_plain);
        CHECK(rc_plain == 0);

        int rc_detailed = 0;
        auto detailed =
            run_info_capture(binary, {f, "--report", "detailed"}, &rc_detailed);
        CHECK(rc_detailed == 0);

        // Detailed query adds the "Detailed Statistics:" section.
        CHECK(detailed.find("Detailed Statistics") != dstd::String::npos);
    }

    TEST_CASE("info with force rebuild") {
        auto binary = find_info_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_info binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 30, 0);
        REQUIRE(!f.empty());

        int rc = run_info(binary, {f, "--force"});
        CHECK(rc == 0);
    }

    TEST_CASE("rebuilds stale index on changed source") {
        auto binary = find_info_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_info binary not found, skipping.");
            return;
        }

        auto valid_events = [](const dstd::String& out) -> long {
            auto p = out.find("Valid Events");
            if (p == dstd::String::npos) return -1;
            p = out.find_first_of("0123456789", p);
            return p == dstd::String::npos ? -1 : std::atol(out.c_str() + p);
        };

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        auto f = create_pfw_gz(env, 100, 0);
        REQUIRE(!f.empty());

        int rc = 0;
        long n1 = valid_events(run_info_capture(binary, {env.dir()}, &rc));
        CHECK(rc == 0);
        CHECK(n1 > 0);

        // Rewrite the same file with more events; the index is now stale.
        auto raw = env.create_dft_test_gzip_file(300);
        REQUIRE(!raw.empty());
        dstd::fs::remove(f).discard();
        REQUIRE(dstd::fs::rename(raw, f).has_value());

        long n2 = valid_events(run_info_capture(binary, {env.dir()}, &rc));
        CHECK(rc == 0);
        CHECK(n2 > n1);
    }
}
