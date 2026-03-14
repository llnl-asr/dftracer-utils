#include <dftracer/utils/core/common/archive_format.h>
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/utils/string.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>
#include <dftracer/utils/utilities/indexer/internal/indexer_factory.h>

#include <argparse/argparse.hpp>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <unordered_set>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::composites::dft;
using dftracer::utils::utilities::indexer::internal::Indexer;

static std::string format_size(std::uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_index = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unit_index < 4) {
        size /= 1024.0;
        unit_index++;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << size << " "
        << units[unit_index];
    return oss.str();
}

/// Zero-decompression path for summary mode: stat() for compressed size,
/// estimate uncompressed size and event count from empirical compression
/// ratio.  No file I/O beyond stat().  Handles multi-member gzip files
/// correctly (unlike ISIZE trailer which only covers the last member).
/// 60K files completes in seconds instead of minutes.
static coro::CoroTask<MetadataCollectorUtilityOutput> gz_trailer_info(
    std::string file_path) {
    MetadataCollectorUtilityOutput meta;
    meta.file_path = file_path;
    meta.has_index = false;
    meta.index_valid = false;

    try {
        meta.format = dftracer::utils::utilities::indexer::internal::
            IndexerFactory::detect_format(file_path);
        meta.compressed_size = fs::file_size(file_path);
        meta.size_mb =
            static_cast<double>(meta.compressed_size) / (1024.0 * 1024.0);

        // DFTracer JSON traces compress at ~20:1 with gzip.
        // Multi-member gzip files make the ISIZE trailer unreliable,
        // so estimate from compressed size instead.
        constexpr double COMPRESSION_RATIO = 20.0;
        constexpr double BYTES_PER_EVENT = 210.0;

        auto est_uncompressed = static_cast<std::uint64_t>(
            static_cast<double>(meta.compressed_size) * COMPRESSION_RATIO);
        meta.uncompressed_size = est_uncompressed;

        std::size_t est_events = static_cast<std::size_t>(
            static_cast<double>(est_uncompressed) / BYTES_PER_EVENT);
        if (est_events > 2) est_events -= 2;

        meta.num_lines = est_events + 2;
        meta.valid_events = est_events;
        meta.start_line = 1;
        meta.end_line = meta.num_lines;
        meta.size_per_line =
            (est_events > 0) ? meta.size_mb / static_cast<double>(est_events)
                             : 0;
        meta.success = true;
    } catch (const std::exception& e) {
        meta.error_message = e.what();
        meta.success = false;
    }

    co_return meta;
}

/// Detailed path for small compressed files: one streaming decompress pass,
/// count lines with JSON validation, no sidecar index created.
static coro::CoroTask<MetadataCollectorUtilityOutput> direct_scan_info(
    std::string file_path) {
    using dftracer::utils::utilities::fileio::lines::sources::
        async_streaming_gz_lines;

    MetadataCollectorUtilityOutput meta;
    meta.file_path = file_path;
    meta.has_index = false;
    meta.index_valid = false;

    try {
        meta.format = dftracer::utils::utilities::indexer::internal::
            IndexerFactory::detect_format(file_path);
        meta.compressed_size = fs::file_size(file_path);

        std::size_t total_lines = 0;
        std::size_t valid_events = 0;
        std::uint64_t total_bytes = 0;

        auto gen = async_streaming_gz_lines(file_path);
        while (auto line_opt = co_await gen.next()) {
            total_lines++;
            const auto& line = *line_opt;
            total_bytes += line.content.length();
            const char* trimmed;
            std::size_t trimmed_length;
            if (json_trim_and_validate(line.content.data(),
                                       line.content.length(), trimmed,
                                       trimmed_length) &&
                trimmed_length > 8) {
                valid_events++;
            }
        }

        meta.num_lines = total_lines;
        meta.valid_events = valid_events;
        meta.uncompressed_size = total_bytes;
        meta.size_mb =
            static_cast<double>(meta.compressed_size) / (1024.0 * 1024.0);
        meta.start_line = 1;
        meta.end_line = total_lines;
        meta.size_per_line =
            (valid_events > 0)
                ? meta.size_mb / static_cast<double>(valid_events)
                : 0;
        meta.success = true;
    } catch (const std::exception& e) {
        meta.error_message = e.what();
        meta.success = false;
    }

    co_return meta;
}

static void print_file_info(const MetadataCollectorUtilityOutput& info,
                            bool verbose) {
    std::printf("========================================\n");
    std::printf("File: %s\n", info.file_path.c_str());
    std::printf("========================================\n");

    if (!info.success) {
        std::printf("  Status: ERROR - %s\n", info.error_message.c_str());
        std::printf("\n");
        return;
    }

    // Basic Information
    std::printf("Basic Information:\n");
    std::printf("  Format: %s\n", get_format_name(info.format));
    std::printf("  Status: %s\n", "OK");

    // File Size Information
    std::printf("\nFile Size:\n");
    std::printf("  Compressed:   %12s (%llu bytes)\n",
                format_size(info.compressed_size).c_str(),
                (unsigned long long)info.compressed_size);
    std::printf("  Uncompressed: %12s (%llu bytes)\n",
                format_size(info.uncompressed_size).c_str(),
                (unsigned long long)info.uncompressed_size);

    if (info.compressed_size > 0 && info.uncompressed_size > 0 &&
        info.compressed_size != info.uncompressed_size) {
        double ratio =
            100.0 * (1.0 - static_cast<double>(info.compressed_size) /
                               static_cast<double>(info.uncompressed_size));
        double compression_factor =
            static_cast<double>(info.uncompressed_size) /
            static_cast<double>(info.compressed_size);
        std::printf(
            "  Savings:      %12s (%.2f%% reduction)\n",
            format_size(info.uncompressed_size - info.compressed_size).c_str(),
            ratio);
        std::printf("  Ratio:        %.2fx compression\n", compression_factor);
    }

    // Content Information
    std::printf("\nContent:\n");
    std::printf("  Total Lines: %llu\n", (unsigned long long)info.num_lines);
    std::printf("  Valid Events: %zu (estimated)\n", info.valid_events);

    if (info.num_lines > 0) {
        std::printf("  Avg Bytes/Line: %.2f bytes\n",
                    static_cast<double>(info.uncompressed_size) /
                        static_cast<double>(info.num_lines));
    }

    if (info.valid_events > 0) {
        std::printf("  Avg Bytes/Event: %.2f bytes\n",
                    static_cast<double>(info.uncompressed_size) /
                        static_cast<double>(info.valid_events));
    }

    // Index Information (always show if index-capable format)
    if (info.format == ArchiveFormat::GZIP ||
        info.format == ArchiveFormat::TAR_GZ) {
        std::printf("\nIndex Information:\n");
        std::printf("  Index File: %s\n", info.idx_path.empty()
                                              ? "(auto-generated)"
                                              : info.idx_path.c_str());
        std::printf("  Index Status: %s\n",
                    info.has_index ? (info.index_valid ? "Valid" : "Invalid")
                                   : "Not Created");

        if (info.has_index && info.index_valid) {
            std::printf("  Checkpoint Size: %s (%llu bytes)\n",
                        format_size(info.checkpoint_size).c_str(),
                        (unsigned long long)info.checkpoint_size);
            std::printf("  Number of Checkpoints: %zu\n", info.num_checkpoints);

            if (info.num_checkpoints > 0) {
                std::uint64_t avg_chunk =
                    info.uncompressed_size / info.num_checkpoints;
                std::uint64_t lines_per_checkpoint =
                    info.num_lines / info.num_checkpoints;
                std::printf("  Avg Chunk Size: %s (%llu bytes)\n",
                            format_size(avg_chunk).c_str(),
                            (unsigned long long)avg_chunk);
                std::printf("  Avg Lines/Checkpoint: %llu\n",
                            (unsigned long long)lines_per_checkpoint);

                // Calculate index overhead
                if (fs::exists(info.idx_path)) {
                    std::uint64_t index_size = fs::file_size(info.idx_path);
                    double index_overhead =
                        100.0 * static_cast<double>(index_size) /
                        static_cast<double>(info.compressed_size);
                    std::printf("  Index File Size: %s (%llu bytes)\n",
                                format_size(index_size).c_str(),
                                (unsigned long long)index_size);
                    std::printf("  Index Overhead: %.2f%% of compressed size\n",
                                index_overhead);
                }
            }
        }
    }

    // Detailed Statistics (verbose mode)
    if (verbose) {
        std::printf("\nDetailed Statistics:\n");
        std::printf("  Start Line: %zu\n", info.start_line);
        std::printf("  End Line: %zu\n", info.end_line);
        std::printf("  Size (MB): %.6f\n", info.size_mb);
        std::printf("  MB per Event: %.8f\n", info.size_per_line);

        // Performance estimates
        if (info.num_checkpoints > 0 && info.num_lines > 0) {
            std::uint64_t lines_per_checkpoint =
                info.num_lines / info.num_checkpoints;
            std::printf("\nRandom Access Performance:\n");
            std::printf("  Worst-case lines to scan: %llu (1 checkpoint)\n",
                        (unsigned long long)lines_per_checkpoint);
            std::printf(
                "  Best-case lines to scan: 1 (exact checkpoint hit)\n");
            std::printf("  Avg lines to scan: %llu (0.5 checkpoint)\n",
                        (unsigned long long)(lines_per_checkpoint / 2));
        }

        // Memory estimates
        if (info.checkpoint_size > 0) {
            std::printf("\nMemory Estimates:\n");
            std::printf("  Memory for 1 checkpoint: ~%s\n",
                        format_size(info.checkpoint_size).c_str());
            if (info.num_checkpoints > 0) {
                std::uint64_t total_memory_for_all =
                    info.checkpoint_size * info.num_checkpoints;
                std::printf("  Memory for all checkpoints: ~%s\n",
                            format_size(total_memory_for_all).c_str());
            }
        }
    }

    std::printf("\n");
}

int main(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    auto default_checkpoint_size_str =
        std::to_string(Indexer::DEFAULT_CHECKPOINT_SIZE) + " B (" +
        std::to_string(Indexer::DEFAULT_CHECKPOINT_SIZE / (1024 * 1024)) +
        " MB)";

    argparse::ArgumentParser program("dftracer_info",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "Display metadata and index information for DFTracer compressed files "
        "using composable utilities and pipeline processing");

    program.add_argument("--files")
        .help("Compressed files to inspect (GZIP, TAR.GZ)")
        .nargs(argparse::nargs_pattern::any)
        .default_value<std::vector<std::string>>({});

    program.add_argument("-d", "--directory")
        .help("Directory containing files to inspect")
        .default_value<std::string>("");

    program.add_argument("--query")
        .help(
            "Query type: summary (aggregate all files, default) or "
            "detailed (per-file output)")
        .default_value<std::string>("summary");

    program.add_argument("-v", "--verbose")
        .help("Show detailed information including index details")
        .flag();

    program.add_argument("-f", "--force-rebuild")
        .help("Force rebuild index files")
        .flag();

    program.add_argument("-c", "--checkpoint-size")
        .help("Checkpoint size for indexing in bytes (default: " +
              default_checkpoint_size_str + ")")
        .scan<'d', std::size_t>()
        .default_value(
            static_cast<std::size_t>(Indexer::DEFAULT_CHECKPOINT_SIZE));

    program.add_argument("--index-dir")
        .help("Directory to store index files (default: system temp directory)")
        .default_value<std::string>("");

    program.add_argument("--executor-threads")
        .help(
            "Number of executor threads for parallel processing (default: "
            "number of CPU cores)")
        .scan<'d', std::size_t>()
        .default_value(
            static_cast<std::size_t>(std::thread::hardware_concurrency()));

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        DFTRACER_UTILS_LOG_ERROR("Error occurred: %s", err.what());
        std::cerr << program;
        return 1;
    }

    // Parse arguments
    std::string directory = program.get<std::string>("--directory");
    std::string query_type = program.get<std::string>("--query");
    bool verbose = program.get<bool>("--verbose");
    bool force_rebuild = program.get<bool>("--force-rebuild");
    std::size_t checkpoint_size = program.get<std::size_t>("--checkpoint-size");
    std::string index_dir = program.get<std::string>("--index-dir");
    std::size_t executor_threads =
        program.get<std::size_t>("--executor-threads");

    bool summary_mode = (query_type != "detailed");

    // Collect files to process
    std::vector<std::string> files;
    if (!directory.empty()) {
        if (!fs::exists(directory)) {
            DFTRACER_UTILS_LOG_ERROR("Directory does not exist: %s",
                                     directory.c_str());
            return 1;
        }

        for (const auto& entry : fs::directory_iterator(directory)) {
            if (entry.is_regular_file()) {
                std::string path = entry.path().string();
                std::string ext = entry.path().extension().string();
                if (ext == ".gz") {
                    files.push_back(path);
                }
            }
        }

        if (files.empty()) {
            DFTRACER_UTILS_LOG_ERROR(
                "No compressed files found in directory: %s",
                directory.c_str());
            return 1;
        }
    } else {
        files = program.get<std::vector<std::string>>("--files");

        if (files.empty()) {
            DFTRACER_UTILS_LOG_ERROR(
                "%s", "No files or directory specified. Use --help for usage.");
            std::cerr << program;
            return 1;
        }
    }

    // Small files skip indexing to avoid creating sidecar files on
    // metadata-sensitive filesystems (e.g. Lustre).
    static constexpr std::size_t INDEX_SIZE_THRESHOLD = 8 * 1024 * 1024;
    std::unordered_set<std::string> small_files;
    for (const auto& file_path : files) {
        std::error_code ec;
        auto fsize = fs::file_size(file_path, ec);
        if (!ec && fsize > 0 && fsize < INDEX_SIZE_THRESHOLD) {
            small_files.insert(file_path);
        }
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    if (summary_mode) {
        // Summary: accumulate totals in workers, print once at the end.
        // No per-file storage, no sort, no per-file print.
        std::atomic<std::uint64_t> total_compressed{0};
        std::atomic<std::uint64_t> total_uncompressed{0};
        std::atomic<std::uint64_t> total_lines{0};
        std::atomic<std::uint64_t> total_valid_events{0};
        std::atomic<std::size_t> successful{0};
        std::atomic<std::size_t> failed{0};

        {
            auto pipeline_config = PipelineConfig()
                                       .with_name("DFTracer File Info")
                                       .with_compute_threads(executor_threads)
                                       .with_watchdog(false);

            Pipeline pipeline(pipeline_config);

            auto info_task = make_task(
                [&](CoroScope& ctx) -> coro::CoroTask<void> {
                    co_await ctx.scope([&](CoroScope& scope)
                                           -> coro::CoroTask<void> {
                        auto* files_ptr = &files;
                        auto* total_compressed_ptr = &total_compressed;
                        auto* total_uncompressed_ptr = &total_uncompressed;
                        auto* total_lines_ptr = &total_lines;
                        auto* total_valid_events_ptr = &total_valid_events;
                        auto* successful_ptr = &successful;
                        auto* failed_ptr = &failed;

                        auto file_chan = coro::make_channel<std::size_t>(
                            executor_threads * 2);

                        scope.spawn(
                            [ch = file_chan->producer(), files_ptr](
                                CoroScope&) mutable -> coro::CoroTask<void> {
                                auto guard = ch.guard();
                                for (std::size_t i = 0; i < files_ptr->size();
                                     ++i) {
                                    if (!co_await ch.send(i)) {
                                        co_return;
                                    }
                                }
                                co_return;
                            });

                        for (std::size_t w = 0; w < executor_threads; ++w) {
                            scope.spawn(
                                [file_chan, files_ptr, total_compressed_ptr,
                                 total_uncompressed_ptr, total_lines_ptr,
                                 total_valid_events_ptr, successful_ptr,
                                 failed_ptr](
                                    CoroScope&) -> coro::CoroTask<void> {
                                    while (auto fi_opt =
                                               co_await file_chan->receive()) {
                                        std::size_t fi = *fi_opt;
                                        const auto& fp = (*files_ptr)[fi];

                                        auto info =
                                            co_await gz_trailer_info(fp);

                                        if (info.success) {
                                            total_compressed_ptr->fetch_add(
                                                info.compressed_size,
                                                std::memory_order_relaxed);
                                            total_uncompressed_ptr->fetch_add(
                                                info.uncompressed_size,
                                                std::memory_order_relaxed);
                                            total_lines_ptr->fetch_add(
                                                info.num_lines,
                                                std::memory_order_relaxed);
                                            total_valid_events_ptr->fetch_add(
                                                info.valid_events,
                                                std::memory_order_relaxed);
                                            successful_ptr->fetch_add(
                                                1, std::memory_order_relaxed);
                                        } else {
                                            failed_ptr->fetch_add(
                                                1, std::memory_order_relaxed);
                                        }
                                    }
                                    co_return;
                                });
                        }
                        co_return;
                    });
                    co_return;
                },
                "CollectInfo");

            pipeline.set_source(info_task);
            pipeline.set_destination(info_task);
            pipeline.execute();
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> duration =
            end_time - start_time;

        auto tc = total_compressed.load();
        auto tu = total_uncompressed.load();
        auto tl = total_lines.load();
        auto tv = total_valid_events.load();
        auto ok = successful.load();
        auto bad = failed.load();

        std::printf("==========================================\n");
        std::printf("DFTracer File Info Summary\n");
        std::printf("==========================================\n");
        std::printf("  Total Files:        %zu\n", files.size());
        std::printf("  Successful:         %zu\n", ok);
        std::printf("  Failed:             %zu\n", bad);
        std::printf("  Total Lines:        %llu\n", (unsigned long long)tl);
        std::printf("  Valid Events:       %llu\n", (unsigned long long)tv);
        std::printf("  Total Compressed:   %s (%llu bytes)\n",
                    format_size(tc).c_str(), (unsigned long long)tc);
        std::printf("  Total Uncompressed: %s (%llu bytes)\n",
                    format_size(tu).c_str(), (unsigned long long)tu);

        if (tc > 0 && tu > 0 && tc != tu) {
            double ratio = 100.0 * (1.0 - static_cast<double>(tc) /
                                              static_cast<double>(tu));
            std::printf("  Compression:        %.2f%%\n", ratio);
        }

        if (tv > 0) {
            std::printf("  Avg Bytes/Event:    %.2f bytes\n",
                        static_cast<double>(tu) / static_cast<double>(tv));
        }

        std::printf("  Processing Time:    %.2f seconds\n",
                    duration.count() / 1000.0);
        std::printf("==========================================\n");

        return (bad == 0) ? 0 : 1;
    }

    // Detailed mode: per-file output (original behavior)
    struct IndexedResult {
        std::size_t index;
        MetadataCollectorUtilityOutput info;
    };

    std::vector<IndexedResult> results;
    std::mutex results_mutex;

    {
        auto pipeline_config = PipelineConfig()
                                   .with_name("DFTracer File Info")
                                   .with_compute_threads(executor_threads)
                                   .with_watchdog(false);

        Pipeline pipeline(pipeline_config);

        auto info_task = make_task(
            [&](CoroScope& ctx) -> coro::CoroTask<void> {
                co_await ctx.scope([&](CoroScope& scope)
                                       -> coro::CoroTask<void> {
                    auto* files_ptr = &files;
                    auto* results_ptr = &results;
                    auto* results_mutex_ptr = &results_mutex;

                    auto small_set =
                        std::make_shared<std::unordered_set<std::string>>(
                            small_files);

                    auto file_chan =
                        coro::make_channel<std::size_t>(executor_threads * 2);

                    scope.spawn(
                        [ch = file_chan->producer(), files_ptr](
                            CoroScope&) mutable -> coro::CoroTask<void> {
                            auto guard = ch.guard();
                            for (std::size_t i = 0; i < files_ptr->size();
                                 ++i) {
                                if (!co_await ch.send(i)) {
                                    co_return;
                                }
                            }
                            co_return;
                        });

                    for (std::size_t w = 0; w < executor_threads; ++w) {
                        scope.spawn([file_chan, files_ptr, checkpoint_size,
                                     force_rebuild, verbose, index_dir,
                                     small_set, results_ptr, results_mutex_ptr](
                                        CoroScope&) -> coro::CoroTask<void> {
                            while (auto fi_opt =
                                       co_await file_chan->receive()) {
                                std::size_t fi = *fi_opt;
                                const auto& file_path = (*files_ptr)[fi];
                                bool is_small = small_set->count(file_path) > 0;

                                MetadataCollectorUtilityOutput info;
                                if (is_small) {
                                    info = co_await direct_scan_info(file_path);
                                } else {
                                    auto input =
                                        MetadataCollectorUtilityInput::
                                            from_file(file_path)
                                                .with_checkpoint_size(
                                                    checkpoint_size)
                                                .with_force_rebuild(
                                                    force_rebuild)
                                                .with_count_lines(verbose);

                                    if (!index_dir.empty()) {
                                        input.with_index(
                                            internal::determine_index_path(
                                                file_path, index_dir));
                                    }

                                    MetadataCollectorUtility collector;
                                    info = co_await collector.process(input);
                                }

                                std::lock_guard<std::mutex> lock(
                                    *results_mutex_ptr);
                                results_ptr->push_back({fi, std::move(info)});
                            }
                            co_return;
                        });
                    }
                    co_return;
                });
                co_return;
            },
            "CollectInfo");

        pipeline.set_source(info_task);
        pipeline.set_destination(info_task);
        pipeline.execute();
    }

    std::sort(results.begin(), results.end(),
              [](const IndexedResult& a, const IndexedResult& b) {
                  return a.index < b.index;
              });

    std::uint64_t total_compressed = 0;
    std::uint64_t total_uncompressed = 0;
    std::uint64_t total_lines = 0;
    std::size_t successful = 0;

    for (const auto& r : results) {
        print_file_info(r.info, verbose);
        if (r.info.success) {
            successful++;
            total_compressed += r.info.compressed_size;
            total_uncompressed += r.info.uncompressed_size;
            total_lines += r.info.num_lines;
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    if (files.size() > 1) {
        std::printf("==========================================\n");
        std::printf("Summary\n");
        std::printf("==========================================\n");
        std::printf("Total Files: %zu\n", files.size());
        std::printf("Successful: %zu\n", successful);
        std::printf("Failed: %zu\n", files.size() - successful);
        std::printf("Total Lines: %llu\n", (unsigned long long)total_lines);
        std::printf("Total Compressed: %s\n",
                    format_size(total_compressed).c_str());
        std::printf("Total Uncompressed: %s\n",
                    format_size(total_uncompressed).c_str());

        if (total_uncompressed > 0) {
            double ratio =
                100.0 * (1.0 - static_cast<double>(total_compressed) /
                                   static_cast<double>(total_uncompressed));
            std::printf("Overall Compression: %.2f%%\n", ratio);
        }

        std::printf("Processing Time: %.2f seconds\n",
                    duration.count() / 1000.0);
    }

    return (successful == files.size()) ? 0 : 1;
}
