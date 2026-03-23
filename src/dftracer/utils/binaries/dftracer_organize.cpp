#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/io/io.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/reorganization_planner.h>
#include <dftracer/utils/utilities/composites/file_compressor_utility.h>
#include <dftracer/utils/utilities/composites/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/composites/types.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>
#include <dftracer/utils/utilities/indexer/provenance_database.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>
#include <fcntl.h>
#include <unistd.h>

#include <argparse/argparse.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites;
using namespace dftracer::utils::utilities::composites::dft;
using namespace dftracer::utils::utilities::composites::dft::reorganize;
using namespace dftracer::utils::utilities::indexer;

namespace {

coro::CoroTask<int> run_organize(
    const std::string& /*directory*/, const std::string& output_dir,
    const std::string& index_dir, const std::vector<std::string>& files,
    const std::vector<PredicateGroup>& groups, std::size_t checkpoint_size,
    bool force_rebuild, bool no_compress, std::size_t executor_threads) {
    std::printf(
        "========================================"
        "==\n");
    std::printf("DFTracer Trace Reorganizer\n");
    std::printf(
        "========================================"
        "==\n");
    std::printf("  Input files: %zu\n", files.size());
    std::printf("  Output directory: %s\n", output_dir.c_str());
    std::printf("  Groups: %zu\n", groups.size());
    for (const auto& g : groups) {
        std::printf("    %s: %s\n", g.name.c_str(),
                    g.query.empty() ? "(remainder)" : g.query.c_str());
    }
    std::printf(
        "========================================"
        "==\n\n");

    auto start_time = std::chrono::high_resolution_clock::now();

    // Step 1: Auto-build .idx for files that need it
    std::printf("Step 1: Building indices...\n");
    {
        auto pipeline_config = PipelineConfig()
                                   .with_name("Organize: Build IDX")
                                   .with_compute_threads(executor_threads)
                                   .with_watchdog(false);

        Pipeline pipeline(pipeline_config);

        std::atomic<std::size_t> built_count{0};
        std::atomic<std::size_t> skipped_count{0};

        auto build_task = make_task(
            [&](CoroScope& ctx) -> coro::CoroTask<void> {
                co_await ctx.scope(
                    [&](CoroScope& scope) -> coro::CoroTask<void> {
                        auto* built_count_ptr = &built_count;
                        auto* skipped_count_ptr = &skipped_count;
                        for (std::size_t i = 0; i < files.size(); ++i) {
                            const auto file_path = files[i];
                            scope.spawn([file_path, index_dir, checkpoint_size,
                                         force_rebuild, built_count_ptr,
                                         skipped_count_ptr](CoroScope& /*fctx*/)
                                            -> coro::CoroTask<void> {
                                auto config =
                                    IndexBuildConfig::for_file(file_path)
                                        .with_index_dir(index_dir)
                                        .with_checkpoint_size(checkpoint_size)
                                        .with_force_rebuild(force_rebuild)
                                        .with_manifest(true)
                                        .with_index_threshold(0);

                                IndexBuilderUtility builder;
                                auto result = co_await builder.process(config);

                                if (result.was_skipped) {
                                    (*skipped_count_ptr)++;
                                } else if (result.success) {
                                    (*built_count_ptr)++;
                                } else {
                                    DFTRACER_UTILS_LOG_ERROR(
                                        "IDX "
                                        "build "
                                        "failed"
                                        " for "
                                        "%s: "
                                        "%s",
                                        file_path.c_str(),
                                        result.error_message.c_str());
                                }
                                co_return;
                            });
                        }
                        co_return;
                    });
                co_return;
            },
            "BuildIDX");

        pipeline.set_source(build_task);
        pipeline.set_destination(build_task);
        pipeline.execute();

        std::printf("  Built: %zu, Skipped: %zu\n", built_count.load(),
                    skipped_count.load());
    }

    // Step 2: Build extraction plan
    std::printf("Step 2: Building extraction plan...\n");
    ReorganizationPlannerUtility planner;
    ReorganizationPlannerInput planner_input;
    planner_input.source_files = files;
    planner_input.groups = groups;
    planner_input.index_dir = index_dir;
    planner_input.checkpoint_size = checkpoint_size;

    ExtractionPlan plan;
    try {
        plan = co_await planner.process(planner_input);
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Planning failed: %s", e.what());
        co_return 1;
    }

    std::printf("  Groups: %zu\n", plan.groups.size());
    std::printf("  Source files: %zu\n", plan.source_files.size());
    std::printf("  Extraction tasks: %zu\n", plan.tasks.size());
    std::printf("  Total events: %zu\n", plan.total_events);

    if (plan.tasks.empty()) {
        std::printf("No events to extract.\n");
        co_return 0;
    }

    // Step 3: Extract and route lines
    std::printf(
        "Step 3: Extracting and routing "
        "lines...\n");

    // Open per-group output files
    std::map<std::string, int> group_fds;
    std::map<std::string, std::string> group_pfw_paths;
    for (const auto& g : plan.groups) {
        std::string pfw_path = output_dir + "/" + g.name + ".pfw";
        ssize_t open_result = co_await io::open(
            pfw_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (open_result < 0) {
            DFTRACER_UTILS_LOG_ERROR("Failed to open output: %s",
                                     pfw_path.c_str());
            co_return 1;
        }
        int fd = static_cast<int>(open_result);
        group_fds[g.name] = fd;
        group_pfw_paths[g.name] = pfw_path;
    }

    // Group tasks by (source_file_idx,
    // checkpoint_idx) so we process each
    // checkpoint once
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

    std::atomic<std::size_t> lines_written{0};
    std::atomic<std::size_t> checkpoints_processed{0};

    // 256KB write buffers per group file
    constexpr std::size_t WRITE_BUFFER_SIZE = 256 * 1024;
    std::unordered_map<std::string, std::vector<char>> write_buffers;

    // Process checkpoints sequentially per source
    // file (output files are shared, so we
    // serialize writes)
    for (const auto& [ckpt_key, tasks] : checkpoint_tasks) {
        const auto& src = plan.source_files[ckpt_key.source_file_idx];

        // Build line routing table:
        // line_number -> list of group names
        std::map<std::uint32_t, std::vector<std::string>> line_routing;
        for (const auto* task : tasks) {
            for (auto ln : task->line_numbers) {
                line_routing[ln].push_back(task->target_group);
            }
        }

        // Create reader for this source file
        std::string idx_path = composites::dft::internal::determine_index_path(
            src.file_path, index_dir);
        auto reader_input = IndexedReadInput::from_file(src.file_path)
                                .with_index(idx_path)
                                .with_checkpoint_size(src.checkpoint_size > 0
                                                          ? src.checkpoint_size
                                                          : checkpoint_size);
        IndexedFileReaderUtility reader_utility;
        std::shared_ptr<reader::internal::Reader> reader;
        try {
            reader = co_await reader_utility.process(reader_input);
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_ERROR(
                "Failed to create reader "
                "for %s: %s",
                src.file_path.c_str(), e.what());
            continue;
        }

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
            auto chunk = co_await stream->read_async();
            if (chunk.empty()) break;

            const char* data = chunk.data();
            std::size_t bytes_read = chunk.size();
            std::size_t pos = 0;

            while (pos < bytes_read) {
                const char* line_start = data + pos;
                const char* newline = static_cast<const char*>(
                    std::memchr(line_start, '\n', bytes_read - pos));
                if (!newline) break;
                std::size_t line_len = newline - line_start;

                auto it = line_routing.find(line_number);
                if (it != line_routing.end()) {
                    for (const auto& gname : it->second) {
                        auto fit = group_fds.find(gname);
                        if (fit != group_fds.end()) {
                            auto& buf = write_buffers[gname];
                            buf.insert(buf.end(), line_start,
                                       line_start + line_len);
                            buf.push_back('\n');
                            if (buf.size() >= WRITE_BUFFER_SIZE) {
                                co_await io::write(fit->second, buf.data(),
                                                   buf.size());
                                buf.clear();
                            }
                            lines_written++;
                        }
                    }
                }

                pos = static_cast<std::size_t>(newline - data) + 1;
                line_number++;
            }
        }

        checkpoints_processed++;
    }

    // Flush remaining write buffers
    for (auto& [gname, buf] : write_buffers) {
        if (!buf.empty()) {
            auto fit = group_fds.find(gname);
            if (fit != group_fds.end()) {
                co_await io::write(fit->second, buf.data(), buf.size());
            }
            buf.clear();
        }
    }

    // Close all output files
    for (auto& [gname, fd] : group_fds) {
        co_await io::close(fd);
    }

    std::printf("  Checkpoints processed: %zu\n", checkpoints_processed.load());
    std::printf("  Lines written: %zu\n", lines_written.load());

    // Step 4: Compress output files
    if (!no_compress) {
        std::printf(
            "Step 4: Compressing output "
            "files...\n");
        for (const auto& [gname, pfw_path] : group_pfw_paths) {
            if (!fs::exists(pfw_path) || fs::file_size(pfw_path) == 0) {
                // Remove empty files
                fs::remove(pfw_path);
                continue;
            }

            FileCompressorUtility compressor;
            auto comp_input = FileCompressionUtilityInput::from_file(pfw_path);
            auto comp_result = co_await compressor.process(comp_input);

            if (comp_result.success) {
                // Remove plain .pfw after compression
                fs::remove(pfw_path);
                std::printf(
                    "  %s: %.2f MB -> %.2f MB"
                    " (%.1f%% reduction)\n",
                    gname.c_str(),
                    static_cast<double>(comp_result.original_size) /
                        (1024.0 * 1024.0),
                    static_cast<double>(comp_result.compressed_size) /
                        (1024.0 * 1024.0),
                    comp_result.compression_percentage());
            } else {
                DFTRACER_UTILS_LOG_ERROR(
                    "Compression failed for "
                    "%s: %s",
                    pfw_path.c_str(), comp_result.error_message.c_str());
            }
        }
    }

    // Step 5: Build sidecars for output files
    std::printf("Step 5: Building sidecars...\n");
    {
        auto pipeline_config = PipelineConfig()
                                   .with_name("Organize: Build Sidecars")
                                   .with_compute_threads(executor_threads)
                                   .with_watchdog(false);

        Pipeline pipeline(pipeline_config);

        // Collect output files that exist
        std::vector<std::string> output_files;
        for (const auto& [gname, pfw_path] : group_pfw_paths) {
            std::string gz_path = pfw_path + ".gz";
            if (!no_compress && fs::exists(gz_path)) {
                output_files.push_back(gz_path);
            } else if (no_compress && fs::exists(pfw_path)) {
                output_files.push_back(pfw_path);
            }
        }

        auto sidecar_task = make_task(
            [&](CoroScope& ctx) -> coro::CoroTask<void> {
                co_await ctx.scope(
                    [&](CoroScope& scope) -> coro::CoroTask<void> {
                        for (std::size_t i = 0; i < output_files.size(); ++i) {
                            const auto out_file = output_files[i];
                            scope.spawn([out_file, output_dir,
                                         checkpoint_size](CoroScope& /*fctx*/)
                                            -> coro::CoroTask<void> {
                                auto config =
                                    IndexBuildConfig::for_file(out_file)
                                        .with_index_dir(output_dir)
                                        .with_checkpoint_size(checkpoint_size)
                                        .with_force_rebuild(true)
                                        .with_manifest(true)
                                        .with_index_threshold(0);

                                IndexBuilderUtility builder;
                                auto result = co_await builder.process(config);

                                if (result.success) {
                                    std::printf(
                                        "  %s"
                                        ": "
                                        ".idx"
                                        " "
                                        "buil"
                                        "t "
                                        "(%zu"
                                        " "
                                        "even"
                                        "ts,"
                                        " "
                                        "%zu "
                                        "chun"
                                        "ks)"
                                        "\n",
                                        out_file.c_str(),
                                        result.events_processed,
                                        result.chunks_processed);
                                }

                                co_return;
                            });
                        }
                        co_return;
                    });
                co_return;
            },
            "BuildSidecars");

        pipeline.set_source(sidecar_task);
        pipeline.set_destination(sidecar_task);
        pipeline.execute();
    }

    // Step 6: Write provenance to output .pidx files
    std::printf("Step 6: Writing provenance...\n");
    for (const auto& g : plan.groups) {
        std::string out_file;
        std::string pfw_path = output_dir + "/" + g.name + ".pfw";
        std::string gz_path = pfw_path + ".gz";
        if (!no_compress && fs::exists(gz_path)) {
            out_file = gz_path;
        } else if (no_compress && fs::exists(pfw_path)) {
            out_file = pfw_path;
        } else {
            continue;
        }

        std::string idx_path = composites::dft::internal::determine_index_path(
            out_file, output_dir);
        if (!fs::exists(idx_path)) continue;

        std::string pidx_path =
            determine_provenance_index_path(out_file, output_dir);

        try {
            ProvenanceDatabase pdb(pidx_path);
            pdb.init_schema();

            std::uint64_t out_hash = 0;
            if (fs::exists(out_file)) {
                out_hash = static_cast<std::uint64_t>(fs::file_size(out_file));
            }
            int fid = pdb.get_or_create_file_info(out_file, out_hash);

            pdb.begin_transaction();

            pdb.insert_info("version", "1.0");
            pdb.insert_info("tool", "dftracer_organize");

            pdb.insert_group(g.name, g.query);

            for (std::size_t si = 0; si < plan.source_files.size(); ++si) {
                const auto& src = plan.source_files[si];
                pdb.insert_source(fid, static_cast<int>(si), src.file_path,
                                  static_cast<int>(src.num_checkpoints));
            }

            // Track which lines came from where
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
                    pdb.insert_segment(static_cast<int>(src_idx),
                                       static_cast<int>(ckpt), output_line,
                                       output_line + static_cast<int>(count),
                                       static_cast<int>(count));
                    output_line += static_cast<int>(count);
                }
            }

            pdb.commit_transaction();
            std::printf("  %s: provenance written\n", g.name.c_str());
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_ERROR(
                "Provenance write failed for "
                "%s: %s",
                g.name.c_str(), e.what());
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    std::printf(
        "\n========================================"
        "==\n");
    std::printf("Reorganization Complete\n");
    std::printf(
        "========================================"
        "==\n");
    std::printf("  Time: %.2f seconds\n", duration.count() / 1000.0);
    std::printf("  Input files: %zu\n", files.size());
    std::printf("  Output groups: %zu\n", plan.groups.size());
    std::printf("  Total events routed: %zu\n", plan.total_events);
    std::printf("  Lines written: %zu\n", lines_written.load());

    // List output files
    std::printf("  Output files:\n");
    for (const auto& g : plan.groups) {
        std::string pfw_path = output_dir + "/" + g.name + ".pfw";
        std::string gz_path = pfw_path + ".gz";
        if (!no_compress && fs::exists(gz_path)) {
            std::printf("    %s (%.2f MB)\n", gz_path.c_str(),
                        static_cast<double>(fs::file_size(gz_path)) /
                            (1024.0 * 1024.0));
        } else if (fs::exists(pfw_path)) {
            std::printf("    %s (%.2f MB)\n", pfw_path.c_str(),
                        static_cast<double>(fs::file_size(pfw_path)) /
                            (1024.0 * 1024.0));
        }
    }
    std::printf(
        "========================================"
        "==\n");

    co_return 0;
}

}  // namespace

int main(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    argparse::ArgumentParser program("dftracer_organize",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "Reorganize DFTracer trace files by routing "
        "events to predicate-based groups. Creates "
        "separate output files per group with "
        "provenance tracking.");

    program.add_argument("--files")
        .help("Input trace files (.pfw, .pfw.gz)")
        .nargs(argparse::nargs_pattern::any)
        .default_value<std::vector<std::string>>({});

    program.add_argument("-d", "--directory")
        .help("Directory containing trace files")
        .default_value<std::string>("");

    program.add_argument("-o", "--output")
        .help("Output directory (required)")
        .required();

    program.add_argument("--groups")
        .help(
            "Predicate groups: \"io:cat=POSIX\" "
            "\"compute:cat=APP\"")
        .nargs(argparse::nargs_pattern::at_least_one)
        .required();

    program.add_argument("--checkpoint-size")
        .help("Checkpoint size for indexing in bytes")
        .scan<'d', std::size_t>()
        .default_value(static_cast<std::size_t>(
            indexer::internal::Indexer::DEFAULT_CHECKPOINT_SIZE));

    program.add_argument("--index-dir")
        .help("Directory for sidecar files")
        .default_value<std::string>("");

    program.add_argument("-f", "--force")
        .help("Force rebuild of indices")
        .flag();

    program.add_argument("--no-compress")
        .help("Write plain .pfw instead of .pfw.gz")
        .flag();

    program.add_argument("--executor-threads")
        .help("Worker threads")
        .scan<'d', std::size_t>()
        .default_value(
            static_cast<std::size_t>(std::thread::hardware_concurrency()));

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        DFTRACER_UTILS_LOG_ERROR("Error: %s", err.what());
        std::cerr << program;
        return 1;
    }

    std::string directory = program.get<std::string>("--directory");
    std::string output_dir = program.get<std::string>("--output");
    std::string index_dir = program.get<std::string>("--index-dir");
    auto group_specs = program.get<std::vector<std::string>>("--groups");
    std::size_t checkpoint_size = program.get<std::size_t>("--checkpoint-size");
    bool force_rebuild = program.get<bool>("--force");
    bool no_compress = program.get<bool>("--no-compress");
    std::size_t executor_threads =
        program.get<std::size_t>("--executor-threads");

    // Create output directory
    fs::create_directories(output_dir);

    // Parse group specs
    auto groups = parse_group_specs(group_specs);
    if (groups.empty()) {
        DFTRACER_UTILS_LOG_ERROR("%s", "No groups specified.");
        return 1;
    }

    // Collect input files
    std::vector<std::string> files;
    if (!directory.empty()) {
        if (!fs::exists(directory)) {
            DFTRACER_UTILS_LOG_ERROR("Directory does not exist: %s",
                                     directory.c_str());
            return 1;
        }
        filesystem::PatternDirectoryScannerUtility scanner;
        filesystem::PatternDirectoryScannerUtilityInput scan_input{
            directory, {".pfw", ".pfw.gz"}, false};
        auto matched = scanner.process(scan_input).get();
        for (const auto& entry : matched) {
            files.push_back(entry.path.string());
        }
    } else {
        files = program.get<std::vector<std::string>>("--files");
    }

    if (files.empty()) {
        DFTRACER_UTILS_LOG_ERROR("%s",
                                 "No input files. "
                                 "Use --files or --directory.");
        return 1;
    }

    return run_organize(directory, output_dir, index_dir, files, groups,
                        checkpoint_size, force_rebuild, no_compress,
                        executor_threads)
        .get();
}
