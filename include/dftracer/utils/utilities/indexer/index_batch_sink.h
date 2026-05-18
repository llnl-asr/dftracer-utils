#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_BATCH_SINK_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_BATCH_SINK_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_dimension_stats.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_statistics.h>
#include <dftracer/utils/utilities/indexer/internal/checkpoint.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace dftracer::utils::utilities::indexer {

/// Abstract sink that accepts index records for a batch of files.
///
/// Two backends implement this:
///   - `IndexDatabaseWriterContext`: writes directly to a live RocksDB.
///   - `IndexDatabaseSstWriterContext`: writes to SST files for later bulk
///     ingest (process-safe fan-out for distributed indexing).
///
/// Only the step-1 subset of methods is abstracted here (file metadata,
/// checkpoints, manifest event ranges and metadata lines). Bloom/hash/stats
/// writes remain on the concrete type until their CFs are ported to SST.
class IndexBatchSink {
   public:
    using IndexerCheckpoint = internal::IndexerCheckpoint;
    using ChunkStatistics = composites::dft::indexing::ChunkStatistics;
    using ChunkDimensionStats = composites::dft::indexing::ChunkDimensionStats;

    virtual ~IndexBatchSink() = default;

    virtual void insert_file_metadata(int file_id,
                                      std::uint64_t checkpoint_size,
                                      std::uint64_t total_lines,
                                      std::uint64_t total_uc_size) = 0;

    virtual void insert_checkpoint(int file_id,
                                   const IndexerCheckpoint& checkpoint) = 0;

    virtual void insert_event_range(
        int file_id, std::uint64_t checkpoint_idx, std::string_view cat,
        std::string_view name, std::span<const std::uint32_t> line_numbers) = 0;

    virtual void insert_metadata_lines(
        int file_id, std::uint64_t checkpoint_idx, std::string_view meta_type,
        std::span<const std::uint32_t> line_numbers) = 0;

    virtual void insert_file_pids(
        int file_id, const std::unordered_set<std::uint64_t>& pids) = 0;

    // Bloom / stats / dimension CFs --------------------------------------

    virtual void insert_chunk_bloom_filter(
        int file_id, std::uint64_t checkpoint_idx, std::string_view dimension,
        std::span<const unsigned char> blob_data,
        std::uint64_t num_entries) = 0;

    virtual void insert_file_bloom_filter(
        int file_id, std::string_view dimension,
        std::span<const unsigned char> blob_data,
        std::uint64_t num_entries) = 0;

    virtual void insert_chunk_statistics(int file_id,
                                         std::uint64_t checkpoint_idx,
                                         const ChunkStatistics& stats) = 0;

    virtual void insert_file_scalar_stats(int file_id,
                                          const ChunkStatistics& stats,
                                          std::uint64_t num_chunks) = 0;

    virtual void insert_file_category_counts(
        int file_id, const StringViewMap<std::uint64_t>& counts) = 0;

    virtual void insert_file_pid_tid_counts(
        int file_id, const StringViewMap<std::uint64_t>& counts) = 0;

    virtual void insert_file_name_counts(
        int file_id, const StringViewMap<std::uint64_t>& counts) = 0;

    virtual void insert_index_dimension(int file_id,
                                        std::string_view dimension) = 0;

    virtual void insert_chunk_dimension_stats(
        int file_id, std::uint64_t checkpoint_idx,
        const ChunkDimensionStats& stats,
        std::size_t value_counts_cap = 4096) = 0;

    // Name dictionary + postings. `name_id` is a 64-bit FNV1a hash of `name`
    // (deterministic, stateless), so multiple workers can emit the same
    // (name_id, name) pair without coordination. Dictionary duplicates are
    // dropped via `ingest_behind=true` at bulk-ingest time. Posting keys
    // include the file_id, which is worker-disjoint, so they need no
    // coordination either.
    virtual void insert_name_dictionary_entry(std::uint64_t name_id,
                                              std::string_view name) = 0;

    virtual void insert_name_file_posting(std::uint64_t name_id,
                                          int file_id) = 0;

    virtual void insert_name_chunk_posting(std::uint64_t name_id, int file_id,
                                           std::uint64_t checkpoint_idx) = 0;

    // Content-addressed hash table (FH/HH/SH/PR). Writes both the forward
    // (hash -> name) and reverse (name -> hash) entries. Deterministic keys
    // mean different workers emit identical (key, value) pairs for shared
    // hashes; cross-worker duplicates are resolved at read time via the LSM
    // sequence number.
    virtual void insert_hash_table_entry(std::uint8_t type,
                                         std::string_view hash,
                                         std::string_view name) = 0;

    // Aggregation column family. The AGGREGATION CF holds a mix of
    // Merge-operand records (per-`(pid, time_bucket, ...)` aggregated
    // stats) and Put records (intern-dictionary entries using the
    // AGG_INTERN_DICT_PREFIX prefix, global-config key, per-file
    // completion markers, and EventAggregator finalization metadata).
    // A rocksdb merge_operator collapses Merge operands at read/compaction
    // time; the concrete writer routes to `db_->merge` / `db_->put` via
    // the shared WriteBatch, the SST writer buffers `(key, value, op_kind)`
    // tuples and emits a mixed-op SST on commit.
    virtual void insert_aggregation_merge(std::string_view key,
                                          std::string_view operand) = 0;

    virtual void insert_aggregation_put(std::string_view key,
                                        std::string_view value) = 0;

    // System-metrics column family. Merge-operand only in practice (no
    // intern dictionary sidecar).
    virtual void insert_system_metrics_merge(std::string_view key,
                                             std::string_view operand) = 0;

    // Convenience overloads forwarding to span variants; concrete classes
    // need not override.
    void insert_event_range(int file_id, std::uint64_t checkpoint_idx,
                            std::string_view cat, std::string_view name,
                            const std::vector<std::uint32_t>& line_numbers) {
        insert_event_range(file_id, checkpoint_idx, cat, name,
                           std::span<const std::uint32_t>(line_numbers));
    }

    void insert_metadata_lines(int file_id, std::uint64_t checkpoint_idx,
                               std::string_view meta_type,
                               const std::vector<std::uint32_t>& line_numbers) {
        insert_metadata_lines(file_id, checkpoint_idx, meta_type,
                              std::span<const std::uint32_t>(line_numbers));
    }
};

}  // namespace dftracer::utils::utilities::indexer

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_BATCH_SINK_H
