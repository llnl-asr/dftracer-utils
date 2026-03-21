#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATOR_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATOR_UTILITY_H

#include <dftracer/utils/core/utilities/streaming_utility.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_config.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_key.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_metrics.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/utilities/common/arrow/arrow_export.h>
#endif

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::aggregators {

struct AggregatorInput {
    std::string directory;
    AggregationConfig config;
    std::optional<common::query::Query> query;
    std::size_t checkpoint_size = 32 * 1024 * 1024;
    std::string index_dir;
    bool force_rebuild = false;
    std::size_t chunk_size_mb = 64;
    std::size_t batch_size_mb = 4;
    std::size_t event_batch_size = 10000;

    AggregatorInput& with_directory(const std::string& dir);
    AggregatorInput& with_config(const AggregationConfig& cfg);
    AggregatorInput& with_checkpoint_size(std::size_t sz);
    AggregatorInput& with_index_dir(const std::string& dir);
    AggregatorInput& with_force_rebuild(bool force);
    AggregatorInput& with_chunk_size_mb(std::size_t mb);
    AggregatorInput& with_batch_size_mb(std::size_t mb);
    AggregatorInput& with_event_batch_size(std::size_t sz);
};

struct AggregationBatch {
    std::vector<std::pair<AggregationKey, AggregationMetrics>> entries;
    std::size_t total_events_processed = 0;
    std::size_t total_files_processed = 0;
    std::size_t total_bytes_processed = 0;

#ifdef DFTRACER_UTILS_ENABLE_ARROW
    common::arrow::ArrowExportResult to_arrow() const;
#endif
};

class AggregatorUtility
    : public StreamingUtility<AggregatorInput, AggregationBatch> {
   public:
    coro::AsyncGenerator<AggregationBatch> process(
        const AggregatorInput& input) override;
};

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATOR_UTILITY_H
