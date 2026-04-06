#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/utilities/behaviors/behavior_chain.h>
#include <dftracer/utils/core/utilities/utility_executor.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/reorganization_planner.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/provenance_database.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <string>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites::dft::reorganize;
using dftracer::utils::utilities::behaviors::BehaviorChain;
using dftracer::utils::utilities::behaviors::UtilityExecutor;
using dftracer::utils::utilities::indexer::determine_provenance_index_path;
using dftracer::utils::utilities::indexer::IndexBuildConfig;
using dftracer::utils::utilities::indexer::IndexBuilderUtility;
using dftracer::utils::utilities::indexer::ProvenanceDatabase;
namespace tags = dftracer::utils::utilities::tags;

// Create a test trace with known events:
// Line 0: HH metadata
// Line 1: FH metadata
// Line 2: POSIX read
// Line 3: POSIX write
// Line 4: APP compute
// Line 5: POSIX read
static std::string create_planner_test_trace(const std::string& dir) {
    std::string plain_path = dir + "/planner_test.trace";
    {
        std::ofstream ofs(plain_path);
        ofs << R"({"name":"HH","ph":"M","args":{"value":"h1","name":"host1"}})"
            << "\n";
        ofs << R"({"name":"FH","ph":"M","args":{"value":"f1","name":"/data/a.h5"}})"
            << "\n";
        ofs << R"({"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1000,"dur":100,"ph":"X","args":{"hhash":"h1","fhash":"f1"}})"
            << "\n";
        ofs << R"({"name":"write","cat":"POSIX","pid":1,"tid":1,"ts":2000,"dur":200,"ph":"X","args":{"hhash":"h1","fhash":"f1"}})"
            << "\n";
        ofs << R"({"name":"compute","cat":"APP","pid":1,"tid":1,"ts":3000,"dur":500,"ph":"X","args":{"hhash":"h1"}})"
            << "\n";
        ofs << R"({"name":"read","cat":"POSIX","pid":2,"tid":1,"ts":4000,"dur":120,"ph":"X","args":{"hhash":"h1","fhash":"f1"}})"
            << "\n";
        ofs.close();
    }

    std::string gz_path = plain_path + ".gz";
    dft_utils_test::compress_file_to_gzip(plain_path, gz_path);
    fs::remove(plain_path);
    return gz_path;
}

// Build .idx for a trace file using IndexBuilder via Runtime.
static void build_idx(const std::string& trace_file,
                      const std::string& index_dir) {
    Runtime rt(4);
    indexer::IndexBuildResult result;
    auto* result_ptr = &result;

    auto task = run_coro_scope(
        rt.executor(),
        [trace_file, index_dir,
         result_ptr](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<indexer::IndexBuildConfig,
                            indexer::IndexBuildResult, tags::NeedsContext>
                exec(builder, BehaviorChain<indexer::IndexBuildConfig,
                                            indexer::IndexBuildResult>{});
            auto config = IndexBuildConfig::for_file(trace_file)
                              .with_index_dir(index_dir)
                              .with_manifest(true)
                              .with_index_threshold(0);
            *result_ptr = co_await exec.execute_with_context(scope, config);
        });

    rt.submit(std::move(task), "build-idx").wait();
    rt.shutdown();

    if (!result.success) {
        throw std::runtime_error("Failed to build .idx for test");
    }
}

TEST_SUITE("ReorganizationPlanner") {
    TEST_CASE("parse_group_specs") {
        SUBCASE("name:query format") {
            auto groups = parse_group_specs(
                {"io:cat == \"POSIX\"", "compute:cat == \"APP\""});
            REQUIRE(groups.size() == 2);
            CHECK(groups[0].name == "io");
            CHECK(groups[0].query == "cat == \"POSIX\"");
            CHECK(groups[1].name == "compute");
            CHECK(groups[1].query == "cat == \"APP\"");
        }

        SUBCASE("name only (remainder)") {
            auto groups = parse_group_specs({"other"});
            REQUIRE(groups.size() == 1);
            CHECK(groups[0].name == "other");
            CHECK(groups[0].query.empty());
        }

        SUBCASE("complex query") {
            auto groups = parse_group_specs(
                {"io:cat == \"POSIX\" and name in [\"read\", \"write\"]"});
            REQUIRE(groups.size() == 1);
            CHECK(groups[0].name == "io");
            CHECK(groups[0].query ==
                  "cat == \"POSIX\" and name in [\"read\", \"write\"]");
        }

        SUBCASE("empty input") {
            auto groups = parse_group_specs({});
            CHECK(groups.empty());
        }
    }

    TEST_CASE("Plan with single group") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_planner_single")
                .string();
        fs::create_directories(test_dir);

        std::string trace_file = create_planner_test_trace(test_dir);
        build_idx(trace_file, test_dir);

        ReorganizationPlannerUtility planner;
        ReorganizationPlannerInput input;
        input.source_files = {trace_file};
        input.groups = {{"io", R"(cat == "POSIX")"}};
        input.index_dir = test_dir;

        auto plan = planner.process(input).get();

        // Should have 2 groups: "io" + auto-created "remainder"
        CHECK(plan.groups.size() == 2);
        CHECK(plan.groups[0].name == "io");
        CHECK(plan.groups[1].name == "remainder");

        CHECK(plan.source_files.size() == 1);
        CHECK(plan.source_files[0].file_path == trace_file);
        CHECK(plan.source_files[0].num_checkpoints == 1);

        CHECK(plan.tasks.size() == 2);

        const ExtractionTask* io_task = nullptr;
        const ExtractionTask* remainder_task = nullptr;
        for (const auto& t : plan.tasks) {
            if (t.target_group == "io") {
                io_task = &t;
            } else if (t.target_group == "remainder") {
                remainder_task = &t;
            }
        }

        REQUIRE(io_task != nullptr);
        REQUIRE(remainder_task != nullptr);

        // io group: POSIX read (lines 2, 5) + POSIX write (line 3) + metadata
        // (lines 0, 1) = lines {0, 1, 2, 3, 5}
        std::set<std::uint32_t> io_lines(io_task->line_numbers.begin(),
                                         io_task->line_numbers.end());
        CHECK(io_lines.count(0) == 1);  // HH metadata
        CHECK(io_lines.count(1) == 1);  // FH metadata
        CHECK(io_lines.count(2) == 1);  // POSIX read
        CHECK(io_lines.count(3) == 1);  // POSIX write
        CHECK(io_lines.count(5) == 1);  // POSIX read

        // remainder group: APP compute (line 4) + metadata (lines 0, 1)
        // = lines {0, 1, 4}
        std::set<std::uint32_t> rem_lines(remainder_task->line_numbers.begin(),
                                          remainder_task->line_numbers.end());
        CHECK(rem_lines.count(0) == 1);  // HH metadata
        CHECK(rem_lines.count(1) == 1);  // FH metadata
        CHECK(rem_lines.count(4) == 1);  // APP compute

        CHECK(plan.total_events == 4);

        fs::remove_all(test_dir);
    }

    TEST_CASE("Plan with multiple groups") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_planner_multi")
                .string();
        fs::create_directories(test_dir);

        std::string trace_file = create_planner_test_trace(test_dir);
        build_idx(trace_file, test_dir);

        ReorganizationPlannerUtility planner;
        ReorganizationPlannerInput input;
        input.source_files = {trace_file};
        input.groups = {{"io", R"(cat == "POSIX")"},
                        {"compute", R"(cat == "APP")"}};
        input.index_dir = test_dir;

        auto plan = planner.process(input).get();

        CHECK(plan.groups.size() == 3);

        std::size_t io_count = 0;
        std::size_t compute_count = 0;
        std::size_t remainder_count = 0;
        for (const auto& t : plan.tasks) {
            if (t.target_group == "io") {
                ++io_count;
            } else if (t.target_group == "compute") {
                ++compute_count;
            } else if (t.target_group == "remainder") {
                ++remainder_count;
            }
        }
        CHECK(io_count == 1);
        CHECK(compute_count == 1);
        CHECK(remainder_count == 0);

        fs::remove_all(test_dir);
    }

    TEST_CASE("Metadata in all groups") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_planner_meta").string();
        fs::create_directories(test_dir);

        std::string trace_file = create_planner_test_trace(test_dir);
        build_idx(trace_file, test_dir);

        ReorganizationPlannerUtility planner;
        ReorganizationPlannerInput input;
        input.source_files = {trace_file};
        input.groups = {{"io", R"(cat == "POSIX")"},
                        {"compute", R"(cat == "APP")"}};
        input.index_dir = test_dir;

        auto plan = planner.process(input).get();

        for (const auto& t : plan.tasks) {
            if (t.target_group == "remainder") {
                continue;
            }
            std::set<std::uint32_t> lines(t.line_numbers.begin(),
                                          t.line_numbers.end());
            std::string hh_msg =
                "Group " + t.target_group + " missing HH metadata (line 0)";
            std::string fh_msg =
                "Group " + t.target_group + " missing FH metadata (line 1)";
            CHECK_MESSAGE(lines.count(0) == 1, hh_msg.c_str());
            CHECK_MESSAGE(lines.count(1) == 1, fh_msg.c_str());
        }

        fs::remove_all(test_dir);
    }

    TEST_CASE("Provenance insert and query") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_planner_prov").string();
        fs::create_directories(test_dir);
        std::string provenance_path = test_dir + "/test_prov.pfw.gz.pidx";

        ProvenanceDatabase pdb(provenance_path);
        pdb.init_schema();
        int fid = pdb.get_or_create_file_info("test.pfw.gz", 0);

        pdb.begin_transaction();

        pdb.insert_info(fid, "version", "1.0");
        pdb.insert_info(fid, "created_at", "2026-02-17");
        pdb.insert_source(fid, 0, "/data/trace.pfw.gz", 9, "abc123");
        pdb.insert_group(fid, "io", R"(cat == "POSIX")");
        pdb.insert_segment(fid, 0, 0, 0, 100, 50);
        pdb.insert_segment(fid, 0, 1, 100, 200, 45);

        pdb.commit_transaction();

        CHECK(pdb.query_info(fid, "version") == "1.0");
        CHECK(pdb.query_info(fid, "created_at") == "2026-02-17");
        CHECK(pdb.query_info(fid, "nonexistent").empty());

        auto sources = pdb.query_sources(fid);
        REQUIRE(sources.size() == 1);
        CHECK(sources[0].source_idx == 0);
        CHECK(sources[0].path == "/data/trace.pfw.gz");
        CHECK(sources[0].num_checkpoints == 9);
        CHECK(sources[0].event_hash == "abc123");

        auto segments = pdb.query_segments(fid, 0);
        REQUIRE(segments.size() == 2);
        CHECK(segments[0].source_checkpoint == 0);
        CHECK(segments[0].output_line_start == 0);
        CHECK(segments[0].output_line_end == 100);
        CHECK(segments[0].event_count == 50);
        CHECK(segments[1].source_checkpoint == 1);

        CHECK(pdb.query_group_name(fid) == "io");
        CHECK(pdb.query_group_predicate(fid) == R"(cat == "POSIX")");

        fs::remove_all(test_dir);
    }
}
