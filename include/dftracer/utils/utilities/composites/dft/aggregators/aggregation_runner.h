#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_RUNNER_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_RUNNER_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/utils/timer.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_config.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/perfetto_trace_writer_utility.h>

#include <cstddef>
#include <optional>
#include <string>

namespace dftracer::utils::utilities::composites::dft::aggregators {

// Input bundle for the aggregation pipeline. Callers (binaries) translate their
// CLI flags into this struct, then call run_aggregation().
struct AggregationRunInput {
    // Raw trace input.
    std::string log_dir;           // directory containing .pfw[.gz]
    std::string index_dir;         // where the shared RocksDB lives
    AggregationConfig agg_config;  // time_interval_us, sketch, etc.
    ::dftracer::utils::PipelineConfig
        pipeline_config;           // executor threads, etc.

    // Optional output writer. When `output_file` is std::nullopt the run only
    // populates the AGGREGATION column family in the RocksDB and skips Perfetto
    // / Arrow emission; downstream consumers (e.g. dftracer_gen_dlio_config)
    // open the CF directly. When set, the output is written in `output_format`.
    std::optional<std::string> output_file;
    std::string output_format = AggregationConfig::FORMAT_JSON;
    PerfettoEventFormat event_format = PerfettoEventFormat::COUNTER;
    bool compress_output = false;
    int compression_level = 1;

    // Indexing controls.
    bool force_rebuild = false;
    std::size_t checkpoint_size = 0;

    // Optional staged Timer for profiling. Caller owns lifetime.
    ::dftracer::utils::Timer* stages = nullptr;
    bool verbose = true;  // controls console banner output
};

struct AggregationRunResult {
    bool success = false;

    // Path to the shared RocksDB index that now contains the AGGREGATION CF.
    // Downstream tools (dftracer_gen_dlio_config) open this read-only.
    std::string index_path;

    std::size_t total_keys = 0;
    std::size_t input_file_count = 0;
    std::size_t processed_file_count = 0;
    std::size_t cached_file_count = 0;

    double elapsed_ms = 0.0;
};

// Runs the full index + aggregate pipeline:
//   1. Scan log_dir for input files; consult the existing index.
//   2. Re-index any file that needs it (or all, if force_rebuild).
//   3. Run the aggregation visitor pipeline across all files needing it.
//   4. Optionally write the aggregated events to a Perfetto JSON / Arrow IPC
//      file (when input.output_file is set).
//   5. Write per-file tracking entries and global config to the AGGREGATION CF.
coro::CoroTask<AggregationRunResult> run_aggregation(AggregationRunInput input);

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif
