// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/index_internal.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
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
    REQUIRE(dftracer::utils::dstd::fs::rename(trace_gz, pfw_path).has_value());
    return pfw_path;
}

std::string find_index_binary() {
    return dftu_utils_test::find_binary_by_name("DFTRACER_UTILS_INDEX_PATH",
                                                "dftracer_index");
}

void write_plain_pfw(const std::string& path) {
    std::ofstream plain(path.c_str());
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

        CHECK(dftracer::utils::dstd::fs::exists(
                  dftracer::utils::trace::internal::determine_index_path(f, ""))
                  .value());
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
        REQUIRE(
            dftracer::utils::dstd::fs::create_directories(idx_dir).has_value());

        int rc = run_index(
            binary, {"-d", env.get_dir(), "--force", "--index-dir", idx_dir});
        CHECK(rc == 0);

        CHECK(dftracer::utils::dstd::fs::exists(
                  dftracer::utils::trace::internal::determine_index_path(
                      f, idx_dir))
                  .value());
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
        REQUIRE(
            dftracer::utils::dstd::fs::exists(
                dftracer::utils::trace::internal::determine_index_path(f, ""))
                .value());

        // Second run with --force must overwrite successfully.
        int rc2 = run_index(binary, {"-d", env.get_dir(), "--force"});
        CHECK(rc2 == 0);
        CHECK(dftracer::utils::dstd::fs::exists(
                  dftracer::utils::trace::internal::determine_index_path(f, ""))
                  .value());
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
        CHECK(dftracer::utils::dstd::fs::exists(
                  dftracer::utils::trace::internal::determine_index_path(f, ""))
                  .value());
        CHECK(run_index(binary, {"-d", env.get_dir(), "--force",
                                 "--stats-share", "0"}) != 0);
        CHECK(run_index(binary, {"-d", env.get_dir(), "--force",
                                 "--stats-share", "1.5"}) != 0);
    }

    TEST_CASE("a later run reads the index back and builds only new files") {
        auto binary = find_index_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_index binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, 100, 0).empty());
        REQUIRE(!create_pfw_gz(env, 150, 1).empty());

        dftu_utils_test::set_test_library_path(binary);
        const auto run = [&binary, &env](std::size_t processed,
                                         std::size_t skipped) {
            int rc = -1;
            const std::string out = dftu_utils_test::run_process_capture(
                binary, {"-d", env.get_dir()}, true, &rc);
            INFO(out);
            CHECK(rc == 0);
            CHECK(out.find("Files processed: " + std::to_string(processed) +
                           "\n") != std::string::npos);
            CHECK(out.find("Files skipped: " + std::to_string(skipped) +
                           "\n") != std::string::npos);
        };
        run(2, 0);
        // Each run is its own process, so the second one learns the two
        // files from the index the first one wrote.
        run(0, 2);
        REQUIRE(!create_pfw_gz(env, 120, 2).empty());
        run(1, 2);
    }

    TEST_CASE("a read batch size without a unit is refused") {
        auto binary = find_index_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_index binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, 100, 0).empty());

        dftu_utils_test::set_test_library_path(binary);
        int rc = -1;
        const std::string out = dftu_utils_test::run_process_capture(
            binary, {"-d", env.get_dir(), "--read-batch-size", "4"}, true, &rc);
        INFO(out);
        CHECK(rc != 0);
        CHECK(out.find("--read-batch-size") != std::string::npos);
        CHECK(out.find("a unit is required") != std::string::npos);
    }

    TEST_CASE("a tier of other settings is cleared and built again") {
        auto binary = find_index_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_index binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        const auto f = create_pfw_gz(env, 100, 0);
        REQUIRE(!f.empty());
        namespace ix = dftracer::utils::index;
        ix::IndexerOptions o;
        o.aggregation.time_interval_us = 5'000'000;
        ix::Indexer::open({f}, o).build();

        const auto index_path =
            dftracer::utils::trace::internal::determine_index_path(f, "");
        dftu_utils_test::set_test_library_path(binary);
        int rc = -1;
        const std::string out = dftu_utils_test::run_process_capture(
            binary, {"-d", env.get_dir()}, true, &rc);
        INFO(out);
        CHECK(rc == 0);
        CHECK(out.find("Clearing the aggregation tier of " + index_path) !=
              std::string::npos);
        CHECK(out.find("Files processed: 1\n") != std::string::npos);

        ix::release_root(index_path);
        const auto snapshot = dftracer::utils::common::unwrap(
            ix::plan::open_snapshot(index_path));
        const auto config = dftracer::utils::common::unwrap(
            ix::schemas::dft::agg::tier::read_config(snapshot));
        REQUIRE(config);
        CHECK(config->time_interval_us ==
              ix::schemas::dft::agg::standard_aggregation_config()
                  .time_interval_us);
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
        CHECK(dftracer::utils::dstd::fs::exists(
                  dftracer::utils::trace::internal::determine_index_path(good,
                                                                         ""))
                  .value());
    }
}
