#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregators.h>
#include <dftracer/utils/utilities/composites/dft/comparator/comparison_config.h>
#include <dftracer/utils/utilities/composites/dft/comparator/comparison_result.h>
#include <dftracer/utils/utilities/composites/dft/comparator/comparison_utility.h>
#include <dftracer/utils/utilities/composites/dft/comparator/tree_table_formatter.h>
#include <dftracer/utils/utilities/composites/dft/indexing/index_resolver_utility.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <ctime>

#include "common_cli.h"

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites::dft::aggregators;
using namespace dftracer::utils::utilities::composites::dft::comparator;
using dftracer::utils::utilities::composites::dft::indexing::
    IndexResolverUtility;
using dftracer::utils::utilities::composites::dft::indexing::ResolverInput;
using dftracer::utils::utilities::indexer::IndexBatchBuilderUtility;
using dftracer::utils::utilities::indexer::IndexBuildBatchConfig;

class ComparatorArgParse : public cli::ArgParse {
   public:
    cli::PipelineArgs pipeline;
    cli::IndexingArgs indexing;
    cli::QueryArgs query_args{"Query filter (default: all events)"};

    std::string config_path;
    std::string baseline;
    std::string variant;
    std::string baseline_index_dir;
    std::string variant_index_dir;
    std::string group_by;
    std::string format = "table";
    double time_interval = 5000.0;
    double threshold = 0.0;
    bool no_color = false;

    explicit ComparatorArgParse(argparse::ArgumentParser& p) : ArgParse(p) {
        indexing.with_index_dir = false;
        indexing.force_help = "Force index rebuild";
        schema(pipeline, indexing, query_args);
    }

   protected:
    void register_args() override {
        parser()
            .add_argument("--config")
            .help("JSON config file for hierarchical comparison")
            .default_value<std::string>("");

        parser()
            .add_argument("--baseline")
            .help("Baseline trace file or directory")
            .default_value<std::string>("");

        parser()
            .add_argument("--variant")
            .help("Variant trace file or directory")
            .default_value<std::string>("");

        parser()
            .add_argument("--baseline-index-dir")
            .help(
                "Index directory for baseline (default: co-located with data)")
            .default_value<std::string>("");

        parser()
            .add_argument("--variant-index-dir")
            .help("Index directory for variant (default: co-located with data)")
            .default_value<std::string>("");

        parser()
            .add_argument("--group-by")
            .help("Comma-separated group keys (default: cat,name)")
            .default_value<std::string>("");

        parser()
            .add_argument("--format")
            .help("Output format: table (default) or json")
            .default_value<std::string>("table");

        parser()
            .add_argument("-t", "--time-interval")
            .help("Time interval in milliseconds for bucketing (default: 5000)")
            .scan<'g', double>()
            .default_value(5000.0);

        parser()
            .add_argument("--threshold")
            .help("Hide changes below this percentage")
            .scan<'g', double>()
            .default_value(0.0);

        parser()
            .add_argument("--no-color")
            .help("Disable ANSI color output")
            .flag();
    }

    void post_parse() override {
        config_path = parser().get<std::string>("--config");
        baseline = parser().get<std::string>("--baseline");
        variant = parser().get<std::string>("--variant");
        baseline_index_dir = parser().get<std::string>("--baseline-index-dir");
        variant_index_dir = parser().get<std::string>("--variant-index-dir");
        group_by = parser().get<std::string>("--group-by");
        format = parser().get<std::string>("--format");
        time_interval = parser().get<double>("--time-interval");
        threshold = parser().get<double>("--threshold");
        no_color = parser().get<bool>("--no-color");
    }
};

namespace {

void flatten_nodes(const ComparisonNode& node,
                   std::vector<const ComparisonNode*>& out) {
    out.push_back(&node);
    for (const auto& child : node.children) {
        flatten_nodes(child, out);
    }
}

static coro::CoroTask<void> process_file_task(
    std::string file_path, coro::ChannelProducer<ChunkAggregatorInput> ch,
    std::string index_dir, std::size_t checkpoint_size, bool force_rebuild,
    AggregationConfig agg_config, std::optional<common::query::Query> query,
    std::atomic<int>* global_chunk_idx_ptr) {
    constexpr std::size_t CHUNK_SIZE_MB = 4;
    constexpr std::size_t BATCH_SIZE_MB = 4;

    [[maybe_unused]] auto producer_guard = ch.guard();

    std::string index_path =
        composites::dft::internal::determine_index_path(file_path, index_dir);

    auto meta_input =
        composites::dft::MetadataCollectorUtilityInput::from_file(file_path)
            .with_checkpoint_size(checkpoint_size)
            .with_force_rebuild(force_rebuild)
            .with_index(index_path);
    auto metadata =
        co_await composites::dft::MetadataCollectorUtility{}.process(
            meta_input);

    if (!metadata.success) {
        DFTRACER_UTILS_LOG_WARN("Skipping file: %s", file_path.c_str());
        co_return;
    }

    FileChunkMapperUtility file_mapper;
    auto mapper_input = FileChunkMapperInput::from_metadata(metadata)
                            .with_config(agg_config)
                            .with_checkpoint_size(checkpoint_size)
                            .with_target_chunk_size(CHUNK_SIZE_MB)
                            .with_batch_size(BATCH_SIZE_MB * 1024 * 1024);
    mapper_input.query = query;
    auto file_chunks = co_await file_mapper.process(mapper_input);

    int start_idx =
        global_chunk_idx_ptr->fetch_add(static_cast<int>(file_chunks.size()));
    for (int i = 0; i < static_cast<int>(file_chunks.size()); ++i) {
        file_chunks[i].chunk_index = start_idx + i;
    }

    for (auto& chunk : file_chunks) {
        if (!co_await ch.send(std::move(chunk))) {
            co_return;
        }
    }
    co_return;
}

static coro::CoroTask<void> chunk_worker_task(
    std::shared_ptr<coro::Channel<ChunkAggregatorInput>> chunk_chan,
    coro::ChannelProducer<ChunkAggregationOutput> rp,
    std::shared_ptr<coro::Channel<ChunkAggregationOutput>> result_chan,
    CoroScope* wctx_ptr) {
    [[maybe_unused]] auto producer_guard = rp.guard();
    while (auto input = co_await wctx_ptr->receive(chunk_chan)) {
        ChunkAggregatorUtility agg;
        auto output = co_await agg.process(*input);
        if (!co_await result_chan->send(std::move(output))) {
            co_return;
        }
    }
    co_return;
}

static coro::CoroTask<EventAggregatorOutput> run_aggregation(
    CoroScope& ctx, const std::vector<std::string>& input_files,
    const AggregationConfig& agg_config,
    const std::optional<common::query::Query>& query,
    const std::string& index_dir, std::size_t checkpoint_size,
    bool force_rebuild, std::size_t executor_threads) {
    EventAggregator merger;
    std::atomic<int> global_chunk_idx{0};

    co_await ctx.scope([&](CoroScope& scope) -> coro::CoroTask<void> {
        auto chunk_chan = coro::make_channel<ChunkAggregatorInput>(0);
        auto result_chan = coro::make_channel<ChunkAggregationOutput>(2);

        for (const auto& file_path : input_files) {
            auto* global_chunk_idx_ptr = &global_chunk_idx;
            scope.spawn([file_path, ch = chunk_chan->producer(), index_dir,
                         checkpoint_size, force_rebuild, agg_config, query,
                         global_chunk_idx_ptr](CoroScope& /*fctx*/) mutable
                            -> coro::CoroTask<void> {
                co_await process_file_task(
                    std::move(file_path), std::move(ch), std::move(index_dir),
                    checkpoint_size, force_rebuild, std::move(agg_config),
                    std::move(query), global_chunk_idx_ptr);
            });
        }

        for (std::size_t w = 0; w < executor_threads; ++w) {
            (void)w;
            scope.spawn([chunk_chan, rp = result_chan->producer(), result_chan](
                            CoroScope& wctx) mutable -> coro::CoroTask<void> {
                co_await chunk_worker_task(chunk_chan, std::move(rp),
                                           result_chan, &wctx);
            });
        }

        auto* merger_ptr = &merger;
        scope.spawn(
            [result_chan, merger_ptr](CoroScope& mctx) -> coro::CoroTask<void> {
                while (auto output = co_await mctx.receive(result_chan)) {
                    merger_ptr->merge_chunk(std::move(*output));
                }
                co_return;
            });

        co_return;
    });

    co_return merger.finalize();
}

struct AggSpec {
    AggregationConfig agg_cfg;
    std::optional<common::query::Query> query;
    const ComparisonNode* visitor;
};

struct NodeAggPlan {
    ComparisonNode root;
    std::vector<AggSpec> specs;
};

static coro::CoroTask<void> run_all_aggregations(
    CoroScope& ctx, const std::vector<std::string>& files,
    const std::vector<NodeAggPlan>& plans,
    std::vector<std::vector<EventAggregatorOutput>>& results,
    const std::string& index_dir, const ComparisonConfig& config) {
    results.resize(plans.size());
    for (std::size_t ni = 0; ni < plans.size(); ++ni) {
        const auto& plan = plans[ni];
        results[ni].resize(plan.specs.size());
        for (std::size_t vi = 0; vi < plan.specs.size(); ++vi) {
            const auto& spec = plan.specs[vi];
            results[ni][vi] = co_await run_aggregation(
                ctx, files, spec.agg_cfg, spec.query, index_dir,
                config.checkpoint_size, config.force_rebuild,
                config.executor_threads);
        }
    }
}

}  // namespace

static int run_comparator(const ComparatorArgParse* cli) {
    const auto& config_path = cli->config_path;
    const auto& baseline_path = cli->baseline;
    const auto& variant_path = cli->variant;
    const auto& query_str = cli->query_args.query;
    const auto& group_by_str = cli->group_by;
    auto format = cli->format;
    auto no_color = cli->no_color;
    auto force_rebuild = cli->indexing.force;
    auto checkpoint_size = cli->indexing.checkpoint_size;
    auto threshold = cli->threshold;
    auto time_interval_ms = cli->time_interval;

    ComparisonConfig config;
    if (!config_path.empty()) {
        std::string error;
        auto parsed = ComparisonConfig::from_json_file(config_path, error);
        if (!parsed) {
            DFTRACER_UTILS_LOG_ERROR("Config error: %s", error.c_str());
            return 1;
        }
        config = std::move(*parsed);
    } else if (!baseline_path.empty() && !variant_path.empty()) {
        config = ComparisonConfig::from_cli(baseline_path, variant_path,
                                            query_str, group_by_str);
    } else {
        DFTRACER_UTILS_LOG_ERROR(
            "Must specify --config or both --baseline and --variant");
        return 1;
    }

    if (!format.empty()) config.format = format;
    config.no_color = no_color;
    if (cli->pipeline.executor_threads > 0)
        config.executor_threads = cli->pipeline.executor_threads;
    if (checkpoint_size > 0) config.checkpoint_size = checkpoint_size;
    if (!cli->baseline_index_dir.empty())
        config.baseline_index_dir = cli->baseline_index_dir;
    if (!cli->variant_index_dir.empty())
        config.variant_index_dir = cli->variant_index_dir;
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

    // Precompute aggregation plans from config (needed by both Agg tasks)
    std::vector<NodeAggPlan> agg_plans;
    for (auto& node : config.nodes) {
        NodeAggPlan plan;
        plan.root = node;

        std::vector<const ComparisonNode*> visitors;
        flatten_nodes(node, visitors);

        for (const auto* visitor : visitors) {
            AggSpec spec;
            if (!visitor->composed_query.empty()) {
                auto result =
                    common::query::Query::from_string(visitor->composed_query);
                if (!result) {
                    DFTRACER_UTILS_LOG_ERROR("Invalid query for node '%s': %s",
                                             visitor->name.c_str(),
                                             result.error().format().c_str());
                    return 1;
                }
                spec.query = std::move(*result);
            }

            spec.agg_cfg.time_interval_us = static_cast<std::uint64_t>(
                config.defaults.time_interval_ms * 1000.0);
            spec.agg_cfg.extra_group_keys = {};
            spec.agg_cfg.compute_statistics = true;
            spec.agg_cfg.compute_percentiles = true;
            spec.agg_cfg.percentiles = visitor->resolved_percentiles;
            spec.agg_cfg.sketch_accuracy = 0.01;
            spec.agg_cfg.track_process_parents = false;
            spec.visitor = visitor;

            plan.specs.push_back(std::move(spec));
        }
        agg_plans.push_back(std::move(plan));
    }

    auto pipeline_config =
        cli::build_pipeline_config("DFTracer Comparator", cli->pipeline);
    Pipeline pipeline(pipeline_config);

    auto resolve_and_build =
        [&config](CoroScope& scope, const std::string& path,
                  const std::string& index_dir,
                  std::vector<std::string>& out_files) -> coro::CoroTask<void> {
        if (!fs::exists(path)) {
            DFTRACER_UTILS_LOG_ERROR("Path does not exist: %s", path.c_str());
            co_return;
        }

        IndexResolverUtility resolver;
        ResolverInput resolve_input;
        resolve_input.index_dir = index_dir;
        resolve_input.require_checkpoints = !config.force_rebuild;
        if (fs::is_regular_file(path)) {
            resolve_input.files = {path};
        } else {
            resolve_input.directory = path;
        }

        auto result = co_await resolver.process(resolve_input);
        out_files = std::move(result.all_files);

        if (out_files.empty()) {
            DFTRACER_UTILS_LOG_ERROR("No trace files found in: %s",
                                     path.c_str());
            co_return;
        }

        if (result.needs_checkpoint.empty()) {
            DFTRACER_UTILS_LOG_INFO("All %zu files already indexed",
                                    out_files.size());
            co_return;
        }

        auto batch_cfg = std::make_shared<IndexBuildBatchConfig>();
        batch_cfg->file_paths.reserve(result.needs_checkpoint.size());
        for (const auto& item : result.needs_checkpoint) {
            batch_cfg->file_paths.push_back(item.file_path);
        }
        batch_cfg->index_dir = index_dir;
        batch_cfg->checkpoint_size = config.checkpoint_size;
        batch_cfg->parallelism = config.executor_threads;
        batch_cfg->force_rebuild = config.force_rebuild;
        batch_cfg->use_batch_write = true;
        batch_cfg->rebuild_root_summaries = true;

        DFTRACER_UTILS_LOG_INFO("Indexing %zu of %zu files...",
                                result.needs_checkpoint.size(),
                                out_files.size());
        co_await IndexBatchBuilderUtility::process(&scope,
                                                   std::move(batch_cfg));
    };

    // Shared state between tasks
    std::vector<std::string> baseline_files;
    std::vector<std::string> variant_files;
    std::vector<std::vector<EventAggregatorOutput>> baseline_results;
    std::vector<std::vector<EventAggregatorOutput>> variant_results;

    auto baseline_index_path = composites::dft::internal::determine_index_path(
        config.baseline, config.baseline_index_dir);
    auto variant_index_path = composites::dft::internal::determine_index_path(
        config.variant, config.variant_index_dir);
    bool shared_index = baseline_index_path == variant_index_path;

    std::shared_ptr<Task> enum_index_base;
    std::shared_ptr<Task> enum_index_var;

    if (shared_index) {
        auto enum_index_shared = make_task(
            [&config, &baseline_files, &variant_files,
             &resolve_and_build](CoroScope& scope) -> coro::CoroTask<void> {
                co_await resolve_and_build(scope, config.baseline,
                                           config.baseline_index_dir,
                                           baseline_files);
                if (config.baseline == config.variant) {
                    variant_files = baseline_files;
                } else {
                    co_await resolve_and_build(scope, config.variant,
                                               config.variant_index_dir,
                                               variant_files);
                }
            },
            "EnumIndex");
        enum_index_base = enum_index_shared;
        enum_index_var = enum_index_shared;
    } else {
        enum_index_base = make_task(
            [&config, &baseline_files,
             &resolve_and_build](CoroScope& scope) -> coro::CoroTask<void> {
                co_await resolve_and_build(scope, config.baseline,
                                           config.baseline_index_dir,
                                           baseline_files);
            },
            "EnumIndexBaseline");

        enum_index_var = make_task(
            [&config, &variant_files,
             &resolve_and_build](CoroScope& scope) -> coro::CoroTask<void> {
                co_await resolve_and_build(scope, config.variant,
                                           config.variant_index_dir,
                                           variant_files);
            },
            "EnumIndexVariant");
    }

    std::shared_ptr<Task> agg_base;
    std::shared_ptr<Task> agg_var;

    bool same_files = shared_index && config.baseline == config.variant;
    if (same_files) {
        auto agg_shared = make_task(
            [&baseline_files, &baseline_results, &variant_results, &agg_plans,
             &config](CoroScope& ctx) -> coro::CoroTask<void> {
                if (baseline_files.empty()) co_return;
                co_await run_all_aggregations(
                    ctx, baseline_files, agg_plans, baseline_results,
                    config.baseline_index_dir, config);
                variant_results = baseline_results;
            },
            "Aggregate");
        agg_shared->depends_on(enum_index_base);
        agg_base = agg_shared;
        agg_var = agg_shared;
    } else {
        agg_base = make_task(
            [&baseline_files, &baseline_results, &agg_plans,
             &config](CoroScope& ctx) -> coro::CoroTask<void> {
                if (baseline_files.empty()) co_return;
                co_await run_all_aggregations(
                    ctx, baseline_files, agg_plans, baseline_results,
                    config.baseline_index_dir, config);
            },
            "AggBaseline");
        agg_base->depends_on(enum_index_base);

        agg_var = make_task(
            [&variant_files, &variant_results, &agg_plans,
             &config](CoroScope& ctx) -> coro::CoroTask<void> {
                if (variant_files.empty()) co_return;
                co_await run_all_aggregations(ctx, variant_files, agg_plans,
                                              variant_results,
                                              config.variant_index_dir, config);
            },
            "AggVariant");
        agg_var->depends_on(enum_index_var);
    }

    // Compare (depends on both Agg tasks)
    ComparisonOutput output;
    output.baseline_path = config.baseline;
    output.variant_path = config.variant;
    int result_code = 0;

    auto compare_task = make_task(
        [&config, &baseline_files, &variant_files, &baseline_results,
         &variant_results, &agg_plans, &output, &result_code](
            [[maybe_unused]] CoroScope& ctx) -> coro::CoroTask<void> {
            if (baseline_files.empty() || variant_files.empty()) {
                result_code = 1;
                co_return;
            }

            output.baseline_file_count = baseline_files.size();
            output.variant_file_count = variant_files.size();

            auto start_time = std::chrono::high_resolution_clock::now();

            for (std::size_t ni = 0; ni < agg_plans.size(); ++ni) {
                const auto& plan = agg_plans[ni];
                std::vector<ComparisonVisitorPair> pairs;
                pairs.reserve(plan.specs.size());

                for (std::size_t vi = 0; vi < plan.specs.size(); ++vi) {
                    if (pairs.empty()) {
                        output.baseline_meta = extract_metadata(
                            baseline_results[ni][vi].aggregations,
                            baseline_files.size());
                        output.variant_meta = extract_metadata(
                            variant_results[ni][vi].aggregations,
                            variant_files.size());
                    }

                    ComparisonVisitorPair pair;
                    pair.baseline = std::move(baseline_results[ni][vi]);
                    pair.variant = std::move(variant_results[ni][vi]);
                    pair.node = *plan.specs[vi].visitor;
                    pairs.push_back(std::move(pair));
                }

                ComparisonUtilityInput cmp_input;
                cmp_input.visitors = std::move(pairs);
                cmp_input.root_node = plan.root;
                cmp_input.baseline_file_count = baseline_files.size();
                cmp_input.variant_file_count = variant_files.size();

                ComparisonUtility cmp;
                auto cmp_output = co_await cmp.process(cmp_input);
                output.nodes.push_back(std::move(cmp_output.result));
            }

            auto meta_rows = build_metadata_metrics(output.baseline_meta,
                                                    output.variant_meta);
            for (auto& n : output.nodes) {
                n.summary.metrics.insert(n.summary.metrics.begin(),
                                         meta_rows.begin(), meta_rows.end());
            }

            auto end_time = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double, std::milli> duration =
                end_time - start_time;
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

            co_return;
        },
        "Compare");

    if (same_files) {
        compare_task->depends_on(agg_base);
    } else {
        compare_task->depends_on({agg_base, agg_var});
    }

    if (shared_index) {
        pipeline.set_source(enum_index_base);
    } else {
        pipeline.set_source({enum_index_base, enum_index_var});
    }
    pipeline.set_destination(compare_task);

    try {
        pipeline.execute();
    } catch (const PipelineError& e) {
        DFTRACER_UTILS_LOG_ERROR("Pipeline failed: %s", e.what());
        result_code = 1;
    }

    return result_code;
}

int main(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    argparse::ArgumentParser program("dftracer_comparator",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "Compare DFTracer trace metrics between baseline and variant");

    ComparatorArgParse cli(program);
    cli.setup();
    if (!cli.parse(argc, argv)) return 1;

    return run_comparator(&cli);
}
