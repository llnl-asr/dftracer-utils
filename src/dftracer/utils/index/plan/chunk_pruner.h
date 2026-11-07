// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_PLAN_CHUNK_PRUNER_H
#define DFTRACER_UTILS_INDEX_PLAN_CHUNK_PRUNER_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/query.h>

#include <cstdint>

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
    dstd::String index_path;  ///< Path to the `.dftindex` store.
    dstd::String file_path;   ///< Path to trace file.
    Query query;              ///< Query to evaluate for pruning.
    const index::Snapshot* snapshot = nullptr;  ///< Borrowed.
};

/// Result of chunk pruning.
struct ChunkPrunerOutput {
    bool file_may_match = false;          ///< True if any chunk may match.
    dstd::Vector<std::uint64_t>
        candidate_checkpoints;            ///< Matching chunk indices.
    std::uint64_t total_checkpoints = 0;  ///< Total chunks in file.
    bool success = false;  ///< True if pruning completed without error.
};

/// Input for pruning many files that share one `.dftindex` store with one
/// snapshot.
struct ChunkPrunerBatchItem {
    dstd::String file_path;
    Query query;
};

struct ChunkPrunerBatchInput {
    dstd::String index_path;
    dstd::Vector<ChunkPrunerBatchItem> items;
    const index::Snapshot* snapshot = nullptr;  ///< Borrowed, as above.
};

struct ChunkPrunerBatchOutput {
    dstd::Vector<ChunkPrunerOutput> outputs;  ///< Parallel to items[].
};

/// Why a query reads the chunks it reads in one file.
struct ChunkExplanation {
    struct ByExtension {
        /// Such as "zonemap", or a plugin extension's registered name.
        dstd::String name;
        /// This extension alone proves no event of the file matches.
        bool file_ruled_out = false;
        /// Chunks this extension alone rules out, ascending.
        dstd::Vector<std::uint64_t> removed;
    };
    /// The file is registered in the index.
    bool indexed = false;
    bool file_may_match = true;
    std::uint64_t total_chunks = 0;
    /// Chunks the query reads, ascending; the same chunks @ref ChunkPruner
    /// returns.
    dstd::Vector<std::uint64_t> read;
    /// One entry per pruning extension current for the file.
    dstd::Vector<ByExtension> by_extension;
};

/// Explains the pruning of `file_path` for `query`. Throws on an index read
/// error, where @ref ChunkPruner assumes a match.
ChunkExplanation explain_file_chunks(const index::Snapshot& snapshot,
                                     const dstd::String& file_path,
                                     const Query& query);

/// Chunk pruner over the index's pruning kinds (bloom, postings, counts,
/// zonemap). Walks the Query AST (AND intersects, OR unions, NOT keeps the
/// chunks where not every record is proven to match).
class ChunkPruner {
   public:
    coro::CoroTask<ChunkPrunerOutput> operator()(const ChunkPrunerInput& input);

    /// Prune many files against the same index with one snapshot.
    dstd::Result<ChunkPrunerBatchOutput> process_batch(
        const ChunkPrunerBatchInput& input);
};

}  // namespace dftracer::utils::index::plan

#endif
