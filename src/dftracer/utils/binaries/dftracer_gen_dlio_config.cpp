#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/utils/timer.h>
#include <dftracer/utils/utilities/common/statistics/distributions.h>
#include <dftracer/utils/utilities/common/statistics/mixture.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_runner.h>
#include <dftracer/utils/utilities/dlio/barrier_simulator.h>
#include <dftracer/utils/utilities/dlio/optimizer.h>
#include <dftracer/utils/utilities/dlio/trace_loader.h>
#include <dftracer/utils/utilities/dlio/yaml_emit.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <fstream>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "common_cli.h"

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
namespace agg = dftracer::utils::utilities::composites::dft::aggregators;
namespace dlio = dftracer::utils::utilities::dlio;
namespace stats = dftracer::utils::utilities::common::statistics;

namespace {

class GenDlioConfigArgParse : public cli::ArgParse {
   public:
    cli::DirectoryArgs directory{
        cli::DirMode::DEFAULT_DOT,
        "Input directory containing .pfw or .pfw.gz traces"};
    cli::PipelineArgs pipeline;
    cli::IndexingArgs indexing;

    std::string output;
    double max_bound_percentile = 95.0;
    int simulation_iterations = 5;
    double target_e2e_error = 0.05;
    double target_cdf_similarity = 0.90;
    int patience = 10;
    double epsilon = 1.0;
    double momentum = 0.9;
    double min_percentile = 50.0;
    int num_workers = 8;
    int prefetch_factor = 2;
    std::uint64_t seed = 42;
    std::uint64_t max_samples_per_entry = 100;
    double time_interval = 5000.0;

    explicit GenDlioConfigArgParse(argparse::ArgumentParser& p) : ArgParse(p) {
        indexing.index_dir_help =
            "Directory to store index files (default: system temp directory)";
        indexing.force_help = "Force index recreation";
        schema(directory, pipeline, indexing);
    }

   protected:
    void register_args() override {
        parser()
            .add_argument("-o", "--output")
            .required()
            .help("Output path for the DLIO YAML config.");
        parser()
            .add_argument("--max-bound-percentile")
            .help("Initial max_bound percentile (0-100, default: 95)")
            .scan<'g', double>()
            .default_value(95.0);
        parser()
            .add_argument("--simulation-iterations")
            .help(
                "Max simulator iterations for percentile refinement (default: "
                "5)")
            .scan<'d', int>()
            .default_value(5);
        parser()
            .add_argument("--target-e2e-error")
            .help(
                "Target relative E2E error to declare convergence (default: "
                "0.05)")
            .scan<'g', double>()
            .default_value(0.05);
        parser()
            .add_argument("--target-cdf-similarity")
            .help("Target fetch_block CDF similarity (default: 0.90)")
            .scan<'g', double>()
            .default_value(0.90);
        parser()
            .add_argument("--patience")
            .help("Early-stop after this many iterations without improvement")
            .scan<'d', int>()
            .default_value(10);
        parser()
            .add_argument("--epsilon")
            .help("Base step size for percentile adjustment (default: 1.0)")
            .scan<'g', double>()
            .default_value(1.0);
        parser()
            .add_argument("--momentum")
            .help("Momentum factor in [0, 1) (default: 0.9)")
            .scan<'g', double>()
            .default_value(0.9);
        parser()
            .add_argument("--min-percentile")
            .help("Floor on max_bound percentile (default: 50)")
            .scan<'g', double>()
            .default_value(50.0);
        parser()
            .add_argument("--num-workers")
            .help("DataLoader worker count for the simulator (default: 8)")
            .scan<'d', int>()
            .default_value(8);
        parser()
            .add_argument("--prefetch-factor")
            .help("DataLoader prefetch factor (default: 2)")
            .scan<'d', int>()
            .default_value(2);
        parser()
            .add_argument("--seed")
            .help("Base seed for simulator + sampler (default: 42)")
            .scan<'i', std::uint64_t>()
            .default_value<std::uint64_t>(42);
        parser()
            .add_argument("--max-samples-per-entry")
            .help(
                "Cap on synthesized samples per AGGREGATION entry (default: "
                "100)")
            .scan<'i', std::uint64_t>()
            .default_value<std::uint64_t>(100);
        parser()
            .add_argument("-t", "--time-interval")
            .help("Aggregation time interval in ms (default: 5000)")
            .scan<'g', double>()
            .default_value(5000.0);
    }

    void post_parse() override {
        output = parser().get<std::string>("--output");
        max_bound_percentile = parser().get<double>("--max-bound-percentile");
        simulation_iterations = parser().get<int>("--simulation-iterations");
        target_e2e_error = parser().get<double>("--target-e2e-error");
        target_cdf_similarity = parser().get<double>("--target-cdf-similarity");
        patience = parser().get<int>("--patience");
        epsilon = parser().get<double>("--epsilon");
        momentum = parser().get<double>("--momentum");
        min_percentile = parser().get<double>("--min-percentile");
        num_workers = parser().get<int>("--num-workers");
        prefetch_factor = parser().get<int>("--prefetch-factor");
        seed = parser().get<std::uint64_t>("--seed");
        max_samples_per_entry =
            parser().get<std::uint64_t>("--max-samples-per-entry");
        time_interval = parser().get<double>("--time-interval");
    }
};

std::optional<stats::BestModel> fit_best_model(
    const std::vector<double>& data) {
    if (data.empty()) return std::nullopt;
    const auto singles = stats::fit_all_single_distributions(data);
    std::vector<stats::FittedMixture> mixtures;
    if (data.size() >= 20) {
        mixtures.push_back(stats::fit_gaussian_mixture(data, 2));
        mixtures.push_back(stats::fit_gaussian_mixture(data, 3));
    }
    const auto best = stats::select_best_model(singles, mixtures);
    if (!best) return std::nullopt;
    return best->model;
}

const char* model_label(const stats::BestModel& m) {
    return std::visit(
        [](const auto& v) -> const char* {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, stats::FittedDistribution>) {
                switch (v.kind) {
                    case stats::DistributionKind::Normal:
                        return "Normal";
                    case stats::DistributionKind::Lognormal:
                        return "Lognormal";
                    case stats::DistributionKind::Gamma:
                        return "Gamma";
                    case stats::DistributionKind::Exponential:
                        return "Exponential";
                    case stats::DistributionKind::Weibull:
                        return "Weibull";
                }
                return "Unknown";
            } else {
                return v.weights.size() == 2 ? "GMM-2" : "GMM-3";
            }
        },
        m);
}

}  // namespace

int main(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    argparse::ArgumentParser program("dftracer_gen_dlio_config",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "Generate a DLIO YAML configuration from raw DFTracer logs. Indexes "
        "and aggregates the input directory automatically; users do not need "
        "to run dftracer_aggregator separately.");

    GenDlioConfigArgParse cli(program);
    cli.setup();
    if (!cli.parse(argc, argv)) return 1;

    // --- Aggregation phase: produce / reuse the AGGREGATION CF ---------------
    agg::AggregationConfig agg_config;
    agg_config.time_interval_us =
        static_cast<std::uint64_t>(cli.time_interval * 1000.0);
    agg_config.compute_statistics = true;
    // DDSketch is required for high-fidelity DLIO config generation. Force it
    // on so users don't have to remember the flag.
    agg_config.compute_percentiles = true;
    agg_config.sketch_accuracy = 0.01;
    agg_config.percentiles = {0.25, 0.5, 0.75, 0.90};
    agg_config.track_process_parents = true;
    agg_config.track_default_args = true;

    agg::AggregationRunInput run_input;
    run_input.log_dir = cli.directory.value;
    run_input.index_dir = cli.indexing.index_dir;
    run_input.agg_config = std::move(agg_config);
    run_input.pipeline_config =
        cli::build_pipeline_config("DLIO Config Generator", cli.pipeline);
    run_input.output_file = std::nullopt;  // populate AGGREGATION CF only
    run_input.force_rebuild = cli.indexing.force;
    run_input.checkpoint_size = cli.indexing.checkpoint_size;
    run_input.verbose = true;

    auto run_result = agg::run_aggregation(std::move(run_input)).get();
    if (!run_result.success || run_result.index_path.empty()) {
        DFTRACER_UTILS_LOG_ERROR(
            "Aggregation failed; cannot generate DLIO config");
        return 1;
    }

    // --- Load aggregated traces ---------------------------------------------
    dlio::TraceLoaderOptions loader_opts;
    loader_opts.max_samples_per_entry = cli.max_samples_per_entry;
    loader_opts.seed = cli.seed;
    dlio::AggregatedTraces traces;
    try {
        traces =
            dlio::load_aggregated_traces(run_result.index_path, loader_opts);
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Failed to load AGGREGATION CF: %s", e.what());
        return 1;
    }
    if (!traces.any_data) {
        DFTRACER_UTILS_LOG_ERROR(
            "No DLIO events (fetch.block / preprocess) found in %s",
            cli.directory.value.c_str());
        return 1;
    }
    std::printf("\n");
    std::printf("==========================================\n");
    std::printf("DLIO Config Generation\n");
    std::printf("==========================================\n");
    std::printf("  Loaded %d rank(s), %d step(s) from index at %s\n",
                traces.num_ranks, traces.num_steps,
                run_result.index_path.c_str());
    std::printf("  computation_times: %zu samples (min %.6fs, max %.6fs)\n",
                traces.computation_times.size(), traces.fetch_block_stats.min(),
                traces.fetch_block_stats.max());
    std::printf("  preprocess_times:  %zu samples (min %.6fs, max %.6fs)\n",
                traces.preprocess_times.size(), traces.preprocess_stats.min(),
                traces.preprocess_stats.max());

    // --- Fit distributions ---------------------------------------------------
    auto comp_model = fit_best_model(traces.computation_times);
    auto prep_model = fit_best_model(traces.preprocess_times);
    if (!comp_model) {
        DFTRACER_UTILS_LOG_ERROR(
            "Failed to fit a computation_time distribution");
        return 1;
    }
    std::printf("  Best computation_time model: %s\n",
                model_label(*comp_model));
    if (prep_model) {
        std::printf("  Best preprocess_time  model: %s\n",
                    model_label(*prep_model));
    } else {
        std::printf("  No preprocess events; skipping preprocess block\n");
    }

    // --- Optimize max_bound percentile via simulator -------------------------
    auto ctx = dlio::make_simulator_context(traces, cli.num_workers,
                                            cli.prefetch_factor);

    dlio::OptimizerOptions opts;
    opts.max_iterations = cli.simulation_iterations;
    opts.target_e2e_error = cli.target_e2e_error;
    opts.target_cdf_similarity = cli.target_cdf_similarity;
    opts.patience = cli.patience;
    opts.epsilon = cli.epsilon;
    opts.momentum = cli.momentum;
    opts.min_percentile = cli.min_percentile;
    opts.initial_percentile = cli.max_bound_percentile;
    opts.base_seed = cli.seed;

    auto opt = dlio::optimize_max_bound_percentile(
        ctx, *comp_model, traces.computation_times, opts);
    std::printf(
        "  Optimizer: iters=%d, best_percentile=%.2f%%, e2e_error=%.2f%%, "
        "fetch_block_cdf_similarity=%.4f%s\n",
        opt.iterations_used, opt.best_percentile, opt.best.e2e_error * 100.0,
        opt.best.fetch_block_cdf_similarity,
        opt.converged ? " (converged)" : "");

    auto comp_sorted = traces.computation_times;
    std::sort(comp_sorted.begin(), comp_sorted.end());
    const double comp_max_bound =
        dlio::percentile(comp_sorted, opt.best_percentile);

    double prep_max_bound = 0.0;
    if (prep_model) {
        auto prep_sorted = traces.preprocess_times;
        std::sort(prep_sorted.begin(), prep_sorted.end());
        prep_max_bound = dlio::percentile(prep_sorted, opt.best_percentile);
    }

    // --- Emit YAML -----------------------------------------------------------
    dlio::DlioTimingBlock comp_block{*comp_model, comp_max_bound};
    std::optional<dlio::DlioTimingBlock> prep_block_storage;
    const dlio::DlioTimingBlock* prep_block_ptr = nullptr;
    if (prep_model) {
        prep_block_storage = dlio::DlioTimingBlock{*prep_model, prep_max_bound};
        prep_block_ptr = &(*prep_block_storage);
    }

    std::ofstream out(cli.output);
    if (!out) {
        DFTRACER_UTILS_LOG_ERROR("Cannot open %s for writing",
                                 cli.output.c_str());
        return 1;
    }
    if (!dlio::write_dlio_yaml(out, &comp_block, prep_block_ptr)) {
        DFTRACER_UTILS_LOG_ERROR("Failed to write %s", cli.output.c_str());
        return 1;
    }
    std::printf("  Wrote DLIO config: %s\n", cli.output.c_str());
    std::printf("==========================================\n");
    return 0;
}
