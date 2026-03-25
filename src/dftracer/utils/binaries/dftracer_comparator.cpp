#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregators.h>
#include <dftracer/utils/utilities/composites/dft/comparator/comparison_config.h>
#include <dftracer/utils/utilities/composites/dft/comparator/comparison_result.h>
#include <dftracer/utils/utilities/composites/dft/comparator/comparison_utility.h>
#include <dftracer/utils/utilities/composites/dft/comparator/tree_table_formatter.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>
#include <unistd.h>

#include <argparse/argparse.hpp>
#include <atomic>
#include <chrono>
#include <ctime>
#include <sstream>
#include <thread>
#include <unordered_set>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites::dft::aggregators;
using namespace dftracer::utils::utilities::composites::dft::comparator;

namespace {

void flatten_nodes(const ComparisonNode& node,
                   std::vector<const ComparisonNode*>& out) {
    out.push_back(&node);
    for (const auto& child : node.children) {
        flatten_nodes(child, out);
    }
}

// Run one complete aggregation pipeline for a set of files.
// Returns EventAggregatorUtilityOutput after the pipeline completes.
static coro::CoroTask<EventAggregatorUtilityOutput> run_aggregation(
    const std::vector<std::string>& input_files,
    const AggregationConfig& agg_config,
    const std::optional<common::query::Query>& query,
    const std::string& index_dir, std::size_t checkpoint_size,
    bool force_rebuild, std::size_t executor_threads) {
    constexpr std::size_t CHUNK_SIZE_MB = 4;
    constexpr std::size_t BATCH_SIZE_MB = 4;

    auto pipeline_config = PipelineConfig()
                               .with_name("DFTracer Comparator Aggregation")
                               .with_compute_threads(executor_threads)
                               .with_watchdog(false);
    Pipeline pipeline(pipeline_config);

    EventAggregatorUtility merger;
    std::atomic<int> global_chunk_idx{0};

    auto streaming_task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            auto chunk_chan = coro::make_channel<ChunkAggregatorInput>(0);
            auto result_chan = coro::make_channel<ChunkAggregationOutput>(8);

            co_await ctx.scope([&](CoroScope& scope) -> coro::CoroTask<void> {
                for (const auto& file_path : input_files) {
                    auto* global_chunk_idx_ptr = &global_chunk_idx;
                    scope.spawn([file_path, ch = chunk_chan->producer(),
                                 index_dir, checkpoint_size, force_rebuild,
                                 agg_config, query, global_chunk_idx_ptr](
                                    CoroScope& /*fctx*/) mutable
                                    -> coro::CoroTask<void> {
                        [[maybe_unused]] auto producer_guard = ch.guard();

                        std::string idx_path =
                            composites::dft::internal::determine_index_path(
                                file_path, index_dir);

                        auto meta_input =
                            composites::dft::MetadataCollectorUtilityInput::
                                from_file(file_path)
                                    .with_checkpoint_size(checkpoint_size)
                                    .with_force_rebuild(force_rebuild)
                                    .with_index(idx_path);
                        auto metadata =
                            co_await composites::dft::MetadataCollectorUtility{}
                                .process(meta_input);

                        if (!metadata.success) {
                            DFTRACER_UTILS_LOG_WARN("Skipping file: %s",
                                                    file_path.c_str());
                            co_return;
                        }

                        FileChunkMapperUtility file_mapper;
                        auto mapper_input =
                            FileChunkMapperInput::from_metadata(metadata)
                                .with_config(agg_config)
                                .with_checkpoint_size(checkpoint_size)
                                .with_target_chunk_size(CHUNK_SIZE_MB)
                                .with_batch_size(BATCH_SIZE_MB * 1024 * 1024);
                        mapper_input.query = query;
                        auto file_chunks =
                            co_await file_mapper.process(mapper_input);

                        int start_idx = global_chunk_idx_ptr->fetch_add(
                            static_cast<int>(file_chunks.size()));
                        for (int i = 0;
                             i < static_cast<int>(file_chunks.size()); ++i) {
                            file_chunks[i].chunk_index = start_idx + i;
                        }

                        for (auto& chunk : file_chunks) {
                            if (!co_await ch.send(std::move(chunk))) {
                                co_return;
                            }
                        }
                        co_return;
                    });
                }

                for (std::size_t w = 0; w < executor_threads; ++w) {
                    (void)w;
                    scope.spawn(
                        [chunk_chan, rp = result_chan->producer(), result_chan](
                            CoroScope& wctx) mutable -> coro::CoroTask<void> {
                            [[maybe_unused]] auto producer_guard = rp.guard();
                            while (auto input =
                                       co_await wctx.receive(chunk_chan)) {
                                ChunkAggregatorUtility agg;
                                auto output = co_await agg.process(*input);
                                if (!co_await result_chan->send(
                                        std::move(output))) {
                                    co_return;
                                }
                            }
                            co_return;
                        });
                }

                auto* merger_ptr = &merger;
                scope.spawn([result_chan, merger_ptr](
                                CoroScope& mctx) -> coro::CoroTask<void> {
                    while (auto output = co_await mctx.receive(result_chan)) {
                        merger_ptr->merge_chunk(std::move(*output));
                    }
                    co_return;
                });

                co_return;
            });

            co_return;
        },
        "StreamingAggregate");

    EventAggregatorUtilityOutput result;
    auto post_task = make_task(
        [&](CoroScope& /*ctx*/) -> coro::CoroTask<bool> {
            result = merger.finalize();
            co_return result.success;
        },
        "Finalize");

    post_task->depends_on(streaming_task);
    pipeline.set_source(streaming_task);
    pipeline.set_destination(post_task);
    pipeline.execute();

    co_return result;
}

}  // namespace

static coro::CoroTask<int> run_comparator(argparse::ArgumentParser& program) {
    std::string config_path = program.get<std::string>("--config");
    std::string baseline_path = program.get<std::string>("--baseline");
    std::string variant_path = program.get<std::string>("--variant");
    std::string query_str = program.get<std::string>("--query");
    std::string group_by_str = program.get<std::string>("--group-by");
    std::string format = program.get<std::string>("--format");
    bool no_color = program.get<bool>("--no-color");
    std::size_t executor_threads =
        program.get<std::size_t>("--executor-threads");
    std::string index_dir = program.get<std::string>("--index-dir");
    bool force_rebuild = program.get<bool>("--force");
    std::size_t checkpoint_size = program.get<std::size_t>("--checkpoint-size");
    double threshold = program.get<double>("--threshold");
    double time_interval_ms = program.get<double>("--time-interval");

    ComparisonConfig config;
    if (!config_path.empty()) {
        std::string error;
        auto parsed = ComparisonConfig::from_json_file(config_path, error);
        if (!parsed) {
            DFTRACER_UTILS_LOG_ERROR("Config error: %s", error.c_str());
            co_return 1;
        }
        config = std::move(*parsed);
    } else if (!baseline_path.empty() && !variant_path.empty()) {
        config = ComparisonConfig::from_cli(baseline_path, variant_path,
                                            query_str, group_by_str);
    } else {
        DFTRACER_UTILS_LOG_ERROR(
            "Must specify --config or both --baseline and --variant");
        co_return 1;
    }

    // CLI overrides
    if (!format.empty()) config.format = format;
    config.no_color = no_color;
    if (executor_threads > 0) config.executor_threads = executor_threads;
    if (checkpoint_size > 0) config.checkpoint_size = checkpoint_size;
    if (!index_dir.empty()) config.index_dir = index_dir;
    if (force_rebuild) config.force_rebuild = force_rebuild;
    if (threshold > 0.0) config.defaults.threshold_pct = threshold;
    if (time_interval_ms > 0.0)
        config.defaults.time_interval_ms = time_interval_ms;

    config.resolve();

    if (config.executor_threads == 0) {
        config.executor_threads = dftracer_utils_hardware_concurrency();
    }
    if (config.checkpoint_size == 0) {
        config.checkpoint_size =
            indexer::internal::Indexer::DEFAULT_CHECKPOINT_SIZE;
    }

    std::string temp_index_dir;
    if (config.index_dir.empty()) {
        try {
            auto temp_path = fs::temp_directory_path();
            temp_path /= "dftracer_cmp_" + std::to_string(std::time(nullptr)) +
                         "_" + std::to_string(getpid());
            temp_index_dir = temp_path.string();
            fs::create_directories(temp_index_dir);
            config.index_dir = temp_index_dir;
        } catch (const fs::filesystem_error& e) {
            temp_index_dir = "/tmp/dftracer_cmp_" +
                             std::to_string(std::time(nullptr)) + "_" +
                             std::to_string(getpid());
            fs::create_directories(temp_index_dir);
            config.index_dir = temp_index_dir;
            DFTRACER_UTILS_LOG_WARN(
                "Failed to get system temp directory, using /tmp: %s",
                e.what());
        }
    }

    // Enumerate files for both sides
    auto enumerate_files = [](const std::string& path)
        -> coro::CoroTask<std::vector<std::string>> {
        std::vector<std::string> files;
        if (fs::is_regular_file(path)) {
            files.push_back(path);
            co_return files;
        }
        filesystem::PatternDirectoryScannerUtility scanner;
        filesystem::PatternDirectoryScannerUtilityInput scan_input{
            path, {".pfw", ".pfw.gz"}, false};
        auto entries = co_await scanner.process(scan_input);
        files.reserve(entries.size());
        for (const auto& e : entries) {
            files.push_back(e.path.string());
        }
        co_return files;
    };

    auto baseline_files = co_await enumerate_files(config.baseline);
    auto variant_files = co_await enumerate_files(config.variant);

    if (baseline_files.empty()) {
        DFTRACER_UTILS_LOG_ERROR("No trace files found in baseline: %s",
                                 config.baseline.c_str());
        co_return 1;
    }
    if (variant_files.empty()) {
        DFTRACER_UTILS_LOG_ERROR("No trace files found in variant: %s",
                                 config.variant.c_str());
        co_return 1;
    }

    // Build indexes upfront so parallel aggregation doesn't race on .idx
    {
        std::unordered_set<std::string> seen;
        std::vector<std::string> all_files;
        for (const auto& f : baseline_files) {
            if (seen.insert(f).second) all_files.push_back(f);
        }
        for (const auto& f : variant_files) {
            if (seen.insert(f).second) all_files.push_back(f);
        }
        DFTRACER_UTILS_LOG_INFO("Building indexes for %zu unique files...",
                                all_files.size());
        std::vector<indexer::IndexBuildConfig> idx_configs;
        idx_configs.reserve(all_files.size());
        for (const auto& file_path : all_files) {
            idx_configs.push_back(
                indexer::IndexBuildConfig::for_file(file_path)
                    .with_checkpoint_size(config.checkpoint_size)
                    .with_force_rebuild(config.force_rebuild)
                    .with_index_dir(config.index_dir));
        }
        std::vector<coro::CoroTask<indexer::IndexBuildResult>> idx_tasks;
        idx_tasks.reserve(idx_configs.size());
        for (const auto& cfg : idx_configs) {
            idx_tasks.push_back(indexer::IndexBuilderUtility{}.process(cfg));
        }
        co_await coro::when_all(std::move(idx_tasks));
    }

    ComparisonOutput output;
    output.baseline_path = config.baseline;
    output.variant_path = config.variant;
    output.baseline_file_count = baseline_files.size();
    output.variant_file_count = variant_files.size();

    auto start_time = std::chrono::high_resolution_clock::now();

    for (auto& node : config.nodes) {
        std::vector<const ComparisonNode*> visitors;
        flatten_nodes(node, visitors);

        std::vector<ComparisonVisitorPair> pairs;
        pairs.reserve(visitors.size());

        for (const auto* visitor : visitors) {
            using common::query::Query;
            std::optional<Query> query;
            if (!visitor->composed_query.empty()) {
                auto result = Query::from_string(visitor->composed_query);
                if (!result) {
                    DFTRACER_UTILS_LOG_ERROR("Invalid query for node '%s': %s",
                                             visitor->name.c_str(),
                                             result.error().format().c_str());
                    co_return 1;
                }
                query = std::move(*result);
            }

            AggregationConfig agg_cfg;
            agg_cfg.time_interval_us = static_cast<std::uint64_t>(
                config.defaults.time_interval_ms * 1000.0);
            agg_cfg.extra_group_keys = {};
            agg_cfg.compute_statistics = true;
            agg_cfg.compute_percentiles = true;
            agg_cfg.percentiles = visitor->resolved_percentiles;
            agg_cfg.sketch_accuracy = 0.01;
            agg_cfg.track_process_parents = false;

            auto [base_result, var_result] = co_await coro::when_all(
                run_aggregation(baseline_files, agg_cfg, query,
                                config.index_dir, config.checkpoint_size,
                                config.force_rebuild, config.executor_threads),
                run_aggregation(variant_files, agg_cfg, query, config.index_dir,
                                config.checkpoint_size, config.force_rebuild,
                                config.executor_threads));

            // Extract metadata from first visitor (broadest query)
            if (pairs.empty()) {
                output.baseline_meta = extract_metadata(
                    base_result.aggregations, baseline_files.size());
                output.variant_meta = extract_metadata(var_result.aggregations,
                                                       variant_files.size());
            }

            ComparisonVisitorPair pair;
            pair.baseline = std::move(base_result);
            pair.variant = std::move(var_result);
            pair.node = *visitor;
            pairs.push_back(std::move(pair));
        }

        ComparisonUtilityInput cmp_input;
        cmp_input.visitors = std::move(pairs);
        cmp_input.root_node = node;
        cmp_input.baseline_file_count = baseline_files.size();
        cmp_input.variant_file_count = variant_files.size();

        ComparisonUtility cmp;
        auto cmp_output = co_await cmp.process(cmp_input);
        output.nodes.push_back(std::move(cmp_output.result));
    }

    // Inject metadata rows into root SUMMARY.
    auto meta_rows =
        build_metadata_metrics(output.baseline_meta, output.variant_meta);
    for (auto& node : output.nodes) {
        node.summary.metrics.insert(node.summary.metrics.begin(),
                                    meta_rows.begin(), meta_rows.end());
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;
    output.execution_time_ms = duration.count();

    if (config.format == "json") {
        TreeTableFormatter formatter;
        std::printf("%s\n", formatter.render_json(output).c_str());
    } else {
        bool is_tty = isatty(fileno(stdout));
        FormatterOptions fmt_opts;
        fmt_opts.use_color = is_tty && !config.no_color;
        fmt_opts.use_unicode = is_tty;
        TreeTableFormatter formatter(fmt_opts);
        formatter.render(stdout, output);
    }

    if (!temp_index_dir.empty() && fs::exists(temp_index_dir)) {
        fs::remove_all(temp_index_dir);
    }

    co_return 0;
}

int main(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    argparse::ArgumentParser program("dftracer_comparator",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "Compare DFTracer trace metrics between baseline and variant");

    program.add_argument("--config")
        .help("JSON config file for hierarchical comparison")
        .default_value<std::string>("");

    program.add_argument("--baseline")
        .help("Baseline trace file or directory")
        .default_value<std::string>("");

    program.add_argument("--variant")
        .help("Variant trace file or directory")
        .default_value<std::string>("");

    program.add_argument("--query")
        .help("Query filter (default: all events)")
        .default_value<std::string>("");

    program.add_argument("--group-by")
        .help("Comma-separated group keys (default: cat,name)")
        .default_value<std::string>("");

    program.add_argument("--format")
        .help("Output format: table (default) or json")
        .default_value<std::string>("table");

    program.add_argument("-t", "--time-interval")
        .help("Time interval in milliseconds for bucketing (default: 5000)")
        .scan<'g', double>()
        .default_value(5000.0);

    program.add_argument("--threshold")
        .help("Hide changes below this percentage")
        .scan<'g', double>()
        .default_value(0.0);

    program.add_argument("--no-color").help("Disable ANSI color output").flag();

    program.add_argument("--executor-threads")
        .help("Number of parallel threads (default: auto)")
        .scan<'d', std::size_t>()
        .default_value(
            static_cast<std::size_t>(dftracer_utils_hardware_concurrency()));

    program.add_argument("--index-dir")
        .help("Directory for index files (default: temp)")
        .default_value<std::string>("");

    program.add_argument("--force").help("Force index rebuild").flag();

    program.add_argument("--checkpoint-size")
        .help("Checkpoint size for indexing in bytes")
        .scan<'d', std::size_t>()
        .default_value(static_cast<std::size_t>(
            indexer::internal::Indexer::DEFAULT_CHECKPOINT_SIZE));

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        DFTRACER_UTILS_LOG_ERROR("Error: %s", err.what());
        std::fprintf(stderr, "%s\n", program.help().str().c_str());
        return 1;
    }

    return run_comparator(program).get();
}
