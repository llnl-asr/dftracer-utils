#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/utilities/behaviors/behavior_chain.h>
#include <dftracer/utils/core/utilities/utility_executor.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_index_builder.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_indexer_utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/manifest_index_builder.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>

#include <argparse/argparse.hpp>
#include <atomic>
#include <chrono>
#include <sstream>
#include <thread>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites::dft::indexing;

static coro::CoroTask<int> run_index(argparse::ArgumentParser& program) {
    std::string log_dir = program.get<std::string>("--directory");
    std::string dimensions_str = program.get<std::string>("--dimensions");
    bool force_rebuild = program.get<bool>("--force");
    std::size_t checkpoint_size = program.get<std::size_t>("--checkpoint-size");
    std::size_t executor_threads =
        program.get<std::size_t>("--executor-threads");
    std::string index_dir = program.get<std::string>("--index-dir");
    std::size_t expected_entries =
        program.get<std::size_t>("--expected-entries");
    double false_positive_rate = program.get<double>("--false-positive-rate");
    std::size_t batch_size_mb = program.get<std::size_t>("--read-batch-size");
    bool build_manifest = program.get<bool>("--manifest");

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

    std::vector<std::string> all_dimensions = default_bloom_dimensions();
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
    std::printf("  Build manifest: %s\n", build_manifest ? "true" : "false");
    std::printf("==========================================\n\n");

    // Discover input files
    filesystem::PatternDirectoryScannerUtility scanner;
    filesystem::PatternDirectoryScannerUtilityInput scan_input{
        log_dir, {".pfw", ".pfw.gz"}, false};
    auto matched_entries = co_await scanner.process(scan_input);

    std::vector<std::string> input_files;
    input_files.reserve(matched_entries.size());
    for (const auto& entry : matched_entries) {
        input_files.push_back(entry.path.string());
    }

    if (input_files.empty()) {
        DFTRACER_UTILS_LOG_ERROR("No .pfw or .pfw.gz files found in: %s",
                                 log_dir.c_str());
        co_return 1;
    }

    DFTRACER_UTILS_LOG_INFO("Found %zu input files", input_files.size());

    auto pipeline_config = PipelineConfig()
                               .with_name("DFTracer Bloom Indexer")
                               .with_compute_threads(executor_threads)
                               .with_watchdog(false);

    Pipeline pipeline(pipeline_config);

    auto start_time = std::chrono::high_resolution_clock::now();

    std::atomic<std::size_t> total_events{0};
    std::atomic<std::size_t> total_checkpoints_processed{0};
    std::atomic<std::size_t> total_files_processed{0};
    std::atomic<std::size_t> total_files_skipped{0};

    // Build bloom index input template
    BloomIndexBuildInput build_template;
    build_template.index_dir = index_dir;
    build_template.checkpoint_size = checkpoint_size;
    build_template.indexer_config = indexer_config;
    build_template.batch_size = batch_size_mb * 1024 * 1024;
    build_template.force_rebuild = force_rebuild;
    build_template.dimensions = all_dimensions;

    auto streaming_task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            co_await ctx.scope([&](CoroScope& scope) -> coro::CoroTask<void> {
                auto* total_events_ptr = &total_events;
                auto* total_checkpoints_ptr = &total_checkpoints_processed;
                auto* total_processed_ptr = &total_files_processed;
                auto* total_skipped_ptr = &total_files_skipped;
                for (std::size_t i = 0; i < input_files.size(); ++i) {
                    const auto file_path = input_files[i];
                    scope.spawn([build_template, file_path, build_manifest,
                                 index_dir, checkpoint_size, batch_size_mb,
                                 force_rebuild, total_events_ptr,
                                 total_checkpoints_ptr, total_processed_ptr,
                                 total_skipped_ptr](
                                    CoroScope& fctx) -> coro::CoroTask<void> {
                        BloomIndexBuildInput build_input = build_template;
                        build_input.file_path = file_path;

                        auto utility =
                            std::make_shared<BloomIndexBuilderUtility>();
                        behaviors::BehaviorChain<BloomIndexBuildInput,
                                                 BloomIndexBuildOutput>
                            chain;
                        behaviors::UtilityExecutor<
                            BloomIndexBuildInput, BloomIndexBuildOutput,
                            utilities::tags::NeedsContext>
                            executor(utility, std::move(chain));

                        auto result = co_await executor.execute_with_context(
                            fctx, build_input);

                        if (result.was_skipped) {
                            (*total_skipped_ptr)++;
                        } else if (result.success) {
                            (*total_processed_ptr)++;
                            (*total_events_ptr) += result.events_processed;
                            (*total_checkpoints_ptr) += result.chunks_processed;
                        } else {
                            (*total_skipped_ptr)++;
                        }

                        if (build_manifest) {
                            ManifestIndexBuildInput manifest_input;
                            manifest_input.file_path = file_path;
                            manifest_input.index_dir = index_dir;
                            manifest_input.checkpoint_size = checkpoint_size;
                            manifest_input.batch_size =
                                batch_size_mb * 1024 * 1024;
                            manifest_input.force_rebuild = force_rebuild;

                            auto m_utility =
                                std::make_shared<ManifestIndexBuilderUtility>();
                            behaviors::BehaviorChain<ManifestIndexBuildInput,
                                                     ManifestIndexBuildOutput>
                                m_chain;
                            behaviors::UtilityExecutor<
                                ManifestIndexBuildInput,
                                ManifestIndexBuildOutput,
                                utilities::tags::NeedsContext>
                                m_executor(m_utility, std::move(m_chain));

                            auto m_result =
                                co_await m_executor.execute_with_context(
                                    fctx, manifest_input);

                            if (!m_result.success && !m_result.was_skipped) {
                                DFTRACER_UTILS_LOG_ERROR(
                                    "Manifest index failed "
                                    "for %s: %s",
                                    file_path.c_str(),
                                    m_result.error_message.c_str());
                            }
                        }

                        co_return;
                    });
                }
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
    if (build_manifest) {
        std::printf("  Manifest index: built\n");
    }
    std::printf("==========================================\n");

    co_return 0;
}

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

    program.add_argument("--manifest")
        .help(
            "Also build .midx manifest index "
            "(per-checkpoint event line routing)")
        .flag();

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        DFTRACER_UTILS_LOG_ERROR("Error occurred: %s", err.what());
        std::fprintf(stderr, "%s\n", program.help().str().c_str());
        return 1;
    }

    return run_index(program).get();
}
