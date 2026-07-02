#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/utils/timer.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_runner.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregators.h>

#include <sstream>
#include <string>
#include <vector>

#include "common_cli.h"

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites::dft::aggregators;

class AggregatorArgParse : public cli::ArgParse {
   public:
    cli::DirectoryArgs directory{
        cli::DirMode::DEFAULT_DOT,
        "Input directory containing .pfw or .pfw.gz files"};
    cli::PipelineArgs pipeline;
    cli::IndexingArgs indexing;
    cli::QueryArgs query_args{
        "Query DSL filter (e.g., 'cat == \"POSIX\" and dur > 1000')"};

    std::string output;
    double time_interval = 5000.0;
    std::string group_keys;
    std::string metric_fields;
    bool compress = false;
    int compression_level = 1;
    std::string boundary_events;
    bool no_track_parents = false;
    std::size_t chunk_size = 4;
    std::size_t read_batch_size = 4;
    std::string event_format = "counter";
    bool compute_percentiles = false;
    std::string percentiles = "0.25,0.5,0.75,0.90";
    double relative_accuracy = 0.01;
    std::string format = "json";
    bool no_default_args = false;

    explicit AggregatorArgParse(argparse::ArgumentParser& p) : ArgParse(p) {
        indexing.index_dir_help =
            "Directory to store index files (default: system temp directory)";
        indexing.force_help = "Force index recreation";
        schema(directory, pipeline, indexing, query_args);
    }

   protected:
    void register_args() override {
        parser()
            .add_argument("-o", "--output")
            .help("Output file path for aggregated counters")
            .default_value<std::string>("aggregated_output.json");

        parser()
            .add_argument("-t", "--time-interval")
            .help("Time interval in milliseconds for bucketing (default: 5000)")
            .scan<'g', double>()
            .default_value(5000.0);

        parser()
            .add_argument("-g", "--group-keys")
            .help(
                "Comma-separated extra group keys from args (e.g., "
                "epoch,step,level)")
            .default_value<std::string>("");

        parser()
            .add_argument("-m", "--metric-fields")
            .help(
                "Comma-separated custom metric fields from args (e.g., "
                "iter_count,num_events)")
            .default_value<std::string>("");

        parser()
            .add_argument("--compress")
            .help("Compress output using gzip")
            .default_value(false)
            .implicit_value(true);

        parser()
            .add_argument("--compression-level")
            .help("Gzip compression level (0-9, default: 1)")
            .scan<'d', int>()
            .default_value(1);

        parser()
            .add_argument("--boundary-events")
            .help(
                "Boundary event configuration: "
                "event_name:value_field:output_name "
                "(e.g., \"epoch.block:iter_count:epoch\")")
            .default_value<std::string>("");

        parser()
            .add_argument("--no-track-process-parents")
            .help(
                "Disable tracking of process parent relationships from "
                "fork/spawn")
            .default_value(false)
            .implicit_value(true);

        parser()
            .add_argument("--chunk-size")
            .help(
                "Target chunk size in MB for parallel processing (default: 4)")
            .scan<'d', std::size_t>()
            .default_value(static_cast<std::size_t>(4));

        parser()
            .add_argument("--read-batch-size")
            .help(
                "Batch read size in MB for stream processing (default: 4, "
                "higher = "
                "faster but more memory)")
            .scan<'d', std::size_t>()
            .default_value(static_cast<std::size_t>(4));

        parser()
            .add_argument("--event-format")
            .help(
                "Perfetto event format: 'counter' (ph=C, point-in-time, "
                "default), "
                "'async' (ph=b/e, async tracks with overlaps), "
                "'regular' (ph=X, duration events with original TID)")
            .default_value<std::string>("counter");

        parser()
            .add_argument("--compute-percentiles")
            .help(
                "Enable percentile/quantile computation using DDSketch (opt-in "
                "due "
                "to memory overhead)")
            .default_value(false)
            .implicit_value(true);

        parser()
            .add_argument("--percentiles")
            .help(
                "Comma-separated percentiles to compute (e.g., "
                "\"0.25,0.5,0.75,0.90\" for P25, P50, P75, P90)")
            .default_value<std::string>("0.25,0.5,0.75,0.90");

        parser()
            .add_argument("--relative-accuracy")
            .help(
                "Relative accuracy for DDSketch percentile estimation "
                "(default: 0.01 = 1%)")
            .scan<'g', double>()
            .default_value(0.01);

        parser()
            .add_argument("--format")
            .help(
                "Output format: 'json' (Perfetto JSON, default) or "
                "'arrow' (Arrow IPC file, .arrows extension)")
            .default_value<std::string>("json");

        parser()
            .add_argument("--no-default-args")
            .help(
                "Disable automatic aggregation of numeric event args "
                "(offset, whence, flags, etc.)")
            .default_value(false)
            .implicit_value(true);
    }

    void post_parse() override {
        output = parser().get<std::string>("--output");
        time_interval = parser().get<double>("--time-interval");
        group_keys = parser().get<std::string>("--group-keys");
        metric_fields = parser().get<std::string>("--metric-fields");
        compress = parser().get<bool>("--compress");
        compression_level = parser().get<int>("--compression-level");
        boundary_events = parser().get<std::string>("--boundary-events");
        no_track_parents = parser().get<bool>("--no-track-process-parents");
        chunk_size = parser().get<std::size_t>("--chunk-size");
        read_batch_size = parser().get<std::size_t>("--read-batch-size");
        event_format = parser().get<std::string>("--event-format");
        compute_percentiles = parser().get<bool>("--compute-percentiles");
        percentiles = parser().get<std::string>("--percentiles");
        relative_accuracy = parser().get<double>("--relative-accuracy");
        format = parser().get<std::string>("--format");
        no_default_args = parser().get<bool>("--no-default-args");
    }
};

int main(int argc, char** argv) {
    return cli::cli_main<AggregatorArgParse>(
        argc, argv, "dftracer_aggregator",
        "Aggregate DFTracer events into time-series counters using streaming "
        "coroutine pipeline with minimal memory footprint",
        [](AggregatorArgParse& cli) -> int {
            // Resolve enum-like CLI strings.
            PerfettoEventFormat event_format = PerfettoEventFormat::COUNTER;
            if (cli.event_format == "async") {
                event_format = PerfettoEventFormat::ASYNC;
            } else if (cli.event_format == "regular") {
                event_format = PerfettoEventFormat::REGULAR;
            } else if (cli.event_format != "counter") {
                DFTRACER_UTILS_LOG_ERROR(
                    "Invalid event format: %s (must be 'counter', 'async', or "
                    "'regular')",
                    cli.event_format.c_str());
                return 1;
            }

            // Output filename: append extension if missing.
            std::string output_file = cli.output;
            if (cli.format == AggregationConfig::FORMAT_ARROW) {
                output_file = cli::ensure_suffix(output_file, ".arrows");
            } else if (cli.compress) {
                output_file = cli::ensure_suffix(output_file, ".gz");
            }

            // Parse boundary events.
            std::vector<BoundaryEventConfig> boundary_events;
            {
                std::stringstream ss(cli.boundary_events);
                std::string item;
                while (std::getline(ss, item, ',')) {
                    std::stringstream item_ss(item);
                    std::string event_name, value_field, output_name;
                    if (std::getline(item_ss, event_name, ':') &&
                        std::getline(item_ss, value_field, ':') &&
                        std::getline(item_ss, output_name, ':')) {
                        BoundaryEventConfig bec;
                        bec.event_name = event_name;
                        bec.value_field = value_field;
                        bec.output_name = output_name;
                        boundary_events.push_back(bec);
                    }
                }
            }

            // Parse percentiles.
            std::vector<double> percentiles;
            if (cli.compute_percentiles) {
                for (const auto& p_str : cli::split_csv(cli.percentiles)) {
                    try {
                        double p = std::stod(p_str);
                        if (p < 0.0 || p > 1.0) {
                            DFTRACER_UTILS_LOG_ERROR(
                                "Invalid percentile value: %s (must be in "
                                "[0.0, 1.0])",
                                p_str.c_str());
                            return 1;
                        }
                        percentiles.push_back(p);
                    } catch (const std::exception&) {
                        DFTRACER_UTILS_LOG_ERROR(
                            "Failed to parse percentile: %s", p_str.c_str());
                        return 1;
                    }
                }
                if (percentiles.empty()) {
                    DFTRACER_UTILS_LOG_ERROR(
                        "No valid percentiles specified with "
                        "--compute-percentiles");
                    return 1;
                }
            }

            if (!cli.query_args.query.empty()) {
                DFTRACER_UTILS_LOG_WARN(
                    "--query is not yet supported in fused mode, ignoring");
            }

            AggregationConfig agg_config;
            agg_config.time_interval_us =
                static_cast<std::uint64_t>(cli.time_interval * 1000.0);
            agg_config.extra_group_keys = cli::split_csv(cli.group_keys);
            agg_config.custom_metric_fields = cli::split_csv(cli.metric_fields);
            agg_config.compute_statistics = true;
            agg_config.compute_percentiles = cli.compute_percentiles;
            agg_config.sketch_accuracy = cli.relative_accuracy;
            agg_config.percentiles = percentiles;
            agg_config.boundary_events = boundary_events;
            agg_config.track_process_parents = !cli.no_track_parents;
            agg_config.track_default_args = !cli.no_default_args;

            Timer stages_storage("dftracer_aggregator");
            Timer* stages =
                cli.pipeline.time_profiling ? &stages_storage : nullptr;

            AggregationRunInput input;
            input.log_dir = cli.directory.value;
            input.index_dir = cli.indexing.index_dir;
            input.agg_config = std::move(agg_config);
            input.pipeline_config =
                cli::build_pipeline_config("DFTracer Aggregator", cli.pipeline);
            input.output_file = std::move(output_file);
            input.output_format = cli.format;
            input.event_format = event_format;
            input.compress_output = cli.compress;
            input.compression_level = cli.compression_level;
            input.force_rebuild = cli.indexing.force;
            input.checkpoint_size = cli.indexing.checkpoint_size;
            input.stages = stages;
            input.verbose = true;

            auto result = run_aggregation(std::move(input)).get();
            return result ? 0 : 1;
        });
}
