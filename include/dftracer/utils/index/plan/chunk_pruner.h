// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_PLAN_CHUNK_PRUNER_H
#define DFTRACER_UTILS_INDEX_PLAN_CHUNK_PRUNER_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/runtime/coro/task.h>

#include <cstdint>
#include <string>
#include <vector>

namespace dftracer::utils::index {
class Snapshot;
}

namespace dftracer::utils::index::plan {

using duql::Query;

/// Input for chunk pruning: index path, file path, query, optional snapshot.
///
/// If `snapshot` is non-null the utility reads through it instead of opening
/// the index at `index_path` and taking its own. The caller keeps it alive for
/// the whole query, so many files pruned against one index share one.
struct ChunkPrunerInput {
    std::string index_path;  ///< Path to the `.dftindex` store.
    std::string file_path;   ///< Path to trace file.
    Query query;             ///< Query to evaluate for pruning.
    const index::Snapshot* snapshot = nullptr;  ///< Borrowed.
};

/// Result of chunk pruning.
struct ChunkPrunerOutput {
    bool file_may_match = false;          ///< True if any chunk may match.
    std::vector<std::uint64_t>
        candidate_checkpoints;            ///< Matching chunk indices.
    std::uint64_t total_checkpoints = 0;  ///< Total chunks in file.
    bool success = false;  ///< True if pruning completed without error.
};

/// Input for pruning many files that share one `.dftindex` store with one
/// snapshot.
struct ChunkPrunerBatchItem {
    std::string file_path;
    Query query;
};

struct ChunkPrunerBatchInput {
    std::string index_path;
    std::vector<ChunkPrunerBatchItem> items;
    const index::Snapshot* snapshot = nullptr;  ///< Borrowed, as above.
};

struct ChunkPrunerBatchOutput {
    std::vector<ChunkPrunerOutput> outputs;  ///< Parallel to items[].
};

/// Why a query reads the chunks it reads in one file.
struct ChunkExplanation {
    struct ByExtension {
        /// Such as "zonemap", or a plugin extension's registered name.
        std::string name;
        /// This extension alone proves no event of the file matches.
        bool file_ruled_out = false;
        /// Chunks this extension alone rules out, ascending.
        std::vector<std::uint64_t> removed;
    };
    /// The file is registered in the index.
    bool indexed = false;
    bool file_may_match = true;
    std::uint64_t total_chunks = 0;
    /// Chunks the query reads, ascending; the same chunks ChunkPruner
    /// returns.
    std::vector<std::uint64_t> read;
    /// One entry per pruning extension current for the file.
    std::vector<ByExtension> by_extension;
};

/// Explains the pruning of `file_path` for `query`. Throws on an index read
/// error, where ChunkPruner assumes a match.
ChunkExplanation explain_file_chunks(const index::Snapshot& snapshot,
                                     const std::string& file_path,
                                     const Query& query);

/// Chunk pruner over the index's pruning kinds (bloom, postings, counts,
/// zonemap). Walks the Query AST (AND intersects, OR unions, NOT keeps the
/// chunks where not every record is proven to match).
class ChunkPruner {
   public:
    coro::CoroTask<ChunkPrunerOutput> operator()(const ChunkPrunerInput& input);

    /// Prune many files against the same index with one snapshot.
    common::Result<ChunkPrunerBatchOutput> process_batch(
        const ChunkPrunerBatchInput& input);
};

}  // namespace dftracer::utils::index::plan

#endif
