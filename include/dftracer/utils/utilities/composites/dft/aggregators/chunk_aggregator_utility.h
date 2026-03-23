#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_CHUNK_AGGREGATOR_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_CHUNK_AGGREGATOR_UTILITY_H

#include <dftracer/utils/core/utilities/tags/parallelizable.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/common/json/json_value.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_config.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_key.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_metrics.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_output.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/association_tracker.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::aggregators {

// Import JsonValue from common json namespace
using dftracer::utils::utilities::common::json::JsonValue;

struct ChunkAggregatorInput {
    std::string file_path;
    std::string idx_path;
    std::size_t start_byte;
    std::size_t end_byte;
    std::size_t start_line;
    std::size_t end_line;
    AggregationConfig config;
    std::optional<common::query::Query> query;
    std::size_t checkpoint_size;
    int chunk_index;

    std::size_t batch_size = 4 * 1024 * 1024;

    ChunkAggregatorInput& with_file_path(const std::string& path) {
        file_path = path;
        return *this;
    }

    ChunkAggregatorInput& with_idx_path(const std::string& path) {
        idx_path = path;
        return *this;
    }

    ChunkAggregatorInput& with_byte_range(std::size_t start, std::size_t end) {
        start_byte = start;
        end_byte = end;
        return *this;
    }

    ChunkAggregatorInput& with_line_range(std::size_t start, std::size_t end) {
        start_line = start;
        end_line = end;
        return *this;
    }

    ChunkAggregatorInput& with_chunk_index(int index) {
        chunk_index = index;
        return *this;
    }

    ChunkAggregatorInput& with_config(const AggregationConfig& cfg) {
        config = cfg;
        return *this;
    }

    ChunkAggregatorInput& with_checkpoint_size(std::size_t size) {
        checkpoint_size = size;
        return *this;
    }

    ChunkAggregatorInput& with_batch_size(std::size_t size) {
        batch_size = size;
        return *this;
    }
};

class ChunkAggregatorUtility
    : public utilities::Utility<ChunkAggregatorInput, ChunkAggregationOutput,
                                utilities::tags::Parallelizable> {
   private:
    std::uint64_t compute_time_bucket(std::uint64_t timestamp,
                                      std::uint64_t duration,
                                      const AggregationConfig& config) const;

    AggregationKey build_key(
        const JsonValue& json, const JsonValue& args, std::uint64_t timestamp,
        std::uint64_t duration, const AggregationConfig& config,
        const std::shared_ptr<AssociationTracker>& local_tracker) const;

    void process_event(
        yyjson_val* event, const AggregationConfig& config,
        std::unordered_map<AggregationKey, AggregationMetrics,
                           AggregationKeyHash>& local_aggregations,
        const std::shared_ptr<AssociationTracker>& local_tracker);

   public:
    ChunkAggregatorUtility() = default;

    coro::CoroTask<ChunkAggregationOutput> process(
        const ChunkAggregatorInput& input) override;
};

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_CHUNK_AGGREGATOR_UTILITY_H
