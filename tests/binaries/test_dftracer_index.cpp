// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/index_internal.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <task_result.h>
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

dstd::String create_pfw_gz(dftu_utils_test::TestEnvironment& env,
                           int event_count, int id) {
    auto trace_gz = env.create_dft_test_gzip_file(event_count);
    if (trace_gz.empty()) return "";

    dstd::String pfw_path =
        env.dir() + "/trace_" + dstd::to_string(id) + ".pfw.gz";
    REQUIRE(dstd::fs::rename(trace_gz, pfw_path).has_value());
    return pfw_path;
}

dstd::String find_index_binary() {
    return dftu_utils_test::find_binary_by_name("DFTRACER_UTILS_INDEX_PATH",
                                                "dftracer_index");
}

void write_plain_pfw(const dstd::String& path) {
    dftu_utils_test::FileOut plain(path);
    plain << "{\"name\":\"read\",\"cat\":\"POSIX\",\"ph\":\"X\","
             "\"pid\":1,\"tid\":1,\"ts\":1,\"dur\":1}\n";
}

int run_index(const dstd::String& binary,
              const dstd::Vector<dstd::String>& args) {
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

        int rc = run_index(binary, {env.dir(), "--force"});
        CHECK(rc == 0);

        CHECK(dstd::fs::exists(
                  dstd::String(
                      dftracer::utils::trace::internal::determine_index_path(
                          f, "")))
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
        dstd::String idx_dir = env.dir() + "/idx";
        REQUIRE(dstd::fs::create_directories(idx_dir).has_value());

        int rc =
            run_index(binary, {env.dir(), "--force", "--index-dir", idx_dir});
        CHECK(rc == 0);

        CHECK(dstd::fs::exists(
                  dstd::String(
                      dftracer::utils::trace::internal::determine_index_path(
                          f, idx_dir)))
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

        int rc1 = run_index(binary, {env.dir(), "--force"});
        CHECK(rc1 == 0);
        REQUIRE(dstd::fs::exists(
                    dstd::String(
                        dftracer::utils::trace::internal::determine_index_path(
                            f, "")))
                    .value());

        // Second run with --force must overwrite successfully.
        int rc2 = run_index(binary, {env.dir(), "--force"});
        CHECK(rc2 == 0);
        CHECK(dstd::fs::exists(
                  dstd::String(
                      dftracer::utils::trace::internal::determine_index_path(
                          f, "")))
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

        CHECK(run_index(binary, {env.dir(), "--force", "--stats-share", "0.2",
                                 "--path-budget", "5"}) == 0);
        CHECK(dstd::fs::exists(
                  dstd::String(
                      dftracer::utils::trace::internal::determine_index_path(
                          f, "")))
                  .value());
        CHECK(run_index(binary, {env.dir(), "--force", "--stats-share", "0"}) !=
              0);
        CHECK(run_index(binary,
                        {env.dir(), "--force", "--stats-share", "1.5"}) != 0);
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
            const dstd::String out = dftu_utils_test::run_process_capture(
                binary, {env.dir()}, true, &rc);
            INFO(out);
            CHECK(rc == 0);
            CHECK(out.find("Files processed: " + dstd::to_string(processed) +
                           "\n") != dstd::String::npos);
            CHECK(out.find("Files skipped: " + dstd::to_string(skipped) +
                           "\n") != dstd::String::npos);
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
        const dstd::String out = dftu_utils_test::run_process_capture(
            binary, {env.dir(), "--read-batch-size", "4"}, true, &rc);
        INFO(out);
        CHECK(rc != 0);
        CHECK(out.find("--read-batch-size") != dstd::String::npos);
        CHECK(out.find("a unit is required") != dstd::String::npos);
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
        index::IndexerOptions o;
        o.aggregation.time_interval_us = 5'000'000;
        dftu_utils_test::ok(
            dftu_utils_test::ok(
                index::Indexer::open({dstd::String(dstd::StringView(f))}, o))
                .build());

        const auto index_path = dstd::String(
            dftracer::utils::trace::internal::determine_index_path(f, ""));
        dftu_utils_test::set_test_library_path(binary);
        int rc = -1;
        const dstd::String out = dftu_utils_test::run_process_capture(
            binary, {env.dir()}, true, &rc);
        INFO(out);
        CHECK(rc == 0);
        CHECK(out.find("Clearing the aggregation tier of " + index_path) !=
              dstd::String::npos);
        CHECK(out.find("Files processed: 1\n") != dstd::String::npos);

        index::release_root(index_path);
        const auto snapshot =
            dftu_utils_test::ok(index::plan::open_snapshot(index_path));
        const auto config = dftu_utils_test::ok(
            index::schemas::dft::agg::tier::read_config(snapshot));
        REQUIRE(config);
        CHECK(config->time_interval_us ==
              index::schemas::dft::agg::standard_aggregation_config()
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

        const dstd::String plain = env.dir() + "/plain.pfw.gz";
        write_plain_pfw(plain);

        dftu_utils_test::set_test_library_path(binary);
        int rc = 0;
        const dstd::String out = dftu_utils_test::run_process_capture(
            binary, {env.dir(), "--force"}, true, &rc);
        CHECK(rc != 0);
        CHECK(out.find("plain.pfw.gz") != dstd::String::npos);
        CHECK(out.find("is not gzip") != dstd::String::npos);
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
        write_plain_pfw(env.dir() + "/plain.pfw.gz");

        CHECK(run_index(binary, {env.dir(), "--force"}) != 0);
        CHECK(dstd::fs::exists(
                  dstd::String(
                      dftracer::utils::trace::internal::determine_index_path(
                          good, "")))
                  .value());
    }
}
