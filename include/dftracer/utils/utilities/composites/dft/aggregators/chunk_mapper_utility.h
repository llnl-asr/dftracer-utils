#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_CHUNK_MAPPER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_CHUNK_MAPPER_UTILITY_H

#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_config.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/chunk_aggregator_utility.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::aggregators {

struct FileChunkMapperInput {
    utilities::composites::dft::MetadataCollectorUtilityOutput metadata;
    AggregationConfig config;
    std::optional<common::query::Query> query;
    std::size_t checkpoint_size = 0;
    std::size_t target_chunk_size_mb = 4;
    std::size_t batch_size = 4 * 1024 * 1024;
    int start_chunk_index = 0;

    static FileChunkMapperInput from_metadata(
        const utilities::composites::dft::MetadataCollectorUtilityOutput&
            meta) {
        FileChunkMapperInput input;
        input.metadata = meta;
        return input;
    }

    FileChunkMapperInput& with_config(const AggregationConfig& cfg) {
        config = cfg;
        return *this;
    }

    FileChunkMapperInput& with_checkpoint_size(std::size_t size) {
        checkpoint_size = size;
        return *this;
    }

    FileChunkMapperInput& with_target_chunk_size(std::size_t size_mb) {
        target_chunk_size_mb = size_mb;
        return *this;
    }

    FileChunkMapperInput& with_batch_size(std::size_t size_bytes) {
        batch_size = size_bytes;
        return *this;
    }

    FileChunkMapperInput& with_start_chunk_index(int index) {
        start_chunk_index = index;
        return *this;
    }
};

using FileChunkMapperOutput = std::vector<ChunkAggregatorInput>;

class FileChunkMapperUtility
    : public utilities::Utility<FileChunkMapperInput, FileChunkMapperOutput> {
   public:
    coro::CoroTask<FileChunkMapperOutput> process(
        const FileChunkMapperInput& input) override;
};

struct ChunkMapperInput {
    std::vector<utilities::composites::dft::MetadataCollectorUtilityOutput>
        metadata;
    AggregationConfig config;
    std::optional<common::query::Query> query;
    std::size_t checkpoint_size;
    std::size_t target_chunk_size_mb;
    std::size_t batch_size = 4 * 1024 * 1024;

    static ChunkMapperInput from_metadata(
        const std::vector<
            utilities::composites::dft::MetadataCollectorUtilityOutput>& meta) {
        ChunkMapperInput input;
        input.metadata = meta;
        input.target_chunk_size_mb = 4;
        return input;
    }

    ChunkMapperInput& with_config(const AggregationConfig& cfg) {
        config = cfg;
        return *this;
    }

    ChunkMapperInput& with_checkpoint_size(std::size_t size) {
        checkpoint_size = size;
        return *this;
    }

    ChunkMapperInput& with_target_chunk_size(std::size_t size_mb) {
        target_chunk_size_mb = size_mb;
        return *this;
    }

    ChunkMapperInput& with_batch_size(std::size_t size_bytes) {
        batch_size = size_bytes;
        return *this;
    }
};

using ChunkMapperOutput = std::vector<ChunkAggregatorInput>;

class ChunkMapperUtility
    : public utilities::Utility<ChunkMapperInput, ChunkMapperOutput> {
   public:
    coro::CoroTask<ChunkMapperOutput> process(
        const ChunkMapperInput& input) override;
};

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_CHUNK_MAPPER_UTILITY_H
