#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/tasks/task_context.h>
#include <dftracer/utils/core/tasks/task_scope.h>
#include <dftracer/utils/utilities/composites/dft/index_builder_utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_filter.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_index_schema.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_indexer_utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>

#include <argparse/argparse.hpp>
#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_set>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites::dft::indexing;

// Wrapper to carry file_path through the pipeline alongside indexer output
struct IndexResult {
    std::string file_path;
    ChunkIndexerOutput output;
};

// Intermediate struct carrying Phase 1 results into Phase 2
struct FileIndexInfo {
    std::string file_path;
    std::string idx_path;
    std::size_t uncompressed_size;
    std::size_t num_checkpoints;
};

int main(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    auto default_checkpoint_size_str =
        std::to_string(indexer::internal::Indexer::DEFAULT_CHECKPOINT_SIZE) +
        " B (" +
        std::to_string(indexer::internal::Indexer::DEFAULT_CHECKPOINT_SIZE /
                       (1024 * 1024)) +
        " MB)";

    argparse::ArgumentParser program("dftracer_index",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "Build per-chunk bloom filter indices for DFTracer trace files. "
        "Creates .bidx sidecar databases enabling fast chunk-skipping "
        "queries.");

    program.add_argument("-d", "--directory")
        .help("Input directory containing .pfw or .pfw.gz files")
        .default_value<std::string>(".");

    program.add_argument("--dimensions")
        .help(
            "Comma-separated extra dimensions to index from args "
            "(e.g., args.level,args.mode,args.io.size)")
        .default_value<std::string>("");

    program.add_argument("-f", "--force")
        .help("Force index recreation even if already built")
        .flag();

    program.add_argument("--checkpoint-size")
        .help("Checkpoint size for gzip indexing in bytes (default: " +
              default_checkpoint_size_str + ")")
        .scan<'d', std::size_t>()
        .default_value(static_cast<std::size_t>(
            indexer::internal::Indexer::DEFAULT_CHECKPOINT_SIZE));

    program.add_argument("--executor-threads")
        .help("Number of worker threads for parallel processing")
        .scan<'d', std::size_t>()
        .default_value(
            static_cast<std::size_t>(std::thread::hardware_concurrency()));

    program.add_argument("--scheduler-threads")
        .help("Number of scheduler threads (default: 1)")
        .scan<'d', std::size_t>()
        .default_value(static_cast<std::size_t>(1));

    program.add_argument("--index-dir")
        .help("Directory to store index files (default: same as data files)")
        .default_value<std::string>("");

    program.add_argument("--expected-entries")
        .help(
            "Expected entries per chunk for bloom filter sizing (default: "
            "1024)")
        .scan<'d', std::size_t>()
        .default_value(static_cast<std::size_t>(1024));

    program.add_argument("--false-positive-rate")
        .help("Bloom filter false positive rate (default: 0.01)")
        .scan<'g', double>()
        .default_value(0.01);

    program.add_argument("--read-batch-size")
        .help("Batch read size in MB for stream processing (default: 4)")
        .scan<'d', std::size_t>()
        .default_value(static_cast<std::size_t>(4));

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        DFTRACER_UTILS_LOG_ERROR("Error occurred: %s", err.what());
        std::fprintf(stderr, "%s\n", program.help().str().c_str());
        return 1;
    }

    std::string log_dir = program.get<std::string>("--directory");
    std::string dimensions_str = program.get<std::string>("--dimensions");
    bool force_rebuild = program.get<bool>("--force");
    std::size_t checkpoint_size = program.get<std::size_t>("--checkpoint-size");
    std::size_t executor_threads =
        program.get<std::size_t>("--executor-threads");
    std::size_t scheduler_threads =
        program.get<std::size_t>("--scheduler-threads");
    std::string index_dir = program.get<std::string>("--index-dir");
    std::size_t expected_entries =
        program.get<std::size_t>("--expected-entries");
    double false_positive_rate = program.get<double>("--false-positive-rate");
    std::size_t batch_size_mb = program.get<std::size_t>("--read-batch-size");

    auto split_string = [](const std::string& str) {
        std::vector<std::string> result;
        if (str.empty()) return result;
        std::stringstream ss(str);
        std::string item;
        while (std::getline(ss, item, ',')) {
            if (!item.empty()) {
                result.push_back(item);
            }
        }
        return result;
    };

    std::vector<std::string> extra_dimensions = split_string(dimensions_str);

    ChunkIndexerConfig indexer_config;
    indexer_config.extra_dimensions = extra_dimensions;
    indexer_config.expected_entries_per_chunk = expected_entries;
    indexer_config.false_positive_rate = false_positive_rate;

    std::vector<std::string> all_dimensions = {"name",  "cat",   "pid",  "tid",
                                               "hhash", "fhash", "shash"};
    for (const auto& dim : extra_dimensions) {
        all_dimensions.push_back(dim);
    }

    log_dir = fs::absolute(log_dir).string();

    std::printf("==========================================\n");
    std::printf("DFTracer Bloom Indexer\n");
    std::printf("==========================================\n");
    std::printf("Arguments:\n");
    std::printf("  Input directory: %s\n", log_dir.c_str());
    std::printf("  Force rebuild: %s\n", force_rebuild ? "true" : "false");
    std::printf("  Checkpoint size: %zu bytes (%.2f MB)\n", checkpoint_size,
                static_cast<double>(checkpoint_size) / (1024.0 * 1024.0));
    std::printf("  Executor threads: %zu\n", executor_threads);
    std::printf("  Expected entries/chunk: %zu\n", expected_entries);
    std::printf("  False positive rate: %.4f\n", false_positive_rate);
    if (!extra_dimensions.empty()) {
        std::printf("  Extra dimensions: ");
        for (std::size_t i = 0; i < extra_dimensions.size(); ++i) {
            std::printf("%s%s", extra_dimensions[i].c_str(),
                        i < extra_dimensions.size() - 1 ? ", " : "\n");
        }
    }
    std::printf("==========================================\n\n");

    // Discover input files
    filesystem::PatternDirectoryScannerUtility scanner;
    filesystem::PatternDirectoryScannerUtilityInput scan_input{
        log_dir, {".pfw", ".pfw.gz"}, false};
    auto matched_entries = scanner.process(scan_input);

    std::vector<std::string> input_files;
    input_files.reserve(matched_entries.size());
    for (const auto& entry : matched_entries) {
        input_files.push_back(entry.path.string());
    }

    if (input_files.empty()) {
        DFTRACER_UTILS_LOG_ERROR("No .pfw or .pfw.gz files found in: %s",
                                 log_dir.c_str());
        return 1;
    }

    DFTRACER_UTILS_LOG_INFO("Found %zu input files", input_files.size());

    auto pipeline_config = PipelineConfig()
                               .with_name("DFTracer Bloom Indexer")
                               .with_compute_threads(executor_threads)
                               .with_io_threads(executor_threads)
                               .with_scheduler_threads(scheduler_threads)
                               .with_watchdog(false);

    Pipeline pipeline(pipeline_config);

    auto start_time = std::chrono::high_resolution_clock::now();

    std::atomic<std::size_t> total_events{0};
    std::atomic<std::size_t> total_checkpoints_processed{0};
    std::atomic<std::size_t> total_files_processed{0};
    std::atomic<std::size_t> total_files_skipped{0};

    // Two-phase pipeline:
    //   Phase 1: Build gzip indices + collect metadata (I/O-heavy, all threads)
    //   Phase 2: Streaming bloom filter pipeline (CPU-heavy, all threads)
    std::mutex file_infos_mutex;
    std::vector<FileIndexInfo> file_infos;

    auto streaming_task = make_task(
        [&](TaskContext& ctx) -> coro::CoroTask<void> {
            // Phase 1: Parallel gzip index building + metadata collection
            co_await ctx.scope([&](TaskScope& scope) -> coro::CoroTask<void> {
                for (std::size_t i = 0; i < input_files.size(); ++i) {
                    scope.spawn([&, i](TaskContext& /*fctx*/)
                                    -> coro::CoroTask<void> {
                        const auto& file_path = input_files[i];

                        // Build gzip index
                        std::string idx_path =
                            composites::dft::internal::determine_index_path(
                                file_path, index_dir);
                        auto idx_input =
                            composites::dft::IndexBuildUtilityInput::from_file(
                                file_path)
                                .with_checkpoint_size(checkpoint_size)
                                .with_force_rebuild(force_rebuild)
                                .with_index(idx_path);
                        composites::dft::IndexBuilderUtility{}.process(
                            idx_input);

                        // Collect metadata
                        auto meta_input =
                            composites::dft::MetadataCollectorUtilityInput::
                                from_file(file_path)
                                    .with_checkpoint_size(checkpoint_size)
                                    .with_force_rebuild(false)
                                    .with_index(idx_path);
                        auto metadata =
                            composites::dft::MetadataCollectorUtility{}.process(
                                meta_input);

                        if (!metadata.success) {
                            DFTRACER_UTILS_LOG_WARN("Skipping file: %s",
                                                    file_path.c_str());
                            total_files_skipped++;
                            co_return;
                        }

                        // Check if already indexed (skip if not forced)
                        std::string bidx_path =
                            determine_bloom_index_path(file_path, index_dir);
                        bool needs_indexing = force_rebuild;

                        if (!force_rebuild && fs::exists(bidx_path)) {
                            try {
                                BloomIndexDatabase bidx(bidx_path);
                                bidx.init_schema();
                                int fid = bidx.get_file_info_id(file_path);
                                if (fid >= 0) {
                                    auto existing_dims =
                                        queries::query_index_dimensions(
                                            bidx.db(), fid);
                                    std::unordered_set<std::string>
                                        existing_set(existing_dims.begin(),
                                                     existing_dims.end());
                                    needs_indexing = false;
                                    for (const auto& dim : all_dimensions) {
                                        if (existing_set.find(dim) ==
                                            existing_set.end()) {
                                            needs_indexing = true;
                                            break;
                                        }
                                    }
                                } else {
                                    needs_indexing = true;
                                }
                            } catch (...) {
                                needs_indexing = true;
                            }
                        } else {
                            needs_indexing = true;
                        }

                        if (!needs_indexing) {
                            DFTRACER_UTILS_LOG_INFO(
                                "Skipping already-indexed file: %s",
                                file_path.c_str());
                            total_files_skipped++;
                            co_return;
                        }

                        // Store result for Phase 2
                        {
                            std::lock_guard<std::mutex> lock(file_infos_mutex);
                            file_infos.push_back({file_path, idx_path,
                                                  metadata.uncompressed_size,
                                                  metadata.num_checkpoints});
                        }
                        total_files_processed++;
                        co_return;
                    });
                }
                co_return;
            });

            // Phase 2: Streaming bloom filter pipeline
            if (file_infos.empty()) {
                co_return;
            }

            co_await ctx.scope([&](TaskScope& scope) -> coro::CoroTask<void> {
                auto chunk_chan = coro::make_channel<ChunkIndexerInput>(0);
                auto result_chan = coro::make_channel<IndexResult>(8);

                // Producers: emit chunks from pre-computed metadata (fast)
                scope.spawn_producers(
                    chunk_chan, file_infos.size(),
                    [&](TaskContext& /*fctx*/,
                        std::size_t idx) -> coro::CoroTask<void> {
                        const auto& info = file_infos[idx];
                        std::size_t file_size = info.uncompressed_size;
                        std::size_t num_ckpts = info.num_checkpoints;

                        if (num_ckpts == 0) {
                            ChunkIndexerInput ci;
                            ci.with_file_path(info.file_path)
                                .with_idx_path(info.idx_path)
                                .with_checkpoint_size(checkpoint_size)
                                .with_checkpoint_idx(0)
                                .with_byte_range(0, file_size)
                                .with_config(indexer_config)
                                .with_batch_size(batch_size_mb * 1024 * 1024);
                            chunk_chan->send_blocking(std::move(ci));
                        } else {
                            std::size_t bytes_per = file_size / num_ckpts;
                            for (std::size_t i = 0; i < num_ckpts; ++i) {
                                std::size_t start = i * bytes_per;
                                std::size_t end = (i + 1 == num_ckpts)
                                                      ? file_size
                                                      : (i + 1) * bytes_per;
                                ChunkIndexerInput ci;
                                ci.with_file_path(info.file_path)
                                    .with_idx_path(info.idx_path)
                                    .with_checkpoint_size(checkpoint_size)
                                    .with_checkpoint_idx(
                                        static_cast<std::uint64_t>(i))
                                    .with_byte_range(start, end)
                                    .with_config(indexer_config)
                                    .with_batch_size(batch_size_mb * 1024 *
                                                     1024);
                                chunk_chan->send_blocking(std::move(ci));
                            }
                        }

                        co_return;
                    });

                // Chunk workers: parallel bloom filter indexing
                scope.spawn_transforms(
                    chunk_chan, result_chan, executor_threads,
                    [&](TaskContext& /*wctx*/, ChunkIndexerInput input)
                        -> coro::CoroTask<IndexResult> {
                        ChunkIndexerUtility idx;
                        IndexResult result;
                        result.file_path = input.file_path;
                        result.output = idx.process(input);
                        total_events += result.output.events_processed;
                        total_checkpoints_processed++;
                        co_return result;
                    });

                // Persistence writer: groups by file, batch-writes to .bidx
                scope.spawn([&](TaskContext& mctx) -> coro::CoroTask<void> {
                    std::unordered_map<std::string, std::vector<IndexResult>>
                        file_results;

                    while (auto result =
                               co_await mctx.receive_async(result_chan)) {
                        file_results[result->file_path].push_back(
                            std::move(*result));
                    }

                    for (auto& [fp, results] : file_results) {
                        std::string bidx_path =
                            determine_bloom_index_path(fp, index_dir);
                        BloomIndexDatabase bidx(bidx_path);
                        bidx.init_schema();

                        std::uint64_t file_hash = 0;
                        if (fs::exists(fp)) {
                            file_hash =
                                static_cast<std::uint64_t>(fs::file_size(fp));
                        }
                        int fid = bidx.get_or_create_file_info(fp, file_hash);

                        bidx.begin_transaction();
                        try {
                            std::unordered_map<std::string, BloomFilter>
                                file_blooms;
                            HashResolutions all_hr;

                            for (auto& r : results) {
                                if (!r.output.success) continue;

                                // Chunk bloom filters
                                for (auto& [dim, bloom] :
                                     r.output.bloom_filters) {
                                    auto blob = bloom.serialize();
                                    queries::insert_chunk_bloom_filter(
                                        bidx.db(), fid, r.output.checkpoint_idx,
                                        dim, blob.data(),
                                        static_cast<int>(blob.size()),
                                        bloom.num_entries());

                                    auto it = file_blooms.find(dim);
                                    if (it == file_blooms.end()) {
                                        file_blooms.emplace(dim,
                                                            std::move(bloom));
                                    } else {
                                        it->second.merge_from(bloom);
                                    }
                                }

                                // Chunk statistics
                                queries::insert_chunk_statistics(
                                    bidx.db(), fid, r.output.checkpoint_idx,
                                    r.output.statistics);

                                // Hash resolutions
                                for (auto& [dim, resolutions] :
                                     r.output.hash_resolutions) {
                                    for (auto& [hash, resolved] : resolutions) {
                                        all_hr[dim][hash] = resolved;
                                    }
                                }
                            }

                            // File-level bloom filters
                            for (auto& [dim, bloom] : file_blooms) {
                                auto blob = bloom.serialize();
                                queries::insert_file_bloom_filter(
                                    bidx.db(), fid, dim, blob.data(),
                                    static_cast<int>(blob.size()),
                                    bloom.num_entries());
                            }

                            // Hash resolutions
                            for (const auto& [dim, resolutions] : all_hr) {
                                for (const auto& [hash, resolved] :
                                     resolutions) {
                                    queries::insert_hash_resolution(
                                        bidx.db(), fid, dim, hash, resolved);
                                }
                            }

                            // Index dimensions
                            for (const auto& dim : all_dimensions) {
                                queries::insert_index_dimension(bidx.db(), fid,
                                                                dim);
                            }

                            bidx.commit_transaction();

                            DFTRACER_UTILS_LOG_INFO(
                                "Persisted bloom index for %s "
                                "(%zu checkpoints, %zu dimensions)",
                                fp.c_str(), results.size(),
                                all_dimensions.size());
                        } catch (const std::exception& e) {
                            DFTRACER_UTILS_LOG_ERROR(
                                "Failed to persist bloom index for %s: %s",
                                fp.c_str(), e.what());
                        }
                    }

                    co_return;
                });

                co_return;
            });

            co_return;
        },
        "StreamingIndex");

    pipeline.set_source(streaming_task);
    pipeline.set_destination(streaming_task);
    pipeline.execute();

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    std::printf("\n");
    std::printf("==========================================\n");
    std::printf("Bloom Index Results\n");
    std::printf("==========================================\n");
    std::printf("  Execution time: %.2f seconds\n", duration.count() / 1000.0);
    std::printf("  Files processed: %zu\n", total_files_processed.load());
    std::printf("  Files skipped: %zu\n", total_files_skipped.load());
    std::printf("  Checkpoints indexed: %zu\n",
                total_checkpoints_processed.load());
    std::printf("  Events processed: %zu\n", total_events.load());
    std::printf("  Dimensions indexed: %zu\n", all_dimensions.size());
    std::printf("  Dimensions: ");
    for (std::size_t i = 0; i < all_dimensions.size(); ++i) {
        std::printf("%s%s", all_dimensions[i].c_str(),
                    i < all_dimensions.size() - 1 ? ", " : "\n");
    }
    std::printf("==========================================\n");

    return 0;
}
