#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_DATABASE_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_DATABASE_H

#include <dftracer/utils/core/rocksdb/database.h>
#include <dftracer/utils/core/rocksdb/db_manager.h>
#include <dftracer/utils/utilities/indexer/index_types.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dftracer::utils::utilities::indexer {

class IndexDatabaseWriterContext;
class SstArtifactRegistry;

class IndexDatabase {
   public:
    explicit IndexDatabase(
        const std::string& index_path,
        dftracer::utils::rocksdb::RocksDatabase::OpenMode open_mode =
            dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadWrite);

    IndexDatabase(const IndexDatabase&) = delete;
    IndexDatabase& operator=(const IndexDatabase&) = delete;

    IndexDatabase(IndexDatabase&&) noexcept = default;
    IndexDatabase& operator=(IndexDatabase&&) noexcept = default;

    ~IndexDatabase() = default;

    std::unique_ptr<IndexDatabaseWriterContext> begin_write();

    /// Ingest SST files produced by `IndexDatabaseSstWriterContext` instances.
    /// File-id ranges across input SSTs must be disjoint for the METADATA,
    /// CHECKPOINTS, and MANIFEST column families (step-1 scope). No-op on an
    /// empty registry. Does NOT refresh root summaries; call
    /// `rebuild_root_summaries()` afterward once all ingest phases are done.
    ///
    /// `skip_cfs` optionally holds CF names (e.g. `cf::AGGREGATION`) whose
    /// SSTs must be left outside the unified DB. Distributed builds use this
    /// to keep per-worker AGGREGATION / SYSTEM_METRICS SSTs addressable by
    /// manifest for parallel reads at analyze time.
    void bulk_ingest(const SstArtifactRegistry& registry,
                     const std::unordered_set<std::string>& skip_cfs = {});

    /// Recompute ROOT_SCALAR_STATS, ROOT_{CAT,NAME,PID_TID}_COUNTS from the
    /// current per-file CFs. Call after `bulk_ingest` completes, or whenever
    /// root-level summaries need to be regenerated from scratch.
    void rebuild_root_summaries();

    /// Write the aggregation global-config key (0xFFFE) into the
    /// AGGREGATION CF. Required for `iter_arrow_dfanalyzer_all` to recognise
    /// the index as aggregator-populated. Distributed builds call this after
    /// `bulk_ingest(skip_cfs={aggregation, system_metrics})` so the unified
    /// DB has a config marker even though the AGG SSTs live in the manifest.
    /// `consolidate_index` invokes it too before the deferred AGG ingest.
    void write_agg_global_config(std::uint64_t time_interval_us,
                                 std::uint32_t config_hash = 0);

    /// Write per-file aggregation completion markers (0xFFFF + file_id BE)
    /// into the AGGREGATION CF. The index resolver treats these as "this
    /// file has aggregated data"; without them, `ensure_indexed()` concludes
    /// the aggregation tier is incomplete and re-runs the build. Distributed
    /// builds must call this after `bulk_ingest`, since per-worker SSTs
    /// carry data but not markers (markers are written via direct db->put,
    /// not via the SST sink).
    void write_agg_file_markers(const std::vector<int>& file_ids);

    /// Merge per-worker AssociationTracker blobs and write the result to
    /// the AGGREGATION CF under the `__tracker__` key.
    void write_aggregation_tracker(const std::vector<std::string>& blobs);

    /// Atomically reserve `count` contiguous file_ids, returning the first
    /// id in the range `[first, first + count)`. Intended for the
    /// distributed indexer: coordinator hands each worker its own disjoint
    /// range up front so workers need no cross-worker coordination.
    int reserve_file_id_range(std::size_t count);

    /// Register a list of trace files in the DEFAULT-CF file registry and
    /// return the assigned file_ids (parallel to `file_paths`). Idempotent:
    /// files already registered with a matching hash keep their existing
    /// id. Used by the distributed indexer's coordinator to pre-register
    /// every file before dispatching work to SST-backed workers, so workers
    /// never need to touch the DEFAULT column family themselves.
    std::vector<int> register_files(const std::vector<std::string>& file_paths,
                                    bool build_manifest);

    std::shared_ptr<dftracer::utils::rocksdb::RocksDatabase> db() const {
        return db_;
    }

    // Schema initialisation, idempotent
    void init_schema();

    // -----------------------------------------------------------------------
    // Read-only query API
    // -----------------------------------------------------------------------

    IndexFileEntryCapability get_file_capabilities(int file_id) const;

    bool has_bloom_data(int file_id) const;
    bool has_manifest_data(int file_id) const;

    int get_file_info_id(std::string_view path) const;
    std::optional<std::uint64_t> get_file_hash(std::string_view path) const;

    struct FileStat {
        std::uint64_t mtime;
        std::uint64_t size;
    };
    /// Stored mtime/size for `path` (matched by basename). nullopt if not
    /// registered or the record predates schema v2.
    std::optional<FileStat> get_file_stat(std::string_view path) const;

    /// Stored schema version, 0 if unset.
    std::uint32_t get_schema_version() const;

    /// True if the stored schema predates the current build's layout.
    bool schema_outdated() const;

    struct StaleCheckResult {
        std::vector<std::string> changed;
        std::vector<std::string> added;
        std::vector<std::string> removed;
        bool schema_outdated = false;
        bool stale() const {
            return schema_outdated || !changed.empty() || !added.empty() ||
                   !removed.empty();
        }
    };
    /// Stat-only (mtime + size) comparison of on-disk trace files against the
    /// index. If the stored schema predates mtime/size, all inputs are reported
    /// as `changed` and `schema_outdated` is set.
    StaleCheckResult find_stale_files(
        const std::vector<std::string>& current_paths) const;

    std::unordered_map<std::string, int> query_all_file_info_ids() const;
    std::unordered_map<std::string, FileRegistryEntry> query_all_file_registry()
        const;
    std::unordered_set<int> query_files_with_file_scalar_stats() const;
    std::unordered_set<int> query_files_with_bloom_data() const;

    int find_file(std::string_view file_path) const;

    std::uint64_t get_checkpoint_size(int file_id) const;
    std::uint64_t get_num_lines(int file_id) const;
    std::uint64_t get_max_bytes(int file_id) const;
    std::uint64_t get_total_events(int file_id) const;

    std::vector<ChunkBloomResult> query_chunk_bloom_filters(
        int file_id, std::string_view dimension) const;

    std::unordered_map<std::string, std::vector<ChunkBloomResult>>
    query_chunk_bloom_filters_batch(
        int file_id, const std::vector<std::string>& dimensions) const;

    std::optional<FileBloomResult> query_file_bloom_filter(
        int file_id, std::string_view dimension) const;

    std::unordered_map<std::string, FileBloomResult>
    query_file_bloom_filters_batch(
        int file_id, const std::vector<std::string>& dimensions) const;

    std::vector<std::string> query_index_dimensions(int file_id) const;

    bool has_index_dimension(int file_id, std::string_view dimension) const;

    std::vector<ChunkStatisticsResult> query_chunk_statistics(
        int file_id) const;
    std::unordered_map<int, std::vector<ChunkStatisticsResult>>
    query_chunk_statistics_batch(const std::vector<int>& file_ids) const;
    std::unordered_map<int, MergedStatisticsResult>
    query_merged_statistics_batch(const std::vector<int>& file_ids) const;
    std::unordered_map<int, MergedStatisticsResult>
    query_file_scalar_stats_batch(const std::vector<int>& file_ids) const;
    std::unordered_map<int, FileMetadataResult> query_file_metadata_batch(
        const std::vector<int>& file_ids) const;
    std::unordered_map<int, StringViewMap<std::uint64_t>>
    query_file_category_counts_batch(const std::vector<int>& file_ids) const;
    std::unordered_map<int, StringViewMap<std::uint64_t>>
    query_file_pid_tid_counts_batch(const std::vector<int>& file_ids) const;
    std::unordered_map<int, NameSummaryResult> query_file_name_summaries_batch(
        const std::vector<int>& file_ids) const;
    std::optional<RootStatisticsResult> query_root_scalar_stats() const;
    StringViewMap<std::uint64_t> query_root_category_counts() const;
    StringViewMap<std::uint64_t> query_root_pid_tid_counts() const;
    StringViewMap<std::uint64_t> query_root_name_counts() const;
    void merge_file_category_counts_batch_into(
        const std::vector<int>& file_ids,
        std::unordered_map<int, ChunkStatistics*>& targets) const;
    void merge_file_pid_tid_counts_batch_into(
        const std::vector<int>& file_ids,
        std::unordered_map<int, ChunkStatistics*>& targets) const;
    void merge_file_name_counts_batch_into(
        const std::vector<int>& file_ids,
        std::unordered_map<int, ChunkStatistics*>& targets) const;
    void merge_root_category_counts_into(ChunkStatistics& target) const;
    void merge_root_pid_tid_counts_into(ChunkStatistics& target) const;
    void merge_root_name_counts_into(ChunkStatistics& target) const;
    std::vector<int> query_name_file_postings(std::string_view name) const;
    std::vector<std::uint64_t> query_name_chunk_postings(std::string_view name,
                                                         int file_id) const;
    bool has_file_scalar_stats(int file_id) const;
    bool find_checkpoint(int file_id, std::size_t target_offset,
                         IndexerCheckpoint& checkpoint) const;
    std::vector<IndexerCheckpoint> query_checkpoints(int file_id) const;
    std::vector<IndexerCheckpoint> query_checkpoints_for_line_range(
        int file_id, std::uint64_t start_line, std::uint64_t end_line) const;
    std::optional<TarArchiveMetadata> query_tar_archive_metadata(
        int file_id) const;
    std::vector<TarFileRecord> query_tar_files(int file_id) const;
    bool find_tar_file(int file_id, std::string_view file_name,
                       TarFileRecord& record) const;
    std::vector<TarFileRecord> query_tar_files_in_range(
        int file_id, std::uint64_t start_offset,
        std::uint64_t end_offset) const;

    TimeBounds query_time_bounds(int file_id) const;

    std::vector<ChunkDimensionStatsResult> query_chunk_dimension_stats(
        int file_id) const;
    std::unordered_map<int, std::vector<ChunkDimensionStatsResult>>
    query_chunk_dimension_stats_batch(const std::vector<int>& file_ids) const;

    std::vector<ChunkDimensionStatsResult>
    query_chunk_dimension_stats_for_dimension(int file_id,
                                              std::string_view dimension) const;

    std::optional<std::uint64_t> query_name_id(std::string_view name) const;
    std::optional<std::string> query_name_by_id(std::uint64_t name_id) const;

    std::vector<EventRangeResult> query_event_ranges(int file_id) const;

    std::vector<EventRangeResult> query_event_ranges_for_checkpoint(
        int file_id, std::uint64_t checkpoint_idx) const;

    std::vector<MetadataLinesResult> query_metadata_lines(int file_id) const;

    std::vector<MetadataLinesResult> query_metadata_lines_for_checkpoint(
        int file_id, std::uint64_t checkpoint_idx) const;

    // -----------------------------------------------------------------------
    // PID manifest query API (for distributed aggregation)
    // -----------------------------------------------------------------------

    /// Query the set of PIDs observed in a specific file.
    std::unordered_set<std::uint64_t> query_file_pids(int file_id) const;

    /// Query the PIDs for all files at once.
    /// Returns {file_id -> set of PIDs}.
    std::unordered_map<int, std::unordered_set<std::uint64_t>>
    query_all_file_pids() const;

    // -----------------------------------------------------------------------
    // Hash table query API (FH/HH/SH/PR mappings)
    // -----------------------------------------------------------------------

    enum class HashType : std::uint8_t {
        FILE = 0,    // FH: file hash -> file name
        HOST = 1,    // HH: host hash -> host name
        STRING = 2,  // SH: string hash -> string value
        PROC = 3     // PR: proc hash -> proc metadata
    };

    /// Query all entries of a given hash type.
    /// Returns map of {hash_value -> resolved_name}.
    std::unordered_map<std::string, std::string> query_hash_table(
        HashType type) const;

    /// Resolve a single hash to its name.
    /// Returns nullopt if hash is not found.
    std::optional<std::string> resolve_hash(HashType type,
                                            std::string_view hash) const;

    /// Query all hash tables at once.
    /// Returns {type -> {hash -> name}}.
    std::unordered_map<HashType, std::unordered_map<std::string, std::string>>
    query_all_hash_tables() const;

    /// Resolve a name to its hash (reverse lookup for query DSL).
    /// Returns nullopt if name is not found.
    std::optional<std::string> resolve_name_to_hash(
        HashType type, std::string_view name) const;

   private:
    void ensure_hash_tables_cached() const;

    std::string db_path_;
    dftracer::utils::rocksdb::RocksDatabase::OpenMode open_mode_;
    std::shared_ptr<dftracer::utils::rocksdb::RocksDatabase> db_;

    struct HashCache {
        std::shared_mutex mutex;
        bool loaded = false;
        std::unordered_map<std::string, std::string> file_hash;
        std::unordered_map<std::string, std::string> host_hash;
        std::unordered_map<std::string, std::string> string_hash;
        std::unordered_map<std::string, std::string> proc_hash;
        std::unordered_map<std::string, std::string> file_name;
        std::unordered_map<std::string, std::string> host_name;
        std::unordered_map<std::string, std::string> string_name;
        std::unordered_map<std::string, std::string> proc_name;
    };
    mutable std::unique_ptr<HashCache> hash_cache_;
};

}  // namespace dftracer::utils::utilities::indexer

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_DATABASE_H
