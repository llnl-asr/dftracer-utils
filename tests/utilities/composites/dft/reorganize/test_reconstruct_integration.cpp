#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/utilities/behaviors/behavior_chain.h>
#include <dftracer/utils/core/utilities/utility_executor.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/reconstruction_planner.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/reorganization_planner.h>
#include <dftracer/utils/utilities/composites/file_compressor_utility.h>
#include <dftracer/utils/utilities/composites/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/composites/types.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/provenance_database.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites;
using namespace dftracer::utils::utilities::composites::dft;
using namespace dftracer::utils::utilities::composites::dft::reorganize;
using dftracer::utils::utilities::behaviors::BehaviorChain;
using dftracer::utils::utilities::behaviors::UtilityExecutor;
using dftracer::utils::utilities::indexer::determine_provenance_index_path;
using dftracer::utils::utilities::indexer::IndexBuildConfig;
using dftracer::utils::utilities::indexer::IndexBuilderUtility;
using dftracer::utils::utilities::indexer::ProvenanceDatabase;
namespace tags = dftracer::utils::utilities::tags;

static ExtractionPlan run_planner(const ReorganizationPlannerInput& input) {
    Runtime rt(4);
    ExtractionPlan result;
    auto* result_ptr = &result;

    auto task = run_coro_scope(
        rt.executor(),
        [input, result_ptr](CoroScope& scope) -> coro::CoroTask<void> {
            auto planner = std::make_shared<ReorganizationPlannerUtility>();
            UtilityExecutor<ReorganizationPlannerInput, ExtractionPlan,
                            tags::NeedsContext>
                exec(planner, BehaviorChain<ReorganizationPlannerInput,
                                            ExtractionPlan>{});
            *result_ptr = co_await exec.execute_with_context(scope, input);
        });

    rt.submit(std::move(task), "run_planner").wait();
    rt.shutdown();
    return result;
}

// Test trace layout:
// Line 0: HH metadata
// Line 1: FH metadata
// Line 2: POSIX read
// Line 3: POSIX write
// Line 4: APP compute
// Line 5: POSIX read
static std::string create_test_trace(const std::string& dir) {
    std::string plain_path = dir + "/trace.pfw";
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
                              .with_manifest(true);
            *result_ptr = co_await exec.execute_with_context(scope, config);
        });

    rt.submit(std::move(task), "build-idx").wait();
    rt.shutdown();

    if (!result.success) {
        throw std::runtime_error("Failed to build .idx for test: " +
                                 result.error_message);
    }
}

// Read all non-empty lines from a plain text file
static std::vector<std::string> read_lines(const std::string& path) {
    std::ifstream ifs(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(ifs, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

// Check if a JSON line contains "field":"value"
static bool line_contains(const std::string& line, const std::string& field,
                          const std::string& value) {
    std::string pattern = "\"" + field + "\":\"" + value + "\"";
    return line.find(pattern) != std::string::npos;
}

// Extract lines from a source file according to the extraction plan and write
// to group output files. Mirrors the logic in dftracer_organize.
static void execute_extraction(const ExtractionPlan& plan,
                               const std::string& index_dir,
                               std::map<std::string, FILE*>& group_files) {
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

        std::map<std::uint32_t, std::vector<std::string>> line_routing;
        for (const auto* task : tasks) {
            for (auto ln : task->line_numbers) {
                line_routing[ln].push_back(task->target_group);
            }
        }

        std::string index_path =
            internal::determine_index_path(src.file_path, index_dir);
        auto reader_input =
            IndexedReadInput::from_file(src.file_path).with_index(index_path);
        IndexedFileReaderUtility reader_utility;
        auto reader = reader_utility.process(reader_input).get();

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

// A segment interval for binary search during streaming
struct SegmentInterval {
    int line_start;
    int line_end;
    std::string original_path;
    int source_checkpoint;
};

// Find which segment a global line number belongs to
static const SegmentInterval* find_segment(
    const std::vector<SegmentInterval>& intervals, int line_number) {
    auto it = std::upper_bound(
        intervals.begin(), intervals.end(), line_number,
        [](int ln, const SegmentInterval& seg) { return ln < seg.line_start; });
    if (it != intervals.begin()) {
        --it;
        if (line_number >= it->line_start && line_number < it->line_end) {
            return &(*it);
        }
    }
    return nullptr;
}

static void write_group_provenance(
    const ExtractionPlan& plan,
    const std::map<std::string, std::string>& group_gz_paths,
    const std::string& reorg_dir) {
    for (const auto& g : plan.groups) {
        auto gz_it = group_gz_paths.find(g.name);
        if (gz_it == group_gz_paths.end()) continue;
        const std::string& gz_path = gz_it->second;

        std::string db_root =
            internal::determine_provenance_index_path(gz_path, reorg_dir);

        ProvenanceDatabase pdb(db_root);
        pdb.init_schema();
        int fid = pdb.get_or_create_file_info(gz_path, 0);
        REQUIRE(fid >= 0);

        pdb.insert_info(fid, "version", "1.0");
        pdb.insert_info(fid, "tool", "dftracer_organize");
        pdb.insert_group(fid, g.name, g.query);

        for (std::size_t si = 0; si < plan.source_files.size(); ++si) {
            const auto& src = plan.source_files[si];
            pdb.insert_source(fid, static_cast<int>(si), src.file_path,
                              static_cast<int>(src.num_checkpoints), "");
        }

        std::map<std::size_t, std::map<std::uint64_t, std::size_t>>
            segment_events;
        for (const auto& task : plan.tasks) {
            if (task.target_group == g.name) {
                segment_events[task.source_file_idx][task.checkpoint_idx] =
                    task.line_numbers.size();
            }
        }

        int output_line = 0;
        for (const auto& [src_idx, ckpts] : segment_events) {
            for (const auto& [ckpt, count] : ckpts) {
                pdb.insert_segment(fid, static_cast<int>(src_idx),
                                   static_cast<int>(ckpt), /*seq=*/0,
                                   output_line,
                                   output_line + static_cast<int>(count),
                                   static_cast<int>(count));
                output_line += static_cast<int>(count);
            }
        }
    }
}

TEST_SUITE("ReconstructIntegration") {
    TEST_CASE("Round-trip: reorganize then reconstruct") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_recon_integ").string();
        std::string input_dir = test_dir + "/input";
        std::string reorg_dir = test_dir + "/reorg";
        std::string recon_dir = test_dir + "/reconstruct";
        fs::create_directories(input_dir);
        fs::create_directories(reorg_dir);
        fs::create_directories(recon_dir);

        // Step 1: Create and index test trace
        std::string trace_file = create_test_trace(input_dir);
        build_idx(trace_file, input_dir);

        // Step 2: Plan reorganization
        ReorganizationPlannerInput planner_input;
        planner_input.source_files = {trace_file};
        planner_input.groups = {{"io", R"(cat == "POSIX")"},
                                {"compute", R"(cat == "APP")"}};
        planner_input.index_dir = input_dir;

        auto plan = run_planner(planner_input);
        REQUIRE(plan.tasks.size() > 0);

        // Step 3: Execute extraction
        std::map<std::string, FILE*> group_files;
        std::map<std::string, std::string> group_pfw_paths;
        for (const auto& g : plan.groups) {
            std::string pfw_path = reorg_dir + "/" + g.name + ".pfw";
            FILE* f = std::fopen(pfw_path.c_str(), "w");
            REQUIRE(f != nullptr);
            group_files[g.name] = f;
            group_pfw_paths[g.name] = pfw_path;
        }

        execute_extraction(plan, input_dir, group_files);

        for (auto& [gname, f] : group_files) {
            std::fclose(f);
        }

        // Step 4: Compress outputs
        std::map<std::string, std::string> group_gz_paths;
        for (const auto& g : plan.groups) {
            std::string pfw_path = group_pfw_paths[g.name];
            if (!fs::exists(pfw_path) || fs::file_size(pfw_path) == 0) {
                continue;
            }
            FileCompressorUtility compressor;
            auto comp_result =
                compressor
                    .process(FileCompressionUtilityInput::from_file(pfw_path))
                    .get();
            REQUIRE(comp_result.success);
            std::string gz_path = pfw_path + ".gz";
            REQUIRE(fs::exists(gz_path));
            fs::remove(pfw_path);
            group_gz_paths[g.name] = gz_path;
        }

        // Step 5: Build .idx for each compressed output
        for (const auto& [gname, gz_path] : group_gz_paths) {
            build_idx(gz_path, reorg_dir);
        }

        // Step 6: Write provenance into the shared output-root .dftindex
        write_group_provenance(plan, group_gz_paths, reorg_dir);

        // Step 7: Plan reconstruction
        std::vector<std::string> reorg_files;
        for (const auto& [gname, gz_path] : group_gz_paths) {
            reorg_files.push_back(gz_path);
        }

        ReconstructionPlannerUtility recon_planner;
        ReconstructionPlannerInput recon_input;
        recon_input.reorganized_files = reorg_files;
        recon_input.index_dir = reorg_dir;

        auto recon_plan = recon_planner.process(recon_input).get();

        // Step 8: Verify reconstruction plan
        CHECK(recon_plan.files.size() == 1);
        CHECK(recon_plan.total_segments > 0);
        CHECK(recon_plan.total_events > 0);

        // Step 9: Execute reconstruction
        std::map<std::string, std::map<int, std::vector<std::string>>> buffers;

        std::map<std::string, std::vector<SegmentInterval>> per_reorg_segments;

        for (const auto& [orig_path, recon] : recon_plan.files) {
            for (const auto& [ckpt, segs] : recon.checkpoint_segments) {
                for (const auto& seg : segs) {
                    SegmentInterval si;
                    si.line_start = seg.output_line_start;
                    si.line_end = seg.output_line_end;
                    si.original_path = orig_path;
                    si.source_checkpoint = seg.source_checkpoint;
                    per_reorg_segments[seg.reorg_file].push_back(std::move(si));
                }
            }
        }

        for (auto& [file, segs] : per_reorg_segments) {
            std::sort(segs.begin(), segs.end(),
                      [](const SegmentInterval& a, const SegmentInterval& b) {
                          return a.line_start < b.line_start;
                      });
        }

        for (const auto& [reorg_file, intervals] : per_reorg_segments) {
            std::string index_path =
                internal::determine_index_path(reorg_file, reorg_dir);

            MetadataCollectorUtility meta_collector;
            auto meta_input =
                MetadataCollectorUtilityInput::from_file(reorg_file)
                    .with_index(index_path);
            auto meta = meta_collector.process(meta_input).get();
            REQUIRE(meta.success);

            auto reader_input =
                IndexedReadInput::from_file(reorg_file).with_index(index_path);
            IndexedFileReaderUtility reader_utility;
            auto reader = reader_utility.process(reader_input).get();

            auto stream = reader->stream(
                reader::internal::StreamConfig()
                    .stream_type(
                        reader::internal::StreamType::MULTI_LINES_BYTES)
                    .range_type(reader::internal::RangeType::BYTE_RANGE)
                    .buffer_size(4 * 1024 * 1024)
                    .from(0)
                    .to(meta.uncompressed_size));

            int line_number = 0;
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

                    const auto* seg = find_segment(intervals, line_number);
                    if (seg) {
                        buffers[seg->original_path][seg->source_checkpoint]
                            .emplace_back(line_start, line_len);
                    }

                    pos = static_cast<std::size_t>(newline - data) + 1;
                    line_number++;
                }
            }
        }

        // Step 10: Write reconstructed file
        REQUIRE(buffers.size() == 1);
        const auto& [orig_path, ckpt_map] = *buffers.begin();

        std::string recon_pfw = recon_dir + "/reconstructed.pfw";
        {
            FILE* f = std::fopen(recon_pfw.c_str(), "w");
            REQUIRE(f != nullptr);

            for (const auto& [ckpt, lines] : ckpt_map) {
                for (const auto& line : lines) {
                    std::fwrite(line.data(), 1, line.size(), f);
                    std::fputc('\n', f);
                }
            }
            std::fclose(f);
        }

        // Step 11: Verify reconstructed file
        auto recon_lines = read_lines(recon_pfw);

        std::size_t posix_count = 0;
        std::size_t app_count = 0;
        bool has_hh = false;
        bool has_fh = false;
        for (const auto& line : recon_lines) {
            if (line_contains(line, "cat", "POSIX")) posix_count++;
            if (line_contains(line, "cat", "APP")) app_count++;
            if (line_contains(line, "name", "HH")) has_hh = true;
            if (line_contains(line, "name", "FH")) has_fh = true;
        }

        CHECK(posix_count == 3);
        CHECK(app_count == 1);
        CHECK(has_hh);
        CHECK(has_fh);
        CHECK(recon_lines.size() >= 6);

        fs::remove_all(test_dir);
    }

    TEST_CASE(
        "reconstruction planner reads multiple outputs from one shared "
        ".dftindex") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_recon_shared_root")
                .string();
        std::string input_dir = test_dir + "/input";
        std::string reorg_dir = test_dir + "/reorg";
        fs::create_directories(input_dir);
        fs::create_directories(reorg_dir);

        std::string trace_file = create_test_trace(input_dir);
        build_idx(trace_file, input_dir);

        ReorganizationPlannerInput planner_input;
        planner_input.source_files = {trace_file};
        planner_input.groups = {{"io", R"(cat == "POSIX")"},
                                {"compute", R"(cat == "APP")"}};
        planner_input.index_dir = input_dir;

        auto plan = run_planner(planner_input);
        REQUIRE(plan.tasks.size() > 0);

        std::map<std::string, FILE*> group_files;
        std::map<std::string, std::string> group_pfw_paths;
        for (const auto& g : plan.groups) {
            std::string pfw_path = reorg_dir + "/" + g.name + ".pfw";
            FILE* f = std::fopen(pfw_path.c_str(), "w");
            REQUIRE(f != nullptr);
            group_files[g.name] = f;
            group_pfw_paths[g.name] = pfw_path;
        }

        execute_extraction(plan, input_dir, group_files);

        for (auto& [_, f] : group_files) {
            std::fclose(f);
        }

        std::map<std::string, std::string> group_gz_paths;
        for (const auto& g : plan.groups) {
            std::string pfw_path = group_pfw_paths[g.name];
            if (!fs::exists(pfw_path) || fs::file_size(pfw_path) == 0) continue;

            FileCompressorUtility compressor;
            auto comp_result =
                compressor
                    .process(FileCompressionUtilityInput::from_file(pfw_path))
                    .get();
            REQUIRE(comp_result.success);

            std::string gz_path = pfw_path + ".gz";
            REQUIRE(fs::exists(gz_path));
            group_gz_paths[g.name] = gz_path;
            fs::remove(pfw_path);
        }

        for (const auto& [_, gz_path] : group_gz_paths) {
            build_idx(gz_path, reorg_dir);
        }

        write_group_provenance(plan, group_gz_paths, reorg_dir);

        const std::string shared_root =
            determine_provenance_index_path(trace_file, reorg_dir);
        REQUIRE(fs::exists(shared_root));

        ProvenanceDatabase pdb(shared_root);
        const int io_fid = pdb.get_file_info_id(group_gz_paths.at("io"));
        const int compute_fid =
            pdb.get_file_info_id(group_gz_paths.at("compute"));
        REQUIRE(io_fid >= 0);
        REQUIRE(compute_fid >= 0);
        CHECK(io_fid != compute_fid);
        CHECK(pdb.query_group_name(io_fid) == "io");
        CHECK(pdb.query_group_name(compute_fid) == "compute");

        ReconstructionPlannerUtility recon_planner;
        ReconstructionPlannerInput recon_input;
        for (const auto& [_, gz_path] : group_gz_paths) {
            recon_input.reorganized_files.push_back(gz_path);
        }
        recon_input.index_dir = reorg_dir;

        auto recon_plan = recon_planner.process(recon_input).get();
        REQUIRE(recon_plan.files.size() == 1);
        CHECK(recon_plan.total_segments >= 2);
        CHECK(recon_plan.total_events == 8);

        fs::remove_all(test_dir);
    }
}
