#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_CHUNK_PRUNER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_CHUNK_PRUNER_UTILITY_H

#include <dftracer/utils/core/utilities/tags/parallelizable.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_filter_cache.h>

#include <cstdint>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::indexing {

using common::query::Query;

struct ChunkPrunerInput {
    std::string idx_path;
    std::string file_path;
    Query query;
    BloomFilterCache* cache = nullptr;
};

struct ChunkPrunerOutput {
    bool file_may_match = false;
    std::vector<std::uint64_t> candidate_checkpoints;
    std::uint64_t total_checkpoints = 0;
    bool success = false;
};

class ChunkPrunerUtility
    : public utilities::Utility<ChunkPrunerInput, ChunkPrunerOutput,
                                utilities::tags::Parallelizable> {
   public:
    ChunkPrunerUtility() = default;

    coro::CoroTask<ChunkPrunerOutput> process(
        const ChunkPrunerInput& input) override;
};

}  // namespace dftracer::utils::utilities::composites::dft::indexing

#endif
