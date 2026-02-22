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
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/reorganization_planner.h>
#include <dftracer/utils/utilities/composites/file_compressor_utility.h>
#include <dftracer/utils/utilities/composites/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/composites/types.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites;
using namespace dftracer::utils::utilities::composites::dft;
using namespace dftracer::utils::utilities::composites::dft::indexing;
using namespace dftracer::utils::utilities::composites::dft::reorganize;

// Test trace layout:
// Line 0: HH metadata
// Line 1: FH metadata
// Line 2: POSIX read
// Line 3: POSIX write
// Line 4: APP compute
// Line 5: POSIX read
static std::string create_integration_test_trace(const std::string& dir) {
    std::string plain_path = dir + "/integration.trace";
    {
        std::ofstream ofs(plain_path);
        ofs << R"({"name":"HH","ph":"M","args":{)"
            << R"("value":"h1","name":"host1"}})"
            << "\n";
        ofs << R"({"name":"FH","ph":"M","args":{)"
            << R"("value":"f1","name":"/data/a.h5"}})"
            << "\n";
        ofs << R"({"name":"read","cat":"POSIX",)"
            << R"("pid":1,"tid":1,"ts":1000,)"
            << R"("dur":100,"ph":"X","args":{)"
            << R"("hhash":"h1","fhash":"f1"}})"
            << "\n";
        ofs << R"({"name":"write","cat":"POSIX",)"
            << R"("pid":1,"tid":1,"ts":2000,)"
            << R"("dur":200,"ph":"X","args":{)"
            << R"("hhash":"h1","fhash":"f1"}})"
            << "\n";
        ofs << R"({"name":"compute","cat":"APP",)"
            << R"("pid":1,"tid":1,"ts":3000,)"
            << R"("dur":500,"ph":"X","args":{)"
            << R"("hhash":"h1"}})"
            << "\n";
        ofs << R"({"name":"read","cat":"POSIX",)"
            << R"("pid":2,"tid":1,"ts":4000,)"
            << R"("dur":120,"ph":"X","args":{)"
            << R"("hhash":"h1","fhash":"f1"}})"
            << "\n";
        ofs.close();
    }

    std::string gz_path = plain_path + ".gz";
    dft_utils_test::compress_file_to_gzip(plain_path, gz_path);
    fs::remove(plain_path);
    return gz_path;
}

// Build .midx for a trace file using the full pipeline
static void build_midx_for_file(const std::string& trace_file,
                                const std::string& index_dir) {
    ManifestIndexBuildInput input;
    input.file_path = trace_file;
    input.index_dir = index_dir;
    input.force_rebuild = true;

    auto pipeline_config = PipelineConfig()
                               .with_name("IntegrationTestMidxBuild")
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
            result = executor.execute_with_context(ctx, input);
            co_return;
        },
        "BuildMidx");

    pipeline.set_source(task);
    pipeline.set_destination(task);
    pipeline.execute();

    if (!result.success) {
        throw std::runtime_error("Failed to build .midx for test: " +
                                 result.error_message);
    }
}

// Read all lines from a file
static std::vector<std::string> read_lines(const std::string& path) {
    std::ifstream ifs(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(ifs, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

// Check if a line contains a specific JSON field
// value
static bool line_contains(const std::string& line, const std::string& field,
                          const std::string& value) {
    std::string pattern = "\"" + field + "\":\"" + value + "\"";
    return line.find(pattern) != std::string::npos;
}

// Extract lines from a source file according to
// the extraction plan and write to group output
// files. Mirrors the logic in dftracer_organize.
static void execute_extraction(const ExtractionPlan& plan,
                               const std::string& index_dir,
                               std::map<std::string, FILE*>& group_files) {
    // Group tasks by (source_file_idx,
    // checkpoint_idx)
    struct CheckpointKey {
        std::size_t source_file_idx;
        std::uint64_t checkpoint_idx;
        bool operator<(const CheckpointKey& o) const {
            if (source_file_idx != o.source_file_idx)
                return source_file_idx < o.source_file_idx;
            return checkpoint_idx < o.checkpoint_idx;
        }
    };

    std::map<CheckpointKey, std::vector<const ExtractionTask*>>
        checkpoint_tasks;
    for (const auto& task : plan.tasks) {
        CheckpointKey key{task.source_file_idx, task.checkpoint_idx};
        checkpoint_tasks[key].push_back(&task);
    }

    for (const auto& [ckpt_key, tasks] : checkpoint_tasks) {
        const auto& src = plan.source_files[ckpt_key.source_file_idx];

        // Build line routing table
        std::map<std::uint32_t, std::vector<std::string>> line_routing;
        for (const auto* task : tasks) {
            for (auto ln : task->line_numbers) {
                line_routing[ln].push_back(task->target_group);
            }
        }

        // Create reader
        std::string idx_path =
            internal::determine_index_path(src.file_path, index_dir);
        auto reader_input =
            IndexedReadInput::from_file(src.file_path).with_index(idx_path);
        IndexedFileReaderUtility reader_utility;
        auto reader = reader_utility.process(reader_input);

        // Compute byte range
        std::uint64_t start_byte = tasks[0]->start_byte;
        std::uint64_t end_byte = tasks[0]->end_byte;

        auto stream = reader->stream(
            reader::internal::StreamConfig()
                .stream_type(reader::internal::StreamType::MULTI_LINES_BYTES)
                .range_type(reader::internal::RangeType::BYTE_RANGE)
                .buffer_size(4 * 1024 * 1024)
                .from(start_byte)
                .to(end_byte));

        std::uint32_t line_number = 0;
        while (!stream->done()) {
            auto chunk = stream->read();
            if (chunk.empty()) break;

            const char* data = chunk.data();
            std::size_t bytes_read = chunk.size();
            std::size_t pos = 0;

            while (pos < bytes_read) {
                const char* line_start = data + pos;
                const char* newline = static_cast<const char*>(
                    std::memchr(line_start, '\n', bytes_read - pos));
                if (!newline) break;
                std::size_t line_len =
                    static_cast<std::size_t>(newline - line_start);

                auto it = line_routing.find(line_number);
                if (it != line_routing.end()) {
                    for (const auto& gname : it->second) {
                        auto fit = group_files.find(gname);
                        if (fit != group_files.end()) {
                            std::fwrite(line_start, 1, line_len, fit->second);
                            std::fputc('\n', fit->second);
                        }
                    }
                }

                pos = static_cast<std::size_t>(newline - data) + 1;
                line_number++;
            }
        }
    }
}

TEST_SUITE("ReorganizeIntegration") {
    TEST_CASE("Full pipeline: two groups + remainder") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_reorg_integ").string();
        std::string input_dir = test_dir + "/input";
        std::string output_dir = test_dir + "/output";
        fs::create_directories(input_dir);
        fs::create_directories(output_dir);

        // Step 1: Create and index test trace
        std::string trace_file = create_integration_test_trace(input_dir);
        build_midx_for_file(trace_file, input_dir);

        // Step 2: Plan extraction
        ReorganizationPlannerUtility planner;
        ReorganizationPlannerInput planner_input;
        planner_input.source_files = {trace_file};
        planner_input.groups = {{"io", "cat=POSIX"}, {"compute", "cat=APP"}};
        planner_input.index_dir = input_dir;

        auto plan = planner.process(planner_input);
        REQUIRE(plan.tasks.size() > 0);

        // Step 3: Execute extraction
        std::map<std::string, FILE*> group_files;
        std::map<std::string, std::string> group_pfw_paths;
        for (const auto& g : plan.groups) {
            std::string pfw_path = output_dir + "/" + g.name + ".pfw";
            FILE* f = std::fopen(pfw_path.c_str(), "w");
            REQUIRE(f != nullptr);
            group_files[g.name] = f;
            group_pfw_paths[g.name] = pfw_path;
        }

        execute_extraction(plan, input_dir, group_files);

        for (auto& [gname, f] : group_files) {
            std::fclose(f);
        }

        // Verify io output
        std::string io_pfw = output_dir + "/io.pfw";
        REQUIRE(fs::exists(io_pfw));
        auto io_lines = read_lines(io_pfw);
        // io: HH, FH, read, write, read = 5 lines
        CHECK(io_lines.size() == 5);

        // Check metadata present
        bool has_hh = false;
        bool has_fh = false;
        for (const auto& line : io_lines) {
            if (line_contains(line, "name", "HH")) has_hh = true;
            if (line_contains(line, "name", "FH")) has_fh = true;
        }
        CHECK(has_hh);
        CHECK(has_fh);

        // Check POSIX events present
        std::size_t posix_events = 0;
        for (const auto& line : io_lines) {
            if (line_contains(line, "cat", "POSIX")) posix_events++;
        }
        CHECK(posix_events == 3);

        // Verify compute output
        std::string compute_pfw = output_dir + "/compute.pfw";
        REQUIRE(fs::exists(compute_pfw));
        auto compute_lines = read_lines(compute_pfw);
        // compute: HH, FH, compute = 3 lines
        CHECK(compute_lines.size() == 3);

        std::size_t app_events = 0;
        for (const auto& line : compute_lines) {
            if (line_contains(line, "cat", "APP")) app_events++;
        }
        CHECK(app_events == 1);

        // Verify no remainder file (all events
        // matched)
        std::string remainder_pfw = output_dir + "/remainder.pfw";
        if (fs::exists(remainder_pfw)) {
            CHECK(fs::file_size(remainder_pfw) == 0);
        }

        // Verify total lines: io(5) + compute(3)
        // = 8 (metadata duplicated in both)
        CHECK(io_lines.size() + compute_lines.size() == 8);

        fs::remove_all(test_dir);
    }

    TEST_CASE("Compression and sidecar building") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_reorg_compress")
                .string();
        std::string input_dir = test_dir + "/input";
        std::string output_dir = test_dir + "/output";
        fs::create_directories(input_dir);
        fs::create_directories(output_dir);

        // Create trace and index
        std::string trace_file = create_integration_test_trace(input_dir);
        build_midx_for_file(trace_file, input_dir);

        // Plan for io group only
        ReorganizationPlannerUtility planner;
        ReorganizationPlannerInput planner_input;
        planner_input.source_files = {trace_file};
        planner_input.groups = {{"io", "cat=POSIX"}};
        planner_input.index_dir = input_dir;

        auto plan = planner.process(planner_input);

        // Extract io group
        std::string io_pfw = output_dir + "/io.pfw";
        {
            std::map<std::string, FILE*> gf;
            for (const auto& g : plan.groups) {
                std::string pfw_path = output_dir + "/" + g.name + ".pfw";
                FILE* f = std::fopen(pfw_path.c_str(), "w");
                REQUIRE(f != nullptr);
                gf[g.name] = f;
            }

            execute_extraction(plan, input_dir, gf);

            for (auto& [gname, f] : gf) {
                std::fclose(f);
            }
        }

        REQUIRE(fs::exists(io_pfw));
        REQUIRE(fs::file_size(io_pfw) > 0);

        // Compress
        FileCompressorUtility compressor;
        auto comp_result =
            compressor.process(FileCompressionUtilityInput::from_file(io_pfw));
        CHECK(comp_result.success);

        std::string io_gz = io_pfw + ".gz";
        CHECK(fs::exists(io_gz));
        CHECK(fs::file_size(io_gz) > 0);

        // Remove plain .pfw
        fs::remove(io_pfw);

        // Build .midx sidecar for compressed output
        {
            auto pipeline_config = PipelineConfig()
                                       .with_name("SidecarBuild")
                                       .with_compute_threads(2)
                                       .with_watchdog(false);

            Pipeline pipeline(pipeline_config);
            ManifestIndexBuildOutput midx_result;

            auto task = make_task(
                [&](CoroScope& ctx) -> coro::CoroTask<void> {
                    ManifestIndexBuildInput midx_input;
                    midx_input.file_path = io_gz;
                    midx_input.index_dir = output_dir;
                    midx_input.force_rebuild = true;

                    auto utility =
                        std::make_shared<ManifestIndexBuilderUtility>();
                    behaviors::BehaviorChain<ManifestIndexBuildInput,
                                             ManifestIndexBuildOutput>
                        chain;
                    behaviors::UtilityExecutor<ManifestIndexBuildInput,
                                               ManifestIndexBuildOutput,
                                               utilities::tags::NeedsContext>
                        executor(utility, std::move(chain));
                    midx_result =
                        executor.execute_with_context(ctx, midx_input);
                    co_return;
                },
                "BuildOutputMidx");

            pipeline.set_source(task);
            pipeline.set_destination(task);
            pipeline.execute();

            CHECK(midx_result.success);
        }

        // Verify .midx exists for output
        std::string out_midx = determine_manifest_index_path(io_gz, output_dir);
        CHECK(fs::exists(out_midx));

        // Write provenance
        {
            ManifestIndexDatabase midx(out_midx);
            midx.init_schema();
            int fid = midx.get_file_info_id(io_gz);
            REQUIRE(fid >= 0);

            midx.begin_transaction();
            queries::insert_provenance_info(midx.db(), "version", "1.0");
            queries::insert_provenance_info(midx.db(), "tool",
                                            "dftracer_organize");
            queries::insert_provenance_group(midx.db(), "io", "cat=POSIX");
            queries::insert_provenance_source(midx.db(), fid, 0, trace_file, 1,
                                              "");
            queries::insert_provenance_segment(midx.db(), 0, 0, 0, 5, 3);
            midx.commit_transaction();
        }

        // Verify provenance
        {
            ManifestIndexDatabase midx(out_midx);
            midx.init_schema();
            int fid = midx.get_file_info_id(io_gz);
            REQUIRE(fid >= 0);

            CHECK(queries::query_provenance_info(midx.db(), "version") ==
                  "1.0");
            CHECK(queries::query_provenance_info(midx.db(), "tool") ==
                  "dftracer_organize");
            CHECK(queries::query_provenance_group_name(midx.db()) == "io");
            CHECK(queries::query_provenance_group_predicate(midx.db()) ==
                  "cat=POSIX");

            auto sources = queries::query_provenance_sources(midx.db(), fid);
            REQUIRE(sources.size() == 1);
            CHECK(sources[0].path == trace_file);

            auto segments = queries::query_provenance_segments(midx.db(), 0);
            REQUIRE(segments.size() == 1);
            CHECK(segments[0].output_line_start == 0);
            CHECK(segments[0].output_line_end == 5);
            CHECK(segments[0].event_count == 3);
        }

        fs::remove_all(test_dir);
    }
}
