// SPDX-License-Identifier: MIT
#include <dftracer/utils/binaries/common_cli.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/timer.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/scoped_fd.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/variant.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/resolver.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/utilities/dlio/barrier_simulator.h>
#include <dftracer/utils/utilities/dlio/distributions.h>
#include <dftracer/utils/utilities/dlio/mixture.h>
#include <dftracer/utils/utilities/dlio/optimizer.h>
#include <dftracer/utils/utilities/dlio/trace_loader.h>
#include <dftracer/utils/utilities/dlio/yaml_emit.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <type_traits>
#include <variant>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;

namespace {

class GenDlioConfigArgParse : public binaries::ArgParse {
   public:
    binaries::InputArgs inputs{
        binaries::DirMode::DefaultDot,
        "Folder of .pfw.gz traces (default: the current folder)"};
    binaries::RuntimeArgs runtime_args;
    binaries::IndexingArgs indexing;

    dstd::StringView output;
    double max_bound_percentile = 95.0;
    int simulation_iterations = 5;
    double target_e2e_error = 0.05;
    double target_cdf_similarity = 0.90;
    int patience = 10;
    double epsilon = 1.0;
    double momentum = 0.9;
    double min_percentile = 50.0;
    int worker_count = 8;
    int prefetch_factor = 2;
    std::uint64_t seed = 42;
    std::uint64_t max_samples_per_entry = 100;
    double time_interval = 5000.0;

    // Optional YAML/JSON file remapping the (cat, name) of each DLIO component.
    dstd::StringView event_map;

    explicit GenDlioConfigArgParse(binaries::ArgParser& p) : ArgParse(p) {
        indexing.index_dir_help =
            "Directory to store index files (default: system temp directory)";
        indexing.force_help = "Force index recreation";
        inputs.folder_only = true;
        schema(inputs, runtime_args, indexing);
    }

   protected:
    void register_args() override {
        parser()
            .option(output, {"-o", "--output"},
                    "Output file for the DLIO YAML config.")
            .required();

        parser().option(max_bound_percentile, {"--max-bound-percentile"},
                        "Initial max_bound percentile (0-100, default: 95)");

        parser().option(
            simulation_iterations, {"--simulation-iterations"},
            "Max simulator iterations for percentile refinement (default: "
            "5)");

        parser().option(
            target_e2e_error, {"--target-e2e-error"},
            "Target relative E2E error to declare convergence (default: "
            "0.05)");

        parser().option(target_cdf_similarity, {"--target-cdf-similarity"},
                        "Target fetch_block CDF similarity (default: 0.90)");

        parser().option(
            patience, {"--patience"},
            "Early-stop after this many iterations without improvement");

        parser().option(
            epsilon, {"--epsilon"},
            "Base step size for percentile adjustment (default: 1.0)");

        parser().option(momentum, {"--momentum"},
                        "Momentum factor in [0, 1) (default: 0.9)");

        parser().option(min_percentile, {"--min-percentile"},
                        "Floor on max_bound percentile (default: 50)");

        parser().option(
            worker_count, {"--num-workers"},
            "DataLoader worker count for the simulator (default: 8)");

        parser().option(prefetch_factor, {"--prefetch-factor"},
                        "DataLoader prefetch factor (default: 2)");

        parser().option(seed, {"--seed"},
                        "Base seed for simulator + sampler (default: 42)");

        parser().option(
            max_samples_per_entry, {"--max-samples-per-entry"},
            "Cap on synthesized samples per AGGREGATION entry (default: "
            "100)");

        parser().duration(
            time_interval, 1e3, {"-t", "--time-interval"},
            "Aggregation time interval (default: 5000ms). A bare number is "
            "milliseconds; suffixed values like 5s are converted",
            "5000");

        parser().option(
            event_map, {"--event-map"},
            "YAML or JSON file remapping the (cat, name) of the "
            "fetch_block / fetch_iter / preprocess / item components");
    }
};

dstd::Optional<utilities::dlio::BestModel> fit_best_model(
    const dstd::Vector<double>& data) {
    if (data.empty()) return dstd::nullopt;
    const auto singles = utilities::dlio::fit_all_single_distributions(data);
    dstd::Vector<utilities::dlio::FittedMixture> mixtures;
    if (data.size() >= 20) {
        mixtures.push_back(utilities::dlio::fit_gaussian_mixture(data, 2));
        mixtures.push_back(utilities::dlio::fit_gaussian_mixture(data, 3));
    }
    const auto best = utilities::dlio::select_best_model(singles, mixtures);
    if (!best) return dstd::nullopt;
    return best->model;
}

const char* model_label(const utilities::dlio::BestModel& m) {
    return std::visit(
        [](const auto& v) -> const char* {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T,
                                         utilities::dlio::FittedDistribution>) {
                switch (v.kind) {
                    case utilities::dlio::DistributionKind::Normal:
                        return "Normal";
                    case utilities::dlio::DistributionKind::Lognormal:
                        return "Lognormal";
                    case utilities::dlio::DistributionKind::Gamma:
                        return "Gamma";
                    case utilities::dlio::DistributionKind::Exponential:
                        return "Exponential";
                    case utilities::dlio::DistributionKind::Weibull:
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
    return binaries::cli_main<GenDlioConfigArgParse>(
        argc, argv, "dftracer_gen_dlio_config",
        "Generate a DLIO YAML configuration from raw DFTracer logs. Indexes "
        "and aggregates the input directory automatically; no separate "
        "aggregation step is required.",
        [](GenDlioConfigArgParse& cli) -> int {
            index::schemas::dft::agg::AggregationConfig agg_config;
            agg_config.time_interval_us =
                static_cast<std::uint64_t>(cli.time_interval * 1000.0);
            agg_config.compute_statistics = true;
            // DDSketch is required for high-fidelity DLIO config generation.
            // Force it on so users don't have to remember the flag.
            agg_config.compute_percentiles = true;
            agg_config.sketch_accuracy = 0.01;
            agg_config.percentiles = {0.25, 0.5, 0.75, 0.90};

            const index::schemas::dft::agg::AggregationConfig query_config =
                agg_config;

            const auto scanned = binaries::ensure_indexes_fresh_blocking(
                "DLIO Config Generator", cli.runtime_args, cli.inputs.directory,
                {}, cli.indexing.index_dir, cli.indexing.force, agg_config,
                cli.indexing.checkpoint_size);
            if (scanned.empty()) {
                DFTRACER_UTILS_LOG_ERROR(
                    "No trace files found in {}; cannot generate DLIO config",
                    dstd::String(cli.inputs.directory));
                return 1;
            }
            const dstd::String index_path =
                trace::internal::determine_index_path(
                    scanned.front().path.c_str(), cli.indexing.index_dir);

            // --- Load aggregated traces
            // ---------------------------------------------
            utilities::dlio::TraceLoaderOptions loader_opts;
            loader_opts.max_samples_per_entry = cli.max_samples_per_entry;
            loader_opts.seed = cli.seed;
            // Remap component (cat, name) selectors from the event map, if any.
            if (!cli.event_map.empty()) {
                try {
                    utilities::dlio::load_event_map(dstd::String(cli.event_map),
                                                    loader_opts);
                } catch (const std::exception& e) {
                    DFTRACER_UTILS_LOG_ERROR("{}", e.what());
                    return 1;
                }
            }
            utilities::dlio::AggregatedTraces traces;
            try {
                index::build::ResolverInput resolver_input;
                resolver_input.directory = dstd::String(
                    binaries::absolute_path(cli.inputs.directory).str());
                resolver_input.index_dir = dstd::String(cli.indexing.index_dir);
                resolver_input.require_checkpoints = false;
                const auto resolved =
                    index::build::Resolver{}(resolver_input).get();
                if (!resolved) {
                    DFTRACER_UTILS_LOG_ERROR("{}", resolved.error().format());
                    return 1;
                }
                dstd::Vector<dstd::String> files;
                for (const auto& f : resolved->all_files) files.emplace_back(f);
                traces = utilities::dlio::load_aggregated_traces(
                    index_path, files, query_config, loader_opts);
            } catch (const std::exception& e) {
                DFTRACER_UTILS_LOG_ERROR("Failed to query the aggregation: {}",
                                         e.what());
                return 1;
            }
            if (!traces.any_data) {
                DFTRACER_UTILS_LOG_ERROR(
                    "No DLIO events ({}/{}, {}/{}) found in {}",
                    loader_opts.fetch_block.cat, loader_opts.fetch_block.name,
                    loader_opts.preprocess.cat, loader_opts.preprocess.name,
                    dstd::String(cli.inputs.directory));
                return 1;
            }
            std::printf("\n");
            std::printf("==========================================\n");
            std::printf("DLIO Config Generation\n");
            std::printf("==========================================\n");
            std::printf("  Loaded %d rank(s), %d step(s) from index at %s\n",
                        traces.rank_count, traces.step_count,
                        index_path.c_str());
            std::printf(
                "  computation_times: %zu samples (min %.6fs, max %.6fs)\n",
                traces.computation_times.size(), traces.fetch_block_stats.min(),
                traces.fetch_block_stats.max());
            std::printf(
                "  preprocess_times:  %zu samples (min %.6fs, max %.6fs)\n",
                traces.preprocess_times.size(), traces.preprocess_stats.min(),
                traces.preprocess_stats.max());

            // --- Fit distributions
            // ---------------------------------------------------
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
                std::printf(
                    "  No preprocess events; skipping preprocess block\n");
            }

            // --- Optimize max_bound percentile via simulator
            // -------------------------
            auto ctx = utilities::dlio::make_simulator_context(
                traces, cli.worker_count, cli.prefetch_factor);

            utilities::dlio::OptimizerOptions opts;
            opts.max_iterations = cli.simulation_iterations;
            opts.target_e2e_error = cli.target_e2e_error;
            opts.target_cdf_similarity = cli.target_cdf_similarity;
            opts.patience = cli.patience;
            opts.epsilon = cli.epsilon;
            opts.momentum = cli.momentum;
            opts.min_percentile = cli.min_percentile;
            opts.initial_percentile = cli.max_bound_percentile;
            opts.base_seed = cli.seed;

            auto opt = utilities::dlio::optimize_max_bound_percentile(
                ctx, *comp_model, traces.computation_times, opts);
            std::printf(
                "  Optimizer: iters=%d, best_percentile=%.2f%%, "
                "e2e_error=%.2f%%, "
                "fetch_block_cdf_similarity=%.4f%s\n",
                opt.iterations_used, opt.best_percentile,
                opt.best.e2e_error * 100.0, opt.best.fetch_block_cdf_similarity,
                opt.converged ? " (converged)" : "");

            auto comp_sorted = traces.computation_times;
            std::sort(comp_sorted.begin(), comp_sorted.end());
            const double comp_max_bound =
                utilities::dlio::percentile(comp_sorted, opt.best_percentile);

            double prep_max_bound = 0.0;
            if (prep_model) {
                auto prep_sorted = traces.preprocess_times;
                std::sort(prep_sorted.begin(), prep_sorted.end());
                prep_max_bound = utilities::dlio::percentile(
                    prep_sorted, opt.best_percentile);
            }

            // --- Emit YAML
            // -----------------------------------------------------------
            utilities::dlio::DlioTimingBlock comp_block{*comp_model,
                                                        comp_max_bound};
            dstd::Optional<utilities::dlio::DlioTimingBlock> prep_block_storage;
            const utilities::dlio::DlioTimingBlock* prep_block_ptr = nullptr;
            if (prep_model) {
                prep_block_storage = utilities::dlio::DlioTimingBlock{
                    *prep_model, prep_max_bound};
                prep_block_ptr = &(*prep_block_storage);
            }

            const dstd::String out_path(cli.output);
            dstd::ScopedFile out(std::fopen(out_path.c_str(), "w"));
            if (!out) {
                DFTRACER_UTILS_LOG_ERROR("Cannot open {} for writing",
                                         out_path);
                return 1;
            }
            dstd::String yaml =
                utilities::dlio::render_dlio_yaml(&comp_block, prep_block_ptr);
            yaml += "\n";
            const bool wrote = std::fwrite(yaml.data(), 1, yaml.size(),
                                           out.get()) == yaml.size();
            if (!wrote || std::fclose(out.release()) != 0) {
                DFTRACER_UTILS_LOG_ERROR("Failed to write {}", out_path);
                return 1;
            }
            std::printf("  Wrote DLIO config: %s\n",
                        dstd::String(cli.output).c_str());
            std::printf("==========================================\n");
            return 0;
        });
}
