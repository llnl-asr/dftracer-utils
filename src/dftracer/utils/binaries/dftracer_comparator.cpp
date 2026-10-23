#include <dftracer/utils/binaries/common_cli.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/index/build/resolve_and_build.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/runtime/pipeline/pipeline.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/runtime/tasks/task.h>
#include <dftracer/utils/trace/comparator/comparison_aggregation.h>
#include <dftracer/utils/trace/comparator/comparison_config.h>
#include <dftracer/utils/trace/comparator/comparison_result.h>
#include <dftracer/utils/trace/comparator/comparison_utility.h>
#include <dftracer/utils/trace/comparator/tree_table_formatter.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <unistd.h>

#include <chrono>
#include <ctime>
#include <optional>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::comparator;
using dftracer::utils::index::build::resolve_and_build_index;
using dftracer::utils::index::build::ResolveAndBuildInput;

class ComparatorArgParse : public cli::ArgParse {
   public:
    cli::PipelineArgs pipeline;
    cli::IndexingArgs indexing;
    cli::DuqlArgs duql_args{"duql filter (default: all events)"};

    std::string config_path;
    std::string preset;
    std::string baseline;
    std::string variant;
    std::string baseline_index_dir;
    std::string variant_index_dir;
    std::string group_by;
    std::string format = "table";
    double time_interval = 5000.0;
    double threshold = 0.0;
    bool no_color = false;
    bool compact = false;

    explicit ComparatorArgParse(argparse::ArgumentParser& p) : ArgParse(p) {
        indexing.with_index_dir = false;
        indexing.force_help = "Force index rebuild";
        schema(pipeline, indexing, duql_args);
    }

   protected:
    void register_args() override {
        parser()
            .add_argument("--config")
            .help("JSON config file for hierarchical comparison")
            .default_value<std::string>("");

        parser()
            .add_argument("--preset")
            .help("Built-in comparison preset (supported: dlio)")
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
            .help(
                "Time interval for bucketing (default: 5000ms). A bare number "
                "is milliseconds; suffixed values like 5s are converted")
            .default_value(std::string("5000"));

        parser()
            .add_argument("--threshold")
            .help("Hide changes below this percentage")
            .scan<'g', double>()
            .default_value(0.0);

        parser()
            .add_argument("--no-color")
            .help("Disable ANSI color output")
            .flag();

        parser()
            .add_argument("--compact")
            .help(
                "Collapse nodes with only negligible changes to a single line")
            .flag();
    }

    void post_parse() override {
        config_path = parser().get<std::string>("--config");
        preset = parser().get<std::string>("--preset");
        baseline = parser().get<std::string>("--baseline");
        variant = parser().get<std::string>("--variant");
        baseline_index_dir = parser().get<std::string>("--baseline-index-dir");
        variant_index_dir = parser().get<std::string>("--variant-index-dir");
        group_by = parser().get<std::string>("--group-by");
        format = parser().get<std::string>("--format");
        time_interval = cli::get_duration_arg(parser(), "--time-interval", 1e3);
        threshold = parser().get<double>("--threshold");
        no_color = parser().get<bool>("--no-color");
        compact = parser().get<bool>("--compact");
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

struct AggSpec {
    const ComparisonNode* visitor;
};

struct NodeAggPlan {
    ComparisonNode root;
    std::vector<AggSpec> specs;
};

static coro::CoroTask<void> aggregate_all(
    const std::vector<std::string>& files,
    const dstd::Vector<ComparisonFilter>& filters, const std::string& index_dir,
    const ComparisonConfig& config, dstd::StringIntern& strings,
    SideAggregate& out, bool& failed) {
    auto result = co_await aggregate_side(
        files, index_dir, filters,
        static_cast<std::uint64_t>(config.defaults.time_interval_ms * 1000.0),
        config.executor_threads, strings);
    if (!result) {
        DFTRACER_UTILS_LOG_ERROR("Aggregation failed: %s",
                                 result.error().format().c_str());
        failed = true;
        co_return;
    }
    out = std::move(*result);
}

}  // namespace

// Build the comparison config from CLI args (config file, preset, or
// baseline/variant), apply CLI overrides, and resolve defaults. Returns
// nullopt on any user-facing error (already logged).
static std::optional<ComparisonConfig> build_comparison_config(
    const ComparatorArgParse* cli) {
    const auto& config_path = cli->config_path;
    const auto& preset = cli->preset;
    const auto& baseline_path = cli->baseline;
    const auto& variant_path = cli->variant;
    const auto& duql_str = cli->duql_args.duql;
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
            return std::nullopt;
        }
        config = std::move(*parsed);
    } else if (!preset.empty()) {
        if (baseline_path.empty() || variant_path.empty()) {
            DFTRACER_UTILS_LOG_ERROR(
                "--preset requires both --baseline and --variant");
            return std::nullopt;
        }
        auto parsed =
            ComparisonConfig::from_preset(preset, baseline_path, variant_path);
        if (!parsed) {
            DFTRACER_UTILS_LOG_ERROR("Unknown preset: %s. Supported: dlio",
                                     preset.c_str());
            return std::nullopt;
        }
        config = std::move(*parsed);
        // AND the user query into every top-level node so --duql narrows
        // the preset without replacing its structure.
        if (!duql_str.empty()) {
            for (auto& n : config.nodes) {
                n.query = n.query.empty()
                              ? duql_str
                              : "(" + n.query + ") AND (" + duql_str + ")";
            }
        }
    } else if (!baseline_path.empty() && !variant_path.empty()) {
        config = ComparisonConfig::from_cli(baseline_path, variant_path,
                                            duql_str, group_by_str);
    } else {
        DFTRACER_UTILS_LOG_ERROR(
            "Must specify --config, --preset, or both --baseline and "
            "--variant");
        return std::nullopt;
    }

    if (!format.empty()) config.format = format;
    config.no_color = no_color;
    if (cli->compact) config.compact = true;
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
        config.executor_threads = hardware_concurrency();
    }

    if (config.checkpoint_size == 0) {
        config.checkpoint_size = dftracer::utils::index::gzip::
            CheckpointIndexer::DEFAULT_CHECKPOINT_SIZE;
    }
    return config;
}

// Precompute the per-node aggregation plans and the filters of their visitors,
// in plan order (needed by both the baseline and variant Agg tasks). Returns
// false on an invalid or unsupported node query (already logged).
static bool build_agg_plans(const ComparisonConfig& config,
                            std::vector<NodeAggPlan>& agg_plans,
                            dstd::Vector<ComparisonFilter>& filters) {
    for (auto& node : config.nodes) {
        NodeAggPlan plan;
        plan.root = node;

        std::vector<const ComparisonNode*> visitors;
        flatten_nodes(node, visitors);

        for (const auto* visitor : visitors) {
            ComparisonFilter filter;
            if (!visitor->composed_query.empty()) {
                auto query = duql::Query::from_string(visitor->composed_query);
                if (!query) {
                    DFTRACER_UTILS_LOG_ERROR("Invalid query for node '%s': %s",
                                             visitor->name.c_str(),
                                             query.error().format().c_str());
                    return false;
                }
                auto translated = filter_from_query(*query);
                if (!translated) {
                    DFTRACER_UTILS_LOG_ERROR(
                        "Node '%s': %s", visitor->name.c_str(),
                        translated.error().format().c_str());
                    return false;
                }
                filter = std::move(*translated);
            }
            filters.push_back(std::move(filter));
            plan.specs.push_back(AggSpec{visitor});
        }
        agg_plans.push_back(std::move(plan));
    }
    return true;
}

// Render the assembled comparison output as JSON or a formatted tree table.
static void render_comparison_output(const ComparisonOutput& output,
                                     const ComparisonConfig& config) {
    if (config.format == "json") {
        TreeTableFormatter formatter;
        std::printf("%s\n", formatter.render_json(output).c_str());
    } else {
        bool is_tty = isatty(fileno(stdout));
        FormatterOptions fmt_opts;
        fmt_opts.use_color = is_tty && !config.no_color;
        fmt_opts.use_unicode = is_tty;
        fmt_opts.compact = config.compact;
        TreeTableFormatter formatter(fmt_opts);
        formatter.render(stdout, output);
    }
}

static int run_comparator(const ComparatorArgParse* cli) {
    auto config_opt = build_comparison_config(cli);
    if (!config_opt) return 1;
    ComparisonConfig config = std::move(*config_opt);

    // Precompute aggregation plans and filters from config (needed by both
    // Agg tasks)
    std::vector<NodeAggPlan> agg_plans;
    dstd::Vector<ComparisonFilter> filters;
    if (!build_agg_plans(config, agg_plans, filters)) return 1;

    auto pipeline_config =
        cli::build_pipeline_config("DFTracer Comparator", cli->pipeline);
    Pipeline pipeline(pipeline_config);

    // Indexes the traces under `path` with their aggregation tier and lists
    // the trace files. The aggregation config is the one the queries use.
    auto resolve_and_build =
        [&config](CoroScope& scope, const std::string& path,
                  const std::string& index_dir,
                  std::vector<std::string>& out_files) -> coro::CoroTask<void> {
        if (!dftracer::utils::input_exists(path)) {
            DFTRACER_UTILS_LOG_ERROR("Path does not exist: %s", path.c_str());
            co_return;
        }

        ResolveAndBuildInput input;
        if (dftracer::utils::input_is_regular_file(path)) {
            input.files.push_back(path);
        } else {
            input.directory = path;
        }
        input.index_dir = index_dir;
        input.checkpoint_size = config.checkpoint_size;
        input.parallelism = config.executor_threads;
        input.force_rebuild = config.force_rebuild;
        input.build_bloom = false;
        input.require_aggregation = true;
        input.aggregation_config = comparison_index_config();

        auto result =
            co_await resolve_and_build_index(&scope, std::move(input));
        out_files = std::move(result.all_files);

        if (out_files.empty()) {
            DFTRACER_UTILS_LOG_ERROR("No trace files found in: %s",
                                     path.c_str());
            co_return;
        }
        for (const auto& failure : result.failures) {
            DFTRACER_UTILS_LOG_WARN("Could not index %s: %s",
                                    failure.file_path.c_str(),
                                    failure.message.c_str());
        }
    };

    // Shared state between tasks
    std::vector<std::string> baseline_files;
    std::vector<std::string> variant_files;
    // The ids of both sides' groups are in this one table.
    dstd::StringIntern strings;
    SideAggregate baseline_side;
    SideAggregate variant_side;
    bool baseline_failed = false;
    bool variant_failed = false;

    auto baseline_index_path = trace::internal::determine_index_path(
        config.baseline, config.baseline_index_dir);
    auto variant_index_path = trace::internal::determine_index_path(
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
            [&baseline_files, &baseline_side, &baseline_failed, &filters,
             &strings,
             &config]([[maybe_unused]] CoroScope& ctx) -> coro::CoroTask<void> {
                if (baseline_files.empty()) co_return;
                co_await aggregate_all(baseline_files, filters,
                                       config.baseline_index_dir, config,
                                       strings, baseline_side, baseline_failed);
            },
            "Aggregate");
        agg_shared->depends_on(enum_index_base);
        agg_base = agg_shared;
        agg_var = agg_shared;
    } else {
        agg_base = make_task(
            [&baseline_files, &baseline_side, &baseline_failed, &filters,
             &strings,
             &config]([[maybe_unused]] CoroScope& ctx) -> coro::CoroTask<void> {
                if (baseline_files.empty()) co_return;
                co_await aggregate_all(baseline_files, filters,
                                       config.baseline_index_dir, config,
                                       strings, baseline_side, baseline_failed);
            },
            "AggBaseline");
        agg_base->depends_on(enum_index_base);

        agg_var = make_task(
            [&variant_files, &variant_side, &variant_failed, &filters, &strings,
             &config]([[maybe_unused]] CoroScope& ctx) -> coro::CoroTask<void> {
                if (variant_files.empty()) co_return;
                co_await aggregate_all(variant_files, filters,
                                       config.variant_index_dir, config,
                                       strings, variant_side, variant_failed);
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
        [&config, &baseline_files, &variant_files, &baseline_side,
         &variant_side, &baseline_failed, &variant_failed, &strings, &agg_plans,
         &output, &result_code,
         same_files]([[maybe_unused]] CoroScope& ctx) -> coro::CoroTask<void> {
            if (baseline_files.empty() || variant_files.empty() ||
                baseline_failed || variant_failed) {
                result_code = 1;
                co_return;
            }

            const SideAggregate& variant_ref =
                same_files ? baseline_side : variant_side;
            output.baseline_file_count = baseline_files.size();
            output.variant_file_count = variant_files.size();
            // Metadata comes from the first node's first visitor so
            // process_count, thread_count, and makespan reflect the full
            // trace rather than whichever node happens to be last.
            output.baseline_meta = baseline_side.meta;
            output.variant_meta = variant_ref.meta;

            auto start_time = std::chrono::high_resolution_clock::now();

            std::size_t visitor_index = 0;
            for (std::size_t ni = 0; ni < agg_plans.size(); ++ni) {
                const auto& plan = agg_plans[ni];
                std::vector<ComparisonVisitorPair> pairs;
                pairs.reserve(plan.specs.size());

                for (std::size_t vi = 0; vi < plan.specs.size(); ++vi) {
                    ComparisonVisitorPair pair;
                    pair.baseline = &baseline_side.groups[visitor_index];
                    pair.variant = &variant_ref.groups[visitor_index];
                    pair.node = *plan.specs[vi].visitor;
                    pairs.push_back(std::move(pair));
                    ++visitor_index;
                }

                ComparisonUtilityInput cmp_input;
                cmp_input.visitors = std::move(pairs);
                cmp_input.root_node = plan.root;
                cmp_input.strings = &strings;
                cmp_input.baseline_file_count = baseline_files.size();
                cmp_input.variant_file_count = variant_files.size();

                ComparisonUtility cmp;
                auto cmp_output = co_await cmp(cmp_input);
                output.nodes.push_back(std::move(cmp_output->result));
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

            render_comparison_output(output, config);

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
    return cli::cli_main<ComparatorArgParse>(
        argc, argv, "dftracer_comparator",
        "Compare DFTracer trace metrics between baseline and variant",
        [](ComparatorArgParse& cli) { return run_comparator(&cli); });
}
