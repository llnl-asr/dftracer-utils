// SPDX-License-Identifier: MIT
#include <dftracer/utils/binaries/common_cli.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/when_all.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/index/build/resolve_and_build.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer.h>
#include <dftracer/utils/trace/comparator/comparison_aggregation.h>
#include <dftracer/utils/trace/comparator/comparison_config.h>
#include <dftracer/utils/trace/comparator/comparison_result.h>
#include <dftracer/utils/trace/comparator/comparison_utility.h>
#include <dftracer/utils/trace/comparator/tree_table_formatter.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <ctime>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::comparator;
using dftracer::utils::index::build::resolve_and_build_index;
using dftracer::utils::index::build::ResolveAndBuildInput;

class ComparatorArgParse : public binaries::ArgParse {
   public:
    binaries::RuntimeArgs runtime_args;
    binaries::IndexingArgs indexing;
    binaries::DuqlArgs duql_args{"duql filter (default: all events)"};

    dstd::StringView config_path;
    dstd::StringView preset;
    dstd::StringView baseline;
    dstd::StringView variant;
    dstd::StringView baseline_index_dir;
    dstd::StringView variant_index_dir;
    dstd::StringView group_by;
    dstd::StringView format = "table";
    double time_interval = 5000.0;
    double threshold = 0.0;
    bool no_color = false;
    bool compact = false;

    explicit ComparatorArgParse(binaries::ArgParser& p) : ArgParse(p) {
        indexing.with_index_dir = false;
        indexing.force_help = "Force index rebuild";
        schema(runtime_args, indexing, duql_args);
    }

   protected:
    void register_args() override {
        parser().option(config_path, {"--config"},
                        "JSON config file for hierarchical comparison");

        parser().option(preset, {"--preset"},
                        "Built-in comparison preset (supported: dlio)");

        parser().option(baseline, {"--baseline"},
                        "Baseline trace file or directory");

        parser().option(variant, {"--variant"},
                        "Variant trace file or directory");

        parser().option(
            baseline_index_dir, {"--baseline-index-dir"},
            "Index directory for baseline (default: co-located with data)");

        parser().option(
            variant_index_dir, {"--variant-index-dir"},
            "Index directory for variant (default: co-located with data)");

        parser().option(group_by, {"--group-by"},
                        "Comma-separated group keys (default: cat,name)");

        parser().option(format, {"--format"},
                        "Output format: table (default) or json");

        parser().duration(
            time_interval, 1e3, {"-t", "--time-interval"},
            "Time interval for bucketing (default: 5000ms). A bare number "
            "is milliseconds; suffixed values like 5s are converted",
            "5000");

        parser().option(threshold, {"--threshold"},
                        "Hide changes below this percentage");

        parser().flag(no_color, {"--no-color"}, "Disable ANSI color output");

        parser().flag(
            compact, {"--compact"},
            "Collapse nodes with only negligible changes to a single line");
    }
};

namespace {

void flatten_nodes(const ComparisonNode& node,
                   dstd::Vector<const ComparisonNode*>& out) {
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
    dstd::Vector<AggSpec> specs;
};

static coro::CoroTask<void> aggregate_all(
    const dstd::Vector<dstd::String>& files,
    const dstd::Vector<ComparisonFilter>& filters,
    const dstd::String& index_dir, const ComparisonConfig& config,
    std::size_t workers, dstd::StringIntern& strings, SideAggregate& out,
    bool& failed) {
    if (files.empty()) co_return;
    auto result = co_await aggregate_side(
        files, index_dir, filters,
        static_cast<std::uint64_t>(config.defaults.time_interval_ms * 1000.0),
        workers, strings);
    if (!result) {
        DFTRACER_UTILS_LOG_ERROR("Aggregation failed: {}",
                                 result.error().format());
        failed = true;
        co_return;
    }
    out = std::move(*result);
}

// Render the assembled comparison output as JSON or a formatted tree table.
void render_comparison_output(const ComparisonOutput& output,
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

// Indexes the traces under `path` with the aggregation config the queries use
// and lists the trace files.
coro::CoroTask<void> resolve_and_build(const ComparisonConfig& config,
                                       const dstd::String& path,
                                       const dstd::String& index_dir,
                                       dstd::Vector<dstd::String>& out_files,
                                       std::size_t parallelism) {
    if (!common::input_exists(path)) {
        DFTRACER_UTILS_LOG_ERROR("Path does not exist: {}", path);
        co_return;
    }

    ResolveAndBuildInput input;
    if (common::input_is_regular_file(path)) {
        input.files.emplace_back(dstd::StringView(path));
    } else {
        input.directory.assign(path.data(), path.size());
    }
    input.index_dir.assign(index_dir.data(), index_dir.size());
    input.checkpoint_size = config.checkpoint_size;
    input.parallelism = parallelism;
    input.force_rebuild = config.force_rebuild;
    input.build_bloom = false;

    auto result = co_await resolve_and_build_index(std::move(input));
    out_files.clear();
    for (const auto& f : result.all_files)
        out_files.emplace_back(f.data(), f.size());

    if (out_files.empty()) {
        DFTRACER_UTILS_LOG_ERROR("No trace files found in: {}", path);
        co_return;
    }
    for (const auto& failure : result.failures) {
        DFTRACER_UTILS_LOG_WARN("Could not index {}: {}", failure.file_path,
                                failure.message);
    }
}

// What the steps share. The ids of both sides' groups are in one string table.
struct ComparatorState {
    ComparatorState(const ComparisonConfig& comparison,
                    const dstd::Vector<ComparisonFilter>& node_filters,
                    const dstd::Vector<NodeAggPlan>& plans)
        : config(comparison), filters(node_filters), agg_plans(plans) {}

    const ComparisonConfig& config;
    const dstd::Vector<ComparisonFilter>& filters;
    const dstd::Vector<NodeAggPlan>& agg_plans;
    dstd::Vector<dstd::String> baseline_files;
    dstd::Vector<dstd::String> variant_files;
    dstd::StringIntern strings;
    SideAggregate baseline_side;
    SideAggregate variant_side;
    bool baseline_failed = false;
    bool variant_failed = false;
};

// One side on its own: build its index, then aggregate it. The two sides run
// this at once when their indexes are apart, so each takes half of the threads.
coro::CoroTask<void> index_and_aggregate_side(ComparatorState& state,
                                              bool is_baseline,
                                              std::size_t threads) {
    const auto& config = state.config;
    const auto& path = is_baseline ? config.baseline : config.variant;
    const auto& index_dir =
        is_baseline ? config.baseline_index_dir : config.variant_index_dir;
    auto& files = is_baseline ? state.baseline_files : state.variant_files;
    co_await resolve_and_build(config, path, index_dir, files, threads);
    co_await aggregate_all(
        files, state.filters, index_dir, config, threads, state.strings,
        is_baseline ? state.baseline_side : state.variant_side,
        is_baseline ? state.baseline_failed : state.variant_failed);
}

// Both sides share one index root: build it once, baseline first, then
// aggregate. The two aggregations run at once unless both sides are the same
// files.
coro::CoroTask<void> index_and_aggregate_shared(ComparatorState& state,
                                                bool same_files) {
    const auto& config = state.config;
    co_await resolve_and_build(config, config.baseline,
                               config.baseline_index_dir, state.baseline_files,
                               config.executor_threads);
    if (same_files) {
        state.variant_files = state.baseline_files;
        co_await aggregate_all(state.baseline_files, state.filters,
                               config.baseline_index_dir, config,
                               config.executor_threads, state.strings,
                               state.baseline_side, state.baseline_failed);
        co_return;
    }
    co_await resolve_and_build(config, config.variant, config.variant_index_dir,
                               state.variant_files, config.executor_threads);
    const std::size_t agg_threads =
        std::max<std::size_t>(1, config.executor_threads / 2);
    auto baseline_task = aggregate_all(
        state.baseline_files, state.filters, config.baseline_index_dir, config,
        agg_threads, state.strings, state.baseline_side, state.baseline_failed);
    auto variant_task = aggregate_all(
        state.variant_files, state.filters, config.variant_index_dir, config,
        agg_threads, state.strings, state.variant_side, state.variant_failed);
    co_await coro::when_all(std::move(baseline_task), std::move(variant_task));
}

// Compares the two aggregates and prints the result. Returns the exit code.
coro::CoroTask<int> compare_sides(ComparatorState& state, bool same_files) {
    const auto& config = state.config;
    if (state.baseline_files.empty() || state.variant_files.empty() ||
        state.baseline_failed || state.variant_failed) {
        co_return 1;
    }

    ComparisonOutput output;
    output.baseline_path = config.baseline;
    output.variant_path = config.variant;

    const SideAggregate& variant_ref =
        same_files ? state.baseline_side : state.variant_side;
    output.baseline_file_count = state.baseline_files.size();
    output.variant_file_count = state.variant_files.size();
    // Metadata comes from the first node's first visitor so the counts
    // and makespan reflect the full trace, not the last node.
    output.baseline_meta = state.baseline_side.meta;
    output.variant_meta = variant_ref.meta;

    auto start_time = std::chrono::high_resolution_clock::now();

    std::size_t visitor_index = 0;
    for (std::size_t ni = 0; ni < state.agg_plans.size(); ++ni) {
        const auto& plan = state.agg_plans[ni];
        dstd::Vector<ComparisonVisitorPair> pairs;
        pairs.reserve(plan.specs.size());

        for (std::size_t vi = 0; vi < plan.specs.size(); ++vi) {
            ComparisonVisitorPair pair;
            pair.baseline = &state.baseline_side.groups[visitor_index];
            pair.variant = &variant_ref.groups[visitor_index];
            pair.node = *plan.specs[vi].visitor;
            pairs.push_back(std::move(pair));
            ++visitor_index;
        }

        ComparisonUtilityInput cmp_input;
        cmp_input.visitors = std::move(pairs);
        cmp_input.root_node = plan.root;
        cmp_input.strings = &state.strings;
        cmp_input.baseline_file_count = state.baseline_files.size();
        cmp_input.variant_file_count = state.variant_files.size();

        ComparisonUtility cmp;
        auto cmp_output = co_await cmp(cmp_input);
        output.nodes.push_back(std::move(cmp_output->result));
    }

    auto meta_rows =
        build_metadata_metrics(output.baseline_meta, output.variant_meta);
    for (auto& n : output.nodes) {
        n.summary.metrics.insert(n.summary.metrics.begin(), meta_rows.begin(),
                                 meta_rows.end());
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;
    output.execution_time_ms = duration.count();

    render_comparison_output(output, config);
    co_return 0;
}

}  // namespace

// Build the comparison config from CLI args (config file, preset, or
// baseline/variant), apply CLI overrides, and resolve defaults. Returns
// nullopt on any user-facing error (already logged).
static dstd::Optional<ComparisonConfig> build_comparison_config(
    const ComparatorArgParse* cli) {
    const dstd::String config_path(cli->config_path);
    const dstd::String preset(cli->preset);
    const dstd::String baseline_path(cli->baseline);
    const dstd::String variant_path(cli->variant);
    const dstd::String duql_str(cli->duql_args.duql);
    const dstd::String group_by_str(cli->group_by);
    const dstd::String format(cli->format);
    auto no_color = cli->no_color;
    auto force_rebuild = cli->indexing.force;
    auto checkpoint_size = cli->indexing.checkpoint_size;
    auto threshold = cli->threshold;
    auto time_interval_ms = cli->time_interval;

    ComparisonConfig config;
    if (!config_path.empty()) {
        dstd::String error;
        auto parsed = ComparisonConfig::from_json_file(config_path, error);
        if (!parsed) {
            DFTRACER_UTILS_LOG_ERROR("Config error: {}", error);
            return dstd::nullopt;
        }
        config = std::move(*parsed);
    } else if (!preset.empty()) {
        if (baseline_path.empty() || variant_path.empty()) {
            DFTRACER_UTILS_LOG_ERROR(
                "--preset requires both --baseline and --variant");
            return dstd::nullopt;
        }
        auto parsed =
            ComparisonConfig::from_preset(preset, baseline_path, variant_path);
        if (!parsed) {
            DFTRACER_UTILS_LOG_ERROR("Unknown preset: {}. Supported: dlio",
                                     preset);
            return dstd::nullopt;
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
        return dstd::nullopt;
    }

    if (!format.empty()) config.format = format;
    config.no_color = no_color;
    if (cli->compact) config.compact = true;
    if (cli->runtime_args.executor_threads > 0)
        config.executor_threads = cli->runtime_args.executor_threads;
    if (checkpoint_size > 0) config.checkpoint_size = checkpoint_size;
    if (!cli->baseline_index_dir.empty())
        config.baseline_index_dir = dstd::String(cli->baseline_index_dir);
    if (!cli->variant_index_dir.empty())
        config.variant_index_dir = dstd::String(cli->variant_index_dir);
    if (force_rebuild) config.force_rebuild = force_rebuild;
    if (threshold > 0.0) config.defaults.threshold_pct = threshold;
    if (time_interval_ms > 0.0)
        config.defaults.time_interval_ms = time_interval_ms;

    config.resolve();

    if (config.executor_threads == 0) {
        config.executor_threads = common::hardware_concurrency();
    }

    if (config.checkpoint_size == 0) {
        config.checkpoint_size = dftracer::utils::index::gzip::
            CheckpointIndexer::DEFAULT_CHECKPOINT_SIZE;
    }
    return config;
}

// Returns false on an invalid or unsupported node query (already logged).
static bool build_agg_plans(const ComparisonConfig& config,
                            dstd::Vector<NodeAggPlan>& agg_plans,
                            dstd::Vector<ComparisonFilter>& filters) {
    for (auto& node : config.nodes) {
        NodeAggPlan plan;
        plan.root = node;

        dstd::Vector<const ComparisonNode*> visitors;
        flatten_nodes(node, visitors);

        for (const auto* visitor : visitors) {
            ComparisonFilter filter;
            if (!visitor->composed_query.empty()) {
                auto query = duql::Query::from_string(visitor->composed_query);
                if (!query) {
                    DFTRACER_UTILS_LOG_ERROR("Invalid query for node '{}': {}",
                                             visitor->name,
                                             query.error().format());
                    return false;
                }
                auto translated = filter_from_query(*query);
                if (!translated) {
                    DFTRACER_UTILS_LOG_ERROR("Node '{}': {}", visitor->name,
                                             translated.error().format());
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

static int run_comparator(const ComparatorArgParse* cli) {
    auto config_opt = build_comparison_config(cli);
    if (!config_opt) return 1;
    ComparisonConfig config = std::move(*config_opt);

    dstd::Vector<NodeAggPlan> agg_plans;
    dstd::Vector<ComparisonFilter> filters;
    if (!build_agg_plans(config, agg_plans, filters)) return 1;

    ComparatorState state(config, filters, agg_plans);

    auto baseline_index_path = trace::internal::determine_index_path(
        config.baseline, config.baseline_index_dir);
    auto variant_index_path = trace::internal::determine_index_path(
        config.variant, config.variant_index_dir);
    const bool shared_index = baseline_index_path == variant_index_path;
    const bool same_files = shared_index && config.baseline == config.variant;

    try {
        return binaries::run_on_pool(
            "DFTracer Comparator", cli->runtime_args,
            [&](coro::Scope&) -> coro::CoroTask<int> {
                if (shared_index) {
                    co_await index_and_aggregate_shared(state, same_files);
                } else {
                    // The two sides run at once on one executor, so each plans
                    // for half of its threads. Each holds buffers per planned
                    // worker.
                    const std::size_t side_threads =
                        std::max<std::size_t>(1, config.executor_threads / 2);
                    auto baseline_task = index_and_aggregate_side(
                        state, /*is_baseline=*/true, side_threads);
                    auto variant_task = index_and_aggregate_side(
                        state, /*is_baseline=*/false, side_threads);
                    co_await coro::when_all(std::move(baseline_task),
                                            std::move(variant_task));
                }
                co_return co_await compare_sides(state, same_files);
            });
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Run failed: {}", e.what());
        return 1;
    }
}

int main(int argc, char** argv) {
    return binaries::cli_main<ComparatorArgParse>(
        argc, argv, "dftracer_comparator",
        "Compare DFTracer trace metrics between baseline and variant",
        [](ComparatorArgParse& cli) { return run_comparator(&cli); });
}
