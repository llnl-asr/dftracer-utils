// SPDX-License-Identifier: MIT
#include <dftracer/utils/binaries/common_cli.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/aggregation_run.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/build/chunk_indexer.h>
#include <dftracer/utils/index/build/resolve_and_build.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/plugins/plugins.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>

#include <algorithm>
#include <chrono>
#include <numeric>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::index::build;
using namespace dftracer::utils::index::build;
using namespace dftracer::utils::index::store;

class IndexArgParse : public binaries::ArgParse {
   public:
    binaries::InputArgs inputs{
        binaries::DirMode::DefaultDot,
        "Folder of trace files to index (default: the current folder)"};
    binaries::RuntimeArgs runtime_args;
    binaries::IndexingArgs indexing;

    dstd::StringView dimensions;
    std::size_t path_budget = 0;
    double stats_share = 0.05;
    std::uint64_t memory_budget = 0;
    dstd::StringView record_schema;
    std::size_t read_batch_size = 4ull * 1024 * 1024;
    std::size_t expected_entries = 1024;
    double false_positive_rate = 0.01;
    dstd::Vector<dstd::StringView> plugins;

    explicit IndexArgParse(binaries::ArgParser& p) : ArgParse(p) {
        indexing.index_dir_help =
            "Directory where .dftindex stores are created";
        indexing.force_help = "Force index recreation even if already built";
        inputs.folder_only = true;
        schema(inputs, runtime_args, indexing);
    }

   protected:
    void register_args() override {
        parser().option(
            dimensions, {"--dimensions"},
            "Comma-separated args fields to index by name, nested ones "
            "included (e.g., level,mode,io.size)");

        parser().option(
            path_budget, {"--path-budget"},
            "Also index each file's N most frequent other args paths: "
            "numbers get a per-chunk min/max, strings a per-chunk bloom up "
            "to 256 distinct values; 0 = no count limit (default: 0)");

        parser().option(
            stats_share, {"--stats-share"},
            "Share of each file's compressed size its automatic evidence "
            "may use, in (0, 1], at least 8 MiB (default: 0.05)");

        parser().bytes_with_unit(
            read_batch_size, {"--read-batch-size"},
            "Batch read size for stream processing, with a unit (default: "
            "4MB, e.g. 512KB, 4MB)",
            "4MB");

        parser().option(
            record_schema, {"--schema"},
            "Record schema of every trace: a registered id such as "
            "dftracer or generic (default: detected per file from its "
            "first lines)");

        parser().bytes(
            memory_budget, {"--memory-budget"},
            "Bytes the build may hold at once; past it large files are "
            "indexed fewer at a time and spill to disk (0 = auto: ~1/3 of "
            "available memory). Accepts units, e.g. 512MB, 4GB",
            "0");

        parser().option(
            expected_entries, {"--expected-entries"},
            "Expected entries per chunk for bloom filter sizing (default: "
            "1024)");

        parser().option(false_positive_rate, {"--false-positive-rate"},
                        "Bloom filter false positive rate (default: 0.01)");

        parser()
            .option(plugins, {"--plugin"},
                    "Plugin shared library to load for the build; its index "
                    "extensions are built for every file (repeatable)")
            .append();
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

// Split out so the lambda that calls it holds no coroutine locals, which ICEs
// GCC 12/13.
static coro::CoroTask<int> select_stale_task(ResolverInput input,
                                             IndexWork* stale) {
    *stale = co_await files_needing_index(std::move(input));
    co_return 0;
}

static void print_summary(double seconds, std::size_t indexed,
                          std::size_t skipped, std::size_t failed,
                          std::size_t events,
                          const dstd::Vector<dstd::String>& dimensions) {
    std::printf("\n");
    std::printf("==========================================\n");
    std::printf("Bloom Index Results\n");
    std::printf("==========================================\n");
    std::printf("  Execution time: %.2f seconds\n", seconds);
    std::printf("  Files processed: %zu\n", indexed);
    std::printf("  Files skipped: %zu\n", skipped);
    std::printf("  Files failed: %zu\n", failed);
    std::printf("  Events processed: %zu\n", events);
    std::printf("  Dimensions indexed: %zu\n", dimensions.size());
    std::printf("  Dimensions: ");
    for (std::size_t i = 0; i < dimensions.size(); ++i) {
        std::printf("%s%s", dimensions[i].c_str(),
                    i < dimensions.size() - 1 ? ", " : "\n");
    }
    std::printf("==========================================\n");
}

static coro::CoroTask<int> run_index(const IndexArgParse* cli) {
    const auto log_dir = binaries::absolute_path(cli->inputs.directory);
    const auto force_rebuild = cli->indexing.force;
    const auto checkpoint_size = cli->indexing.checkpoint_size;
    const auto executor_threads = cli->runtime_args.executor_threads;
    // Without --index-dir the index goes next to the input data, where
    // determine_index_path(file, "") puts each file's index. The top-level
    // scanner is non-recursive, so all input files share log_dir.
    const dstd::String index_dir = cli->indexing.index_dir.empty()
                                       ? dstd::String(log_dir.str())
                                       : dstd::String(cli->indexing.index_dir);
    const auto expected_entries = cli->expected_entries;
    const auto false_positive_rate = cli->false_positive_rate;

    const auto user_dimensions = binaries::split_csv(cli->dimensions);

    // Held for the whole build: loading registers the plugins' index
    // extensions, and destroying the set removes them.
    dstd::Optional<plugins::Plugins> loaded;
    if (!cli->plugins.empty()) {
        auto builder = plugins::Plugins::builder();
        for (const auto& path : cli->plugins)
            builder.add(
                dftracer::utils::dstd::String(path.data(), path.size()));
        auto built = builder.build();
        if (!built) {
            DFTRACER_UTILS_LOG_ERROR("Failed to load plugins: %s",
                                     built.error().format().c_str());
            co_return 1;
        }
        loaded.emplace(std::move(*built));
    }

    dstd::Vector<dstd::String> extra_dimensions;
    for (const auto field : user_dimensions) {
        const dstd::String dim =
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

    dstd::Vector<dstd::String> all_dimensions(
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

    using StageClock = std::chrono::steady_clock;
    auto stage_start = StageClock::now();
    auto log_stage = [&stage_start](const char* name) {
        const auto now = StageClock::now();
        DFTRACER_UTILS_LOG_DEBUG(
            "dftracer_index: stage=%s ms=%.2f", name,
            std::chrono::duration<double, std::milli>(now - stage_start)
                .count());
        stage_start = now;
    };

    auto matched_entries =
        binaries::scan_trace_files_on_pool(cli->runtime_args, log_dir.str());

    const auto start_time = std::chrono::high_resolution_clock::now();
    if (matched_entries.empty()) {
        DFTRACER_UTILS_LOG_ERROR("No trace files found in: %s",
                                 log_dir.c_str());
        co_return 1;
    }
    DFTRACER_UTILS_LOG_INFO("Found %zu input files", matched_entries.size());
    log_stage("scan");

    std::size_t skipped_fresh = 0;
    dstd::Set<dstd::String> stale_tiers;
    if (!force_rebuild) {
        ResolverInput resolve_input;
        resolve_input.entries = matched_entries;
        resolve_input.index_dir = index_dir;
        resolve_input.require_checkpoints = true;
        resolve_input.require_bloom = true;
        resolve_input.checkpoint_size = checkpoint_size;
        resolve_input.bloom_config = indexer_config;
        resolve_input.schema = dstd::String(cli->record_schema);
        IndexWork stale;
        binaries::run_on_pool(
            "DFTracer Bloom Indexer", cli->runtime_args, [&](coro::Scope&) {
                return select_stale_task(std::move(resolve_input), &stale);
            });
        skipped_fresh = matched_entries.size() - stale.files.size();
        matched_entries = std::move(stale.files);
        stale_tiers.insert(stale.stale_aggregation_roots.begin(),
                           stale.stale_aggregation_roots.end());
        log_stage("resolve");
    }

    if (matched_entries.empty()) {
        print_summary(
            std::chrono::duration<double>(
                std::chrono::high_resolution_clock::now() - start_time)
                .count(),
            0, skipped_fresh, 0, 0, all_dimensions);
        co_return 0;
    }

    dstd::Vector<dstd::String> input_files;
    dstd::Vector<std::size_t> scanned_sizes;
    input_files.reserve(matched_entries.size());
    scanned_sizes.reserve(matched_entries.size());
    for (const auto& entry : matched_entries) {
        input_files.emplace_back(entry.path.str());
        scanned_sizes.push_back(entry.size);
    }

    // File ids follow logical path order, so a run of ids is a run of paths.
    {
        dstd::Vector<dstd::String> logical(input_files.size());
        for (std::size_t i = 0; i < input_files.size(); ++i)
            logical[i] =
                index::store::internal::get_logical_path(input_files[i]);
        dstd::Vector<std::size_t> order(input_files.size());
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::sort(order.begin(), order.end(),
                  [&](std::size_t a, std::size_t b) {
                      return logical[a] < logical[b];
                  });
        dstd::Vector<dstd::String> files(input_files.size());
        dstd::Vector<std::size_t> sizes(input_files.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            files[i] = std::move(input_files[order[i]]);
            sizes[i] = scanned_sizes[order[i]];
        }
        input_files = std::move(files);
        scanned_sizes = std::move(sizes);
    }
    log_stage("sort");

    // On-disk sizes are compressed for .pfw.gz; estimate_per_file_bytes expands
    // them (PER_FILE_EXPANSION_FACTOR) to a decompressed + aggregated peak.
    const std::size_t required_bytes =
        common::estimate_per_file_bytes(scanned_sizes) * scanned_sizes.size();
    binaries::warn_if_memory_tight(required_bytes);

    const dstd::String agg_index =
        trace::internal::determine_index_path(input_files.front(), index_dir);
    if (force_rebuild &&
        common::unwrap(dstd::fs::exists(dstd::PathView(agg_index))))
        stale_tiers.insert(agg_index);
    for (const auto& root : stale_tiers) clear_aggregation(root);

    auto batch_config = dstd::make_shared<IndexBuildBatchConfig>();
    batch_config->file_paths = std::move(input_files);
    batch_config->file_costs.assign(scanned_sizes.begin(), scanned_sizes.end());
    batch_config->index_dir = index_dir;
    batch_config->checkpoint_size = checkpoint_size;
    batch_config->parallelism = executor_threads;
    batch_config->force_rebuild = force_rebuild;
    batch_config->bloom_config = indexer_config;
    batch_config->bloom_dimensions = all_dimensions;
    batch_config->memory_budget = cli->memory_budget;
    batch_config->schema = dstd::String(cli->record_schema);

    AggRun agg_run = start_aggregation(
        agg_index, index::schemas::dft::agg::standard_aggregation_config());
    batch_config->agg_fold_factory = std::move(agg_run.factory);

    IndexBuildBatchResult batch_result;
    binaries::run_on_pool(
        "DFTracer Bloom Indexer", cli->runtime_args,
        [&batch_result, batch_config](coro::Scope&) -> coro::CoroTask<void> {
            batch_result =
                co_await BatchBuilder::process(std::move(batch_config));
        });
    log_stage("build");
    finish_aggregation(agg_run, batch_result);
    log_stage("aggregate");
    DFTRACER_UTILS_LOG_DEBUG(
        "dftracer_index: thread_ms parse=%.2f schema=%.2f sink_commit=%.2f "
        "files_in_flight_max=%zu files_spilled=%zu spill_runs_max=%zu",
        static_cast<double>(batch_result.metrics.parse_ns) / 1e6,
        static_cast<double>(batch_result.metrics.schema_ns) / 1e6,
        static_cast<double>(batch_result.metrics.write_ns) / 1e6,
        batch_result.metrics.max_files_in_flight,
        batch_result.metrics.files_spilled,
        batch_result.metrics.max_spill_runs);

    log_stage("close");
    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    print_summary(duration.count() / 1000.0, batch_result.indexed,
                  batch_result.skipped + skipped_fresh, batch_result.failed,
                  static_cast<std::size_t>(batch_result.total_events),
                  all_dimensions);

    for (const auto& r : batch_result.results) {
        if (!r.success && !r.was_skipped) {
            DFTRACER_UTILS_LOG_ERROR("Failed to index %s: %s",
                                     r.file_path.c_str(),
                                     r.error_message.c_str());
        }
    }
    co_return batch_result.failed > 0 ? 1 : 0;
}

int main(int argc, char** argv) {
    return binaries::cli_main<IndexArgParse>(
        argc, argv, "dftracer_index",
        "Build per-chunk bloom filter indices for DFTracer trace files. "
        "Creates root-local .dftindex databases enabling fast chunk-skipping "
        "queries.",
        [](IndexArgParse& cli) { return run_index(&cli).get(); });
}
