#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/utilities/behaviors/behavior_chain.h>
#include <dftracer/utils/core/utilities/utility_executor.h>
#include <dftracer/utils/utilities/composites/dft/indexing/manifest_index_builder.h>
#include <dftracer/utils/utilities/composites/dft/indexing/manifest_index_schema.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/manifest_queries.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/reorganization_planner.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <string>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites::dft::indexing;
using namespace dftracer::utils::utilities::composites::dft::reorganize;

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

// Build .midx for a trace file using the full pipeline
static void build_midx(const std::string& trace_file,
                       const std::string& index_dir) {
    ManifestIndexBuildInput input;
    input.file_path = trace_file;
    input.index_dir = index_dir;
    input.force_rebuild = true;

    auto pipeline_config = PipelineConfig()
                               .with_name("PlannerTestMidxBuild")
                               .with_compute_threads(2)
                               .with_watchdog(false);

    Pipeline pipeline(pipeline_config);
    ManifestIndexBuildOutput result;

    auto task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            auto utility = std::make_shared<ManifestIndexBuilderUtility>();
            behaviors::BehaviorChain<ManifestIndexBuildInput,
                                     ManifestIndexBuildOutput>
                chain;
            behaviors::UtilityExecutor<ManifestIndexBuildInput,
                                       ManifestIndexBuildOutput,
                                       utilities::tags::NeedsContext>
                executor(utility, std::move(chain));
            result = co_await executor.execute_with_context(ctx, input);
            co_return;
        },
        "BuildMidx");

    pipeline.set_source(task);
    pipeline.set_destination(task);
    pipeline.execute();

    if (!result.success) {
        throw std::runtime_error("Failed to build .midx for test");
    }
}

TEST_SUITE("ReorganizationPlanner") {
    TEST_CASE("parse_group_specs") {
        SUBCASE("name:predicate format") {
            auto groups =
                parse_group_specs({"io:cat=POSIX", "compute:cat=APP"});
            REQUIRE(groups.size() == 2);
            CHECK(groups[0].name == "io");
            CHECK(groups[0].predicate == "cat=POSIX");
            CHECK(groups[1].name == "compute");
            CHECK(groups[1].predicate == "cat=APP");
        }

        SUBCASE("name only (remainder)") {
            auto groups = parse_group_specs({"other"});
            REQUIRE(groups.size() == 1);
            CHECK(groups[0].name == "other");
            CHECK(groups[0].predicate.empty());
        }

        SUBCASE("complex predicate") {
            auto groups = parse_group_specs({"io:cat=POSIX,name=read|write"});
            REQUIRE(groups.size() == 1);
            CHECK(groups[0].name == "io");
            CHECK(groups[0].predicate == "cat=POSIX,name=read|write");
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
        build_midx(trace_file, test_dir);

        ReorganizationPlannerUtility planner;
        ReorganizationPlannerInput input;
        input.source_files = {trace_file};
        input.groups = {{"io", "cat=POSIX"}};
        input.index_dir = test_dir;

        auto plan = planner.process(input).get();

        // Should have 2 groups: "io" + auto-created
        // "remainder"
        CHECK(plan.groups.size() == 2);
        CHECK(plan.groups[0].name == "io");
        CHECK(plan.groups[1].name == "remainder");

        // Should have 1 source file
        CHECK(plan.source_files.size() == 1);
        CHECK(plan.source_files[0].file_path == trace_file);
        CHECK(plan.source_files[0].num_checkpoints == 1);

        // Should have 2 tasks (io + remainder) for
        // checkpoint 0
        CHECK(plan.tasks.size() == 2);

        // Find io task
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

        // io group: POSIX read (lines 2, 5) + POSIX write
        // (line 3) + metadata (lines 0, 1)
        // = lines {0, 1, 2, 3, 5}
        std::set<std::uint32_t> io_lines(io_task->line_numbers.begin(),
                                         io_task->line_numbers.end());
        CHECK(io_lines.count(0) == 1);  // HH metadata
        CHECK(io_lines.count(1) == 1);  // FH metadata
        CHECK(io_lines.count(2) == 1);  // POSIX read
        CHECK(io_lines.count(3) == 1);  // POSIX write
        CHECK(io_lines.count(5) == 1);  // POSIX read

        // remainder group: APP compute (line 4) + metadata
        // (lines 0, 1)
        // = lines {0, 1, 4}
        std::set<std::uint32_t> rem_lines(remainder_task->line_numbers.begin(),
                                          remainder_task->line_numbers.end());
        CHECK(rem_lines.count(0) == 1);  // HH metadata
        CHECK(rem_lines.count(1) == 1);  // FH metadata
        CHECK(rem_lines.count(4) == 1);  // APP compute

        // Total events should be 4 (3 POSIX + 1 APP)
        CHECK(plan.total_events == 4);

        fs::remove_all(test_dir);
    }

    TEST_CASE("Plan with multiple groups") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_planner_multi")
                .string();
        fs::create_directories(test_dir);

        std::string trace_file = create_planner_test_trace(test_dir);
        build_midx(trace_file, test_dir);

        ReorganizationPlannerUtility planner;
        ReorganizationPlannerInput input;
        input.source_files = {trace_file};
        input.groups = {{"io", "cat=POSIX"}, {"compute", "cat=APP"}};
        input.index_dir = test_dir;

        auto plan = planner.process(input).get();

        // 3 groups: io, compute, remainder
        CHECK(plan.groups.size() == 3);

        // All events matched, so remainder should have no
        // tasks (or no event lines, just metadata if any
        // group has events)
        // Actually: all events match io or compute, so
        // remainder has no events -> no remainder task
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
        build_midx(trace_file, test_dir);

        ReorganizationPlannerUtility planner;
        ReorganizationPlannerInput input;
        input.source_files = {trace_file};
        input.groups = {{"io", "cat=POSIX"}, {"compute", "cat=APP"}};
        input.index_dir = test_dir;

        auto plan = planner.process(input).get();

        // Both io and compute tasks should contain
        // metadata lines 0 and 1
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
        std::string midx_path = test_dir + "/test_prov.pfw.gz.midx";

        ManifestIndexDatabase midx(midx_path);
        midx.init_schema();
        int fid = midx.get_or_create_file_info("test.pfw.gz", 0);

        midx.begin_transaction();

        queries::insert_provenance_info(midx.db(), "version", "1.0");
        queries::insert_provenance_info(midx.db(), "created_at", "2026-02-17");
        queries::insert_provenance_source(midx.db(), fid, 0,
                                          "/data/trace.pfw.gz", 9, "abc123");
        queries::insert_provenance_group(midx.db(), "io", "cat=POSIX");
        queries::insert_provenance_segment(midx.db(), 0, 0, 0, 100, 50);
        queries::insert_provenance_segment(midx.db(), 0, 1, 100, 200, 45);

        midx.commit_transaction();

        // Query provenance info
        CHECK(queries::query_provenance_info(midx.db(), "version") == "1.0");
        CHECK(queries::query_provenance_info(midx.db(), "created_at") ==
              "2026-02-17");
        CHECK(queries::query_provenance_info(midx.db(), "nonexistent").empty());

        // Query provenance sources
        auto sources = queries::query_provenance_sources(midx.db(), fid);
        REQUIRE(sources.size() == 1);
        CHECK(sources[0].source_idx == 0);
        CHECK(sources[0].path == "/data/trace.pfw.gz");
        CHECK(sources[0].num_checkpoints == 9);
        CHECK(sources[0].event_hash == "abc123");

        // Query provenance segments
        auto segments = queries::query_provenance_segments(midx.db(), 0);
        REQUIRE(segments.size() == 2);
        CHECK(segments[0].source_checkpoint == 0);
        CHECK(segments[0].output_line_start == 0);
        CHECK(segments[0].output_line_end == 100);
        CHECK(segments[0].event_count == 50);
        CHECK(segments[1].source_checkpoint == 1);

        // Query provenance group
        CHECK(queries::query_provenance_group_name(midx.db()) == "io");
        CHECK(queries::query_provenance_group_predicate(midx.db()) ==
              "cat=POSIX");

        fs::remove_all(test_dir);
    }
}
