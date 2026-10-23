#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/trace/internal/utils.h>
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

std::string create_pfw_gz(dftu_utils_test::TestEnvironment& env, int num_events,
                          int id) {
    auto trace_gz = env.create_dft_test_gzip_file(num_events);
    if (trace_gz.empty()) return "";

    std::string pfw_path =
        env.get_dir() + "/trace_" + std::to_string(id) + ".pfw.gz";
    fs::rename(trace_gz, pfw_path);
    return pfw_path;
}

std::string find_index_binary() {
    return dftu_utils_test::find_binary_by_name("DFTRACER_UTILS_INDEX_PATH",
                                                "dftracer_index");
}

void write_plain_pfw(const std::string& path) {
    std::ofstream plain(path);
    plain << "{\"name\":\"read\",\"cat\":\"POSIX\",\"ph\":\"X\","
             "\"pid\":1,\"tid\":1,\"ts\":1,\"dur\":1}\n";
}

int run_index(const std::string& binary, const std::vector<std::string>& args) {
    dftu_utils_test::set_test_library_path(binary);
    return dftu_utils_test::run_process(binary, args);
}

}  // namespace

// ============================================================================
// Integration tests
// ============================================================================

TEST_SUITE("DFTracerIndex") {
    TEST_CASE("binary exists") {
        auto binary = find_index_binary();
        if (binary.empty()) {
            MESSAGE(
                "dftracer_index binary not found, skipping. "
                "Set DFTRACER_UTILS_INDEX_PATH env to specify location.");
            return;
        }
        CHECK(!binary.empty());
    }

    TEST_CASE("build bloom index") {
        auto binary = find_index_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_index binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 100, 0);
        REQUIRE(!f.empty());

        int rc = run_index(binary, {"-d", env.get_dir(), "--force"});
        CHECK(rc == 0);

        CHECK(fs::exists(
            dftracer::utils::trace::internal::determine_index_path(f, "")));
    }

    TEST_CASE("build index with custom index-dir") {
        auto binary = find_index_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_index binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 100, 0);
        REQUIRE(!f.empty());

        // Place index files in a separate subdirectory.
        std::string idx_dir = env.get_dir() + "/idx";
        fs::create_directories(idx_dir);

        int rc = run_index(
            binary, {"-d", env.get_dir(), "--force", "--index-dir", idx_dir});
        CHECK(rc == 0);

        CHECK(fs::exists(dftracer::utils::trace::internal::determine_index_path(
            f, idx_dir)));
    }

    TEST_CASE("force rebuild runs twice without error") {
        auto binary = find_index_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_index binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 100, 0);
        REQUIRE(!f.empty());

        int rc1 = run_index(binary, {"-d", env.get_dir(), "--force"});
        CHECK(rc1 == 0);
        REQUIRE(fs::exists(
            dftracer::utils::trace::internal::determine_index_path(f, "")));

        // Second run with --force must overwrite successfully.
        int rc2 = run_index(binary, {"-d", env.get_dir(), "--force"});
        CHECK(rc2 == 0);
        CHECK(fs::exists(
            dftracer::utils::trace::internal::determine_index_path(f, "")));
    }
    TEST_CASE("stats share and path budget flags") {
        auto binary = find_index_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_index binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 100, 0);
        REQUIRE(!f.empty());

        CHECK(
            run_index(binary, {"-d", env.get_dir(), "--force", "--stats-share",
                               "0.2", "--path-budget", "5"}) == 0);
        CHECK(fs::exists(
            dftracer::utils::trace::internal::determine_index_path(f, "")));
        CHECK(run_index(binary, {"-d", env.get_dir(), "--force",
                                 "--stats-share", "0"}) != 0);
        CHECK(run_index(binary, {"-d", env.get_dir(), "--force",
                                 "--stats-share", "1.5"}) != 0);
    }

    TEST_CASE("a plain trace makes the run fail and names the file") {
        auto binary = find_index_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_index binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        const std::string plain = env.get_dir() + "/plain.pfw";
        write_plain_pfw(plain);

        dftu_utils_test::set_test_library_path(binary);
        int rc = 0;
        const std::string out = dftu_utils_test::run_process_capture(
            binary, {"-d", env.get_dir(), "--force"}, true, &rc);
        CHECK(rc != 0);
        CHECK(out.find("plain.pfw") != std::string::npos);
        CHECK(out.find("is not gzip") != std::string::npos);
    }

    TEST_CASE("a plain trace beside a good one fails the run but indexes it") {
        auto binary = find_index_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_index binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto good = create_pfw_gz(env, 100, 0);
        REQUIRE(!good.empty());
        write_plain_pfw(env.get_dir() + "/plain.pfw");

        CHECK(run_index(binary, {"-d", env.get_dir(), "--force"}) != 0);
        CHECK(fs::exists(
            dftracer::utils::trace::internal::determine_index_path(good, "")));
    }
}
