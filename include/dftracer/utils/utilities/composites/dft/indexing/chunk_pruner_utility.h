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

/// Input for chunk pruning: index path, file path, query, optional cache.
struct ChunkPrunerInput {
    std::string index_path;             ///< Path to the `.dftindex` store.
    std::string file_path;              ///< Path to trace file.
    Query query;                        ///< Query to evaluate for pruning.
    BloomFilterCache* cache = nullptr;  ///< Optional bloom filter cache.
};

/// Result of chunk pruning.
struct ChunkPrunerOutput {
    bool file_may_match = false;          ///< True if any chunk may match.
    std::vector<std::uint64_t>
        candidate_checkpoints;            ///< Matching chunk indices.
    std::uint64_t total_checkpoints = 0;  ///< Total chunks in file.
    bool success = false;  ///< True if pruning completed without error.
};

/// Three-tier chunk pruner: dictionary → min/max range → bloom filter.
/// Walks the Query AST recursively (AND=intersect, OR=union, NOT=complement).
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
