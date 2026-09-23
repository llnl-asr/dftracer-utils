#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>
#include <zlib.h>

#include <cstdlib>
#include <string>
#include <vector>

// ============================================================================
// Helpers
// ============================================================================

namespace {

std::string create_pfw_gz(dftu_utils_test::TestEnvironment& env, int num_events,
                          int id) {
    auto trace_gz = env.create_dft_test_gzip_file(num_events);
    if (trace_gz.empty()) return "";

    std::string pfw_path =
        env.get_dir() + "/trace_" + std::to_string(id) + ".pfw.gz";
    fs::rename(trace_gz, pfw_path);
    return pfw_path;
}

std::string find_info_binary() {
    return dftu_utils_test::find_binary_by_name("DFTRACER_INFO_PATH",
                                                "dftracer_info");
}

int run_info(const std::string& binary, const std::vector<std::string>& args) {
    dftu_utils_test::set_test_library_path(binary);
    return dftu_utils_test::run_process(binary, args);
}

std::string run_info_capture(const std::string& binary,
                             const std::vector<std::string>& args,
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
                "Set DFTRACER_INFO_PATH env to specify location.");
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
        auto output = run_info_capture(binary, {"--files", f}, &rc);
        CHECK(rc == 0);
        // Default summary mode prints aggregate totals.
        CHECK(output.find("Total Files") != std::string::npos);
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

        int rc = run_info(binary, {"-d", env.get_dir()});
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
        auto plain = run_info_capture(binary, {"--files", f}, &rc_plain);
        CHECK(rc_plain == 0);

        int rc_detailed = 0;
        auto detailed = run_info_capture(
            binary, {"--files", f, "--query", "detailed"}, &rc_detailed);
        CHECK(rc_detailed == 0);

        // Detailed query adds the "Detailed Statistics:" section.
        CHECK(detailed.find("Detailed Statistics") != std::string::npos);
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

        int rc = run_info(binary, {"--files", f, "--force-rebuild"});
        CHECK(rc == 0);
    }

    TEST_CASE("rebuilds stale index on changed source") {
        auto binary = find_info_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_info binary not found, skipping.");
            return;
        }

        auto valid_events = [](const std::string& out) -> long {
            auto p = out.find("Valid Events");
            if (p == std::string::npos) return -1;
            p = out.find_first_of("0123456789", p);
            return p == std::string::npos ? -1 : std::atol(out.c_str() + p);
        };

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        auto f = create_pfw_gz(env, 100, 0);
        REQUIRE(!f.empty());

        int rc = 0;
        long n1 =
            valid_events(run_info_capture(binary, {"-d", env.get_dir()}, &rc));
        CHECK(rc == 0);
        CHECK(n1 > 0);

        // Rewrite the same file with more events; the index is now stale.
        auto raw = env.create_dft_test_gzip_file(300);
        REQUIRE(!raw.empty());
        fs::remove(f);
        fs::rename(raw, f);

        long n2 =
            valid_events(run_info_capture(binary, {"-d", env.get_dir()}, &rc));
        CHECK(rc == 0);
        CHECK(n2 > n1);
    }
}
