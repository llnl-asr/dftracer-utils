#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_CHUNK_PRUNER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_CHUNK_PRUNER_UTILITY_H

#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_filter_cache.h>
#include <dftracer/utils/utilities/indexer/index_database.h>

#include <cstdint>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::indexing {

using common::query::Query;

/// Input for chunk pruning: index path, file path, query, optional cache.
///
/// If `external_db` is non-null the utility reuses that handle instead of
/// opening the RocksDB at `index_path` itself. This lets callers that
/// prune many files against the same directory-level index amortize the
/// (expensive) RocksDB open cost to once per batch rather than once per
/// file.
struct ChunkPrunerInput {
    std::string index_path;             ///< Path to the `.dftindex` store.
    std::string file_path;              ///< Path to trace file.
    Query query;                        ///< Query to evaluate for pruning.
    BloomFilterCache* cache = nullptr;  ///< Optional bloom filter cache.
    indexer::IndexDatabase* external_db = nullptr;  ///< Reused DB handle.
};

/// Result of chunk pruning.
struct ChunkPrunerOutput {
    bool file_may_match = false;          ///< True if any chunk may match.
    std::vector<std::uint64_t>
        candidate_checkpoints;            ///< Matching chunk indices.
    std::uint64_t total_checkpoints = 0;  ///< Total chunks in file.
    bool success = false;  ///< True if pruning completed without error.
};

/// Input for batched pruning across many files that share the same
/// `.dftindex` store. Allows a single RocksDB scan per column family to
/// populate per-file pruner contexts instead of one scan per file.
struct ChunkPrunerBatchItem {
    std::string file_path;
    Query query;
};

struct ChunkPrunerBatchInput {
    std::string index_path;
    std::vector<ChunkPrunerBatchItem> items;
    BloomFilterCache* cache = nullptr;
    indexer::IndexDatabase* external_db = nullptr;
};

struct ChunkPrunerBatchOutput {
    std::vector<ChunkPrunerOutput> outputs;  ///< Parallel to items[].
};

/// Three-tier chunk pruner: dictionary → min/max range → bloom filter.
/// Walks the Query AST recursively (AND=intersect, OR=union, NOT=complement).
class ChunkPrunerUtility
    : public utilities::Utility<ChunkPrunerInput, ChunkPrunerOutput> {
   public:
    ChunkPrunerUtility() = default;

    coro::CoroTask<ChunkPrunerOutput> process(
        const ChunkPrunerInput& input) override;

    /// Batch-prune many files against the same index with shared RocksDB
    /// range scans for dim_stats / chunk_statistics.
    Result<ChunkPrunerBatchOutput> process_batch(
        const ChunkPrunerBatchInput& input);
};

}  // namespace dftracer::utils::utilities::composites::dft::indexing

#endif
