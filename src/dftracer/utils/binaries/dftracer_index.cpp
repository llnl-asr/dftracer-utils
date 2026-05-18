#include <concurrentqueue.h>
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_indexer_utility.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_sst_writer_context.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <sstream>

#include "common_cli.h"

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites::dft::indexing;
using namespace dftracer::utils::utilities::indexer;

class IndexArgParse : public cli::ArgParse {
   public:
    cli::DirectoryArgs directory;
    cli::PipelineArgs pipeline;
    cli::IndexingArgs indexing;

    std::string dimensions;
    bool manifest = false;
    bool rebuild_summaries = false;
    std::size_t read_batch_size = 4;
    std::size_t expected_entries = 1024;
    double false_positive_rate = 0.01;

    explicit IndexArgParse(argparse::ArgumentParser& p) : ArgParse(p) {
        indexing.index_dir_help =
            "Directory where .dftindex stores are created";
        indexing.force_help = "Force index recreation even if already built";
        schema(directory, pipeline, indexing);
    }

   protected:
    void register_args() override {
        parser()
            .add_argument("--dimensions")
            .help(
                "Comma-separated extra dimensions to index from args "
                "(e.g., args.level,args.mode,args.io.size)")
            .default_value<std::string>("");

        parser()
            .add_argument("--manifest")
            .help(
                "Also build manifest data in the .dftindex store "
                "(per-checkpoint event line routing)")
            .flag();

        parser()
            .add_argument("--rebuild-summaries")
            .help(
                "Rebuild ROOT_* aggregated summaries after ingest. Off by "
                "default; ROOT_* CFs are only used by summary tools like "
                "dftracer_info. Bloom-filter chunk-skipping queries do not "
                "need them.")
            .flag();

        parser()
            .add_argument("--read-batch-size")
            .help("Batch read size in MB for stream processing (default: 4)")
            .scan<'d', std::size_t>()
            .default_value(static_cast<std::size_t>(4));

        parser()
            .add_argument("--expected-entries")
            .help(
                "Expected entries per chunk for bloom filter sizing (default: "
                "1024)")
            .scan<'d', std::size_t>()
            .default_value(static_cast<std::size_t>(1024));

        parser()
            .add_argument("--false-positive-rate")
            .help("Bloom filter false positive rate (default: 0.01)")
            .scan<'g', double>()
            .default_value(0.01);
    }

    void post_parse() override {
        dimensions = parser().get<std::string>("--dimensions");
        manifest = parser().get<bool>("--manifest");
        rebuild_summaries = parser().get<bool>("--rebuild-summaries");
        read_batch_size = parser().get<std::size_t>("--read-batch-size");
        expected_entries = parser().get<std::size_t>("--expected-entries");
        false_positive_rate = parser().get<double>("--false-positive-rate");
    }
};

static coro::CoroTask<int> run_index(const IndexArgParse* cli) {
    const auto log_dir = fs::absolute(cli->directory.value).string();
    const auto& dimensions_str = cli->dimensions;
    const auto force_rebuild = cli->indexing.force;
    const auto checkpoint_size = cli->indexing.checkpoint_size;
    const auto executor_threads = cli->pipeline.executor_threads;
    // When --index-dir is not provided, place coord/staging/ingest DBs next to
    // the input data so they line up with each file's per-file index_path
    // (which determine_index_path(file, "") resolves to
    // <file_parent>/.dftindex). The top-level scanner is non-recursive, so all
    // input files share log_dir.
    const auto index_dir =
        cli->indexing.index_dir.empty() ? log_dir : cli->indexing.index_dir;
    const auto expected_entries = cli->expected_entries;
    const auto false_positive_rate = cli->false_positive_rate;
    const auto build_manifest = cli->manifest;
    const auto rebuild_summaries = cli->rebuild_summaries;

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

    std::vector<std::string> user_dimensions = split_string(dimensions_str);

    std::vector<std::string> extra_dimensions(
        dftracer::utils::utilities::indexer::DEFAULT_EXTRA_DIMENSIONS.begin(),
        dftracer::utils::utilities::indexer::DEFAULT_EXTRA_DIMENSIONS.end());
    for (const auto& dim : user_dimensions) {
        if (std::find(extra_dimensions.begin(), extra_dimensions.end(), dim) ==
            extra_dimensions.end()) {
            extra_dimensions.push_back(dim);
        }
    }

    ChunkIndexerConfig indexer_config;
    indexer_config.extra_dimensions = extra_dimensions;
    indexer_config.expected_entries_per_chunk = expected_entries;
    indexer_config.false_positive_rate = false_positive_rate;

    std::vector<std::string> all_dimensions(
        dftracer::utils::utilities::indexer::DEFAULT_BLOOM_DIMENSIONS.begin(),
        dftracer::utils::utilities::indexer::DEFAULT_BLOOM_DIMENSIONS.end());
    for (const auto& dim : extra_dimensions) {
        all_dimensions.push_back(dim);
    }

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

    auto pipeline_config =
        cli::build_pipeline_config("DFTracer Bloom Indexer", cli->pipeline);

    Pipeline pipeline(pipeline_config);

    auto start_time = std::chrono::high_resolution_clock::now();

    std::vector<int> preassigned_file_ids;
    {
        IndexDatabase coord_db(index_dir);
        coord_db.init_schema();
        preassigned_file_ids =
            coord_db.register_files(input_files, build_manifest);
    }

    const std::string staging_root =
        (fs::path(index_dir) / ".dftindex_staging").string();
    fs::create_directories(staging_root);

    auto artifacts_queue = std::make_shared<moodycamel::ConcurrentQueue<
        IndexDatabaseSstWriterContext::Artifacts>>();
    auto batch_counter = std::make_shared<std::atomic<std::size_t>>(0);

    auto batch_config = std::make_shared<IndexBuildBatchConfig>();
    batch_config->file_paths = std::move(input_files);
    batch_config->preassigned_file_ids = std::move(preassigned_file_ids);
    batch_config->index_dir = index_dir;
    batch_config->checkpoint_size = checkpoint_size;
    batch_config->parallelism = executor_threads;
    batch_config->force_rebuild = force_rebuild;
    batch_config->build_manifest = build_manifest;
    batch_config->bloom_config = indexer_config;
    batch_config->bloom_dimensions = all_dimensions;
    batch_config->rebuild_root_summaries = false;

    batch_config->sink_factory =
        [staging_root, batch_counter]() -> std::unique_ptr<IndexBatchSink> {
        const std::size_t idx =
            batch_counter->fetch_add(1, std::memory_order_relaxed);
        return std::make_unique<IndexDatabaseSstWriterContext>(
            staging_root, "batch_" + std::to_string(idx));
    };
    batch_config->sink_commit = [artifacts_queue](IndexBatchSink& sink) {
        auto& sst = static_cast<IndexDatabaseSstWriterContext&>(sink);
        auto a = sst.commit();
        if (!a.empty()) artifacts_queue->enqueue(std::move(a));
    };

    IndexBuildBatchResult batch_result;
    auto streaming_task = make_task(
        [&batch_result,
         batch_config](CoroScope& scope) -> coro::CoroTask<void> {
            batch_result = co_await IndexBatchBuilderUtility::process(
                &scope, std::move(batch_config));
        },
        "StreamingIndex");

    pipeline.set_source(streaming_task);
    pipeline.set_destination(streaming_task);
    pipeline.execute();

    SstArtifactRegistry registry;
    {
        IndexDatabaseSstWriterContext::Artifacts a;
        while (artifacts_queue->try_dequeue(a)) {
            registry.append(std::move(a));
        }
    }
    DFTRACER_UTILS_LOG_INFO(
        "dftracer_index: %zu SSTs in registry (chunk_bloom=%zu file_bloom=%zu)",
        registry.chunk_bloom().size() + registry.file_bloom().size() +
            registry.chunk_stats().size(),
        registry.chunk_bloom().size(), registry.file_bloom().size());
    {
        IndexDatabase ingest_db(index_dir);
        auto t0 = std::chrono::high_resolution_clock::now();
        ingest_db.bulk_ingest(registry, {});
        auto t1 = std::chrono::high_resolution_clock::now();
        if (rebuild_summaries) {
            ingest_db.rebuild_root_summaries();
        }
        auto t2 = std::chrono::high_resolution_clock::now();
        DFTRACER_UTILS_LOG_INFO(
            "dftracer_index: bulk_ingest=%.2fms "
            "rebuild_root_summaries=%.2fms%s",
            std::chrono::duration<double, std::milli>(t1 - t0).count(),
            std::chrono::duration<double, std::milli>(t2 - t1).count(),
            rebuild_summaries ? "" : " (skipped)");
    }

    std::error_code ec;
    fs::remove_all(staging_root, ec);

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    std::printf("\n");
    std::printf("==========================================\n");
    std::printf("Bloom Index Results\n");
    std::printf("==========================================\n");
    std::printf("  Execution time: %.2f seconds\n", duration.count() / 1000.0);
    std::printf("  Files processed: %zu\n", batch_result.indexed);
    std::printf("  Files skipped: %zu\n", batch_result.skipped);
    std::printf("  Files failed: %zu\n", batch_result.failed);
    std::printf("  Events processed: %zu\n",
                static_cast<std::size_t>(batch_result.total_events));
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

    argparse::ArgumentParser program("dftracer_index",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "Build per-chunk bloom filter indices for DFTracer trace files. "
        "Creates root-local .dftindex databases enabling fast chunk-skipping "
        "queries.");

    IndexArgParse cli(program);
    cli.setup();
    if (!cli.parse(argc, argv)) return 1;

    return run_index(&cli).get();
}
