#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_CHUNK_AGGREGATOR_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_CHUNK_AGGREGATOR_UTILITY_H

#include <dftracer/utils/core/utilities/tags/parallelizable.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/common/json/json_value.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_config.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_key.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_map.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_metrics.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_output.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/association_tracker.h>
#include <dftracer/utils/utilities/composites/dft/event.h>

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
using dftracer::utils::utilities::composites::dft::DFTracerEvent;

struct ChunkAggregatorInput {
    std::string file_path;
    std::string index_path;
    std::size_t start_byte = 0;
    std::size_t end_byte = 0;
    std::size_t start_line = 0;
    std::size_t end_line = 0;
    AggregationConfig config;
    std::optional<common::query::Query> query;
    std::size_t checkpoint_size = 0;
    int chunk_index = 0;

    std::size_t batch_size = 4 * 1024 * 1024;

    ChunkAggregatorInput& with_file_path(const std::string& path) {
        file_path = path;
        return *this;
    }

    ChunkAggregatorInput& with_index_path(const std::string& path) {
        index_path = path;
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
   public:
    ChunkAggregatorUtility() = default;

    coro::CoroTask<ChunkAggregationOutput> process(
        const ChunkAggregatorInput& input) override;
};

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_CHUNK_AGGREGATOR_UTILITY_H
