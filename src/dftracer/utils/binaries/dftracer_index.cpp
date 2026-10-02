#include <concurrentqueue.h>
#include <dftracer/utils/binaries/common_cli.h>
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/build/chunk_indexer.h>
#include <dftracer/utils/index/build/resolve_and_build.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_sst_writer_context.h>
#include <dftracer/utils/plugins/plugins.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::index::build;
using namespace dftracer::utils::index::build;
using namespace dftracer::utils::index::store;

class IndexArgParse : public cli::ArgParse {
   public:
    cli::DirectoryArgs directory;
    cli::PipelineArgs pipeline;
    cli::IndexingArgs indexing;

    std::string dimensions;
    std::size_t path_budget = 0;
    double stats_share = 0.05;
    std::uint64_t memory_budget = 0;
    std::string record_schema;
    std::size_t read_batch_size = 4;
    std::size_t expected_entries = 1024;
    double false_positive_rate = 0.01;
    std::vector<std::string> plugins;

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
                "Comma-separated args fields to index by name, nested ones "
                "included (e.g., level,mode,io.size)")
            .default_value<std::string>("");

        parser()
            .add_argument("--path-budget")
            .help(
                "Also index each file's N most frequent other args paths: "
                "numbers get a per-chunk min/max, strings a per-chunk bloom up "
                "to 256 distinct values; 0 = no count limit (default: 0)")
            .scan<'d', std::size_t>()
            .default_value(static_cast<std::size_t>(0));

        parser()
            .add_argument("--stats-share")
            .help(
                "Share of each file's compressed size its automatic evidence "
                "may use, in (0, 1], at least 8 MiB (default: 0.05)")
            .scan<'g', double>()
            .default_value(0.05);

        parser()
            .add_argument("--read-batch-size")
            .help(
                "Batch read size for stream processing (default: 4MB). A bare "
                "number is MB; suffixed values (e.g. 512KB) are absolute")
            .default_value(std::string("4"));

        parser()
            .add_argument("--schema")
            .help(
                "Record schema of every trace: a registered id such as "
                "dftracer or generic (default: detected per file from its "
                "first lines)")
            .default_value(std::string(""));

        parser()
            .add_argument("--memory-budget")
            .help(
                "Bytes the build may hold at once; past it large files are "
                "indexed fewer at a time and spill to disk (0 = auto: ~1/3 of "
                "available memory). Accepts units, e.g. 512MB, 4GB")
            .default_value(std::string("0"));

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

        parser()
            .add_argument("--plugin")
            .help(
                "Plugin shared library to load for the build; its index "
                "extensions are built for every file (repeatable)")
            .append()
            .default_value(std::vector<std::string>{});
    }

    void post_parse() override {
        dimensions = parser().get<std::string>("--dimensions");
        path_budget = parser().get<std::size_t>("--path-budget");
        stats_share = parser().get<double>("--stats-share");
        memory_budget = cli::get_bytes_arg(parser(), "--memory-budget");
        record_schema = parser().get<std::string>("--schema");
        read_batch_size =
            cli::get_bytes_arg(parser(), "--read-batch-size", 1024ull * 1024);
        expected_entries = parser().get<std::size_t>("--expected-entries");
        false_positive_rate = parser().get<double>("--false-positive-rate");
        plugins = parser().get<std::vector<std::string>>("--plugin");
    }

    bool validate() override {
        if (!(stats_share > 0 && stats_share <= 1)) {
            DFTRACER_UTILS_LOG_ERROR("--stats-share must be in (0, 1], got %g",
                                     stats_share);
            return false;
        }
        return true;
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

    std::vector<std::string> user_dimensions = cli::split_csv(dimensions_str);

    // Held for the whole build: loading registers the plugins' index
    // extensions, and destroying the set removes them.
    std::optional<plugins::Plugins> loaded;
    if (!cli->plugins.empty()) {
        auto builder = plugins::Plugins::builder();
        for (const auto& path : cli->plugins) builder.add(path);
        auto built = builder.build();
        if (!built) {
            DFTRACER_UTILS_LOG_ERROR("Failed to load plugins: %s",
                                     built.error().format().c_str());
            co_return 1;
        }
        loaded.emplace(std::move(*built));
    }

    std::vector<std::string> extra_dimensions;
    for (const auto& field : user_dimensions) {
        const std::string dim =
            dftracer::utils::index::build::extra_dimension_name(field);
        if (std::find(extra_dimensions.begin(), extra_dimensions.end(), dim) ==
            extra_dimensions.end()) {
            extra_dimensions.push_back(dim);
        }
    }

    ChunkIndexerConfig indexer_config;
    indexer_config.extra_dimensions = extra_dimensions;
    indexer_config.path_budget = cli->path_budget;
    indexer_config.stats_share = cli->stats_share;
    indexer_config.expected_entries_per_chunk = expected_entries;
    indexer_config.false_positive_rate = false_positive_rate;

    std::vector<std::string> all_dimensions(
        dftracer::utils::index::build::DEFAULT_BLOOM_DIMENSIONS.begin(),
        dftracer::utils::index::build::DEFAULT_BLOOM_DIMENSIONS.end());
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
    std::printf("==========================================\n\n");

    // Discover input files
    filesystem::PatternDirectoryScannerUtility scanner;
    filesystem::PatternDirectoryScannerUtilityInput scan_input{
        log_dir, filesystem::trace_file_patterns(), false};
    auto matched_entries = co_await scanner(scan_input);

    std::vector<std::string> input_files;
    input_files.reserve(matched_entries.size());
    for (const auto& entry : matched_entries) {
        input_files.push_back(entry.path.string());
    }

    if (input_files.empty()) {
        DFTRACER_UTILS_LOG_ERROR("No trace files found in: %s",
                                 log_dir.c_str());
        co_return 1;
    }

    DFTRACER_UTILS_LOG_INFO("Found %zu input files", input_files.size());

    // Normalize single-member inputs to bounded multi-member gzip so the build
    // never OOMs; readers use the same split/ copies.
    {
        auto norm = co_await normalize_members_for_ingest(
            std::move(input_files), checkpoint_size);
        input_files = std::move(norm.files);
        for (const auto& [src, dst] : norm.split)
            DFTRACER_UTILS_LOG_WARN(
                "auto-split single-member input (original kept): %s -> %s",
                src.c_str(), dst.c_str());
    }

    // On-disk sizes are compressed for .pfw.gz; estimate_per_file_bytes expands
    // them (PER_FILE_EXPANSION_FACTOR) to a decompressed + aggregated peak.
    std::vector<std::size_t> file_sizes;
    file_sizes.reserve(input_files.size());
    for (const auto& f : input_files) {
        std::error_code ec;
        const auto sz = fs::file_size(f, ec);
        file_sizes.push_back(ec ? 0 : sz);
    }
    const std::size_t required_bytes =
        estimate_per_file_bytes(file_sizes) * file_sizes.size();
    cli::warn_if_memory_tight(required_bytes);

    auto pipeline_config =
        cli::build_pipeline_config("DFTracer Bloom Indexer", cli->pipeline);

    Pipeline pipeline(pipeline_config);

    auto start_time = std::chrono::high_resolution_clock::now();

    std::vector<int> preassigned_file_ids;
    {
        IndexDatabase coord_db(index_dir);
        coord_db.init_schema();
        preassigned_file_ids = coord_db.assign_file_ids(input_files);
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
    batch_config->bloom_config = indexer_config;
    batch_config->bloom_dimensions = all_dimensions;
    batch_config->memory_budget = cli->memory_budget;
    batch_config->schema = cli->record_schema;

    batch_config->sink_factory =
        [staging_root, batch_counter]() -> std::unique_ptr<IndexWrite> {
        const std::size_t idx =
            batch_counter->fetch_add(1, std::memory_order_relaxed);
        return std::make_unique<IndexDatabaseSstWriterContext>(
            staging_root, "batch_" + std::to_string(idx));
    };
    // A spilled file's runs stay in order: its last run's range deletes must
    // ingest after the others.
    auto spilled = std::make_shared<std::pair<
        std::mutex,
        std::vector<std::vector<IndexDatabaseSstWriterContext::Artifacts>>>>();
    batch_config->spill_dir = staging_root + "/spill";
    batch_config->spill_commit =
        [spilled](std::vector<IndexDatabaseSstWriterContext::Artifacts> runs) {
            std::lock_guard<std::mutex> lock(spilled->first);
            spilled->second.push_back(std::move(runs));
        };
    batch_config->sink_commit = [artifacts_queue](IndexWrite& sink) {
        auto& sst = static_cast<IndexDatabaseSstWriterContext&>(sink);
        auto a = sst.commit();
        if (!a.empty()) artifacts_queue->enqueue(std::move(a));
    };

    IndexBuildBatchResult batch_result;
    auto streaming_task = make_task(
        [&batch_result,
         batch_config](CoroScope& scope) -> coro::CoroTask<void> {
            batch_result =
                co_await BatchBuilder::process(&scope, std::move(batch_config));
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
        for (auto& runs : spilled->second)
            for (auto& run : runs) registry.append(std::move(run));
    }
    {
        IndexDatabase ingest_db(index_dir);
        auto t0 = std::chrono::high_resolution_clock::now();
        ingest_db.bulk_ingest(registry, {});
        auto t1 = std::chrono::high_resolution_clock::now();
        DFTRACER_UTILS_LOG_INFO(
            "dftracer_index: bulk_ingest=%.2fms",
            std::chrono::duration<double, std::milli>(t1 - t0).count());
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
    std::printf("==========================================\n");

    co_return 0;
}

int main(int argc, char** argv) {
    return cli::cli_main<IndexArgParse>(
        argc, argv, "dftracer_index",
        "Build per-chunk bloom filter indices for DFTracer trace files. "
        "Creates root-local .dftindex databases enabling fast chunk-skipping "
        "queries.",
        [](IndexArgParse& cli) { return run_index(&cli).get(); });
}
