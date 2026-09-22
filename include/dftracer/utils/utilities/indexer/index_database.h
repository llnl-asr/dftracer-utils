#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_DATABASE_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_DATABASE_H

#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/utilities/indexer/types/types.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dftracer::utils::rocksdb {
class RocksDatabase;
}

namespace dftracer::utils::utilities::indexer {

class IndexDatabaseWriterContext;
class SstArtifactRegistry;

/// How an index database is opened. Mapped to the RocksDB open mode inside the
/// implementation; kept RocksDB-free here so the public header does not pull in
/// the RocksDB layer.
enum class IndexOpenMode { ReadOnly, ReadWrite };

/// The value type harvested for a groupable column at index build: what the
/// JSON scan observed (a scalar's shape), not what is stored on disk. Unknown
/// marks a record from an index built before schema v12 (no type persisted).
/// The on-disk column record stores the richer `dataframe::DataType` (schema
/// v15+); ColumnType stays the harvest-time vocabulary since a JSON arg value
/// is always one of these three scalar shapes or absent.
enum class ColumnType : std::uint8_t {
    Unknown = 0,
    Int64 = 1,
    Float64 = 2,
    String = 3,
};

/// Fold two observations of the same column's type into one: an Unknown yields
/// to the other, Int64 and Float64 widen to Float64, and any mix with String
/// widens to String. Commutative and associative, so it merges across files.
ColumnType merge_column_type(ColumnType a, ColumnType b);

/// Canonical lowercase name for a ColumnType ("int64"/"float64"/"string");
/// empty string for Unknown.
const char* column_type_name(ColumnType t);

/// The index database: the read/query surface plus the write/ingest/schema
/// surface over one on-disk `.dftindex`. The RocksDB engine and all mutable
/// state live behind an opaque `Impl`, so the public ABI is stable against
/// RocksDB and internal changes. Reads are `const`; a `const IndexDatabase&` is
/// therefore a read-only handle.
class IndexDatabase {
   public:
    explicit IndexDatabase(const std::string& index_path,
                           IndexOpenMode open_mode = IndexOpenMode::ReadWrite);

    IndexDatabase(const IndexDatabase&) = delete;
    IndexDatabase& operator=(const IndexDatabase&) = delete;
    IndexDatabase(IndexDatabase&&) noexcept;
    IndexDatabase& operator=(IndexDatabase&&) noexcept;
    ~IndexDatabase();

    /// The underlying RocksDB handle. Internal callers only; external users
    /// should stay on the typed query/write API.
    std::shared_ptr<dftracer::utils::rocksdb::RocksDatabase> db() const;

    IndexFileEntryCapability get_file_capabilities(int file_id) const;

    bool has_bloom_data(int file_id) const;

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

    /// v2 added per-file mtime/size to the registry record. v3 moved the file
    /// hash into the aggregation key and gave the point-lookup families bloom
    /// filters, neither of which is readable from an older index. v4 moved SSTs
    /// to format_version 7 with keys and values separated in the data block,
    /// which older RocksDB builds cannot read. v5 re-based the pruner chunk
    /// axis on gzip members instead of checkpoints, so chunk-keyed records of
    /// an older index address the wrong bytes. v6 dropped the zran checkpoint
    /// records the read path used to seek with. v7 renamed their column family
    /// to `members`, so an older index's member table is invisible. v8 -> v9
    /// keyed the file registry on the canonical absolute path instead of the
    /// bare filename, so an older index's file lookups miss under new code.
    /// v9 -> v10 added per-counter pid/tid to the ph="C" summary series, so an
    /// older summary would deserialize the new fields as counter bucket data.
    /// v10 -> v11 added the FieldStat numeric domain tag and exact integer
    /// sum/min/max to the serialized aggregation accumulator (persisted rollup
    /// records), so an older rollup lacks those bytes and would misparse.
    /// v11 -> v12 gave each harvested column record a one-byte value type
    /// (ColumnType), so an older index's columns read back with type Unknown.
    /// v12 -> v13 changed the persisted rollup from serialized
    /// GroupMap/AggAccum records to per-group engine AggState blobs
    /// (agg_serialize), so an older rollup's rows are an incompatible layout.
    /// v13 -> v14 appended the name-keyed dyn side-table to the AggState blob
    /// (a trailing has_dyn byte plus, when set, the dyn specs/domain/stats), so
    /// an older rollup blob lacks those bytes and would misparse.
    /// v14 -> v15 widened each harvested column record's value from a single
    /// ColumnType byte to a variable-length TLV encoding of the full
    /// `dataframe::DataType` (column_type_codec.h), so an older record's
    /// single byte would decode as a truncated/malformed type.
    static constexpr std::uint32_t SCHEMA_VERSION = 15;

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
    /// Compare on-disk trace files against the index. A file whose registry
    /// record is missing goes in `added`, one present but no longer matching
    /// (see check_freshness) in `changed`, and a registered path absent from
    /// the inputs in `removed`. Sets `schema_outdated` when the stored schema
    /// predates this build.
    StaleCheckResult find_stale_files(
        const std::vector<std::string>& current_paths) const;

    /// Outcome of a freshness check for one trace file against this index.
    enum class Freshness {
        Fresh,  ///< Index is current for the file.
        Stale,  ///< File is unregistered, or its mtime/size changed; rebuild.
        SchemaOutdated  ///< Whole index predates this build; rebuild all.
    };

    /// Freshness predicate shared by the read path and the server stale scan.
    /// Stat-only by design: it decides on schema version, registry presence,
    /// stored-stat presence, and mtime/size, and never reads the file body,
    /// since it runs on every index open and stale scan. A same-size edit with
    /// a restored mtime is therefore not detected. Build-option changes
    /// (checkpoint size, dimensions) are not a concern here; the index CLI
    /// rebuilds for those.
    Freshness check_freshness(const std::string& file_path) const;

    std::unordered_map<std::string, int> query_all_file_info_ids() const;
    std::unordered_map<std::string, FileRegistryEntry> query_all_file_registry()
        const;

    int find_file(std::string_view file_path) const;

    std::uint64_t get_checkpoint_size(int file_id) const;
    std::uint64_t get_num_lines(int file_id) const;
    std::uint64_t get_max_bytes(int file_id) const;

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

    /// Distinct groupable column names across all files in this index
    /// (top-level scalar fields + args keys), harvested at index build. Empty
    /// for indexes built before column discovery existed.
    std::vector<std::string> query_all_columns() const;

    /// As query_all_columns, but each name is paired with its harvested
    /// ColumnType (folded across files: numeric widens to Float64, any mix
    /// with a string widens to String). Type is Unknown for a record written
    /// before schema v12. Sorted by name.
    std::vector<std::pair<std::string, ColumnType>> query_all_column_types()
        const;

    /// As query_all_column_types, but each name is paired with the full
    /// stored `dataframe::DataType` rather than the coarser ColumnType.
    /// Folded across files via the same rule as query_all_column_types
    /// (numeric widens to Float64, any other mismatch - including two
    /// different nested shapes - widens to a scalar String). A pre-v15
    /// record decodes to a scalar DataType matching its old ColumnType byte;
    /// an undecodable (corrupt) record is treated as Unknown. Sorted by name.
    std::vector<std::pair<std::string, dataframe::DataType>>
    query_all_column_data_types() const;

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
    void merge_root_category_counts_into(ChunkStatistics& target) const;
    void merge_root_pid_tid_counts_into(ChunkStatistics& target) const;
    void merge_root_name_counts_into(ChunkStatistics& target) const;
    std::vector<int> query_name_file_postings(std::string_view name) const;
    std::vector<std::uint64_t> query_name_chunk_postings(std::string_view name,
                                                         int file_id) const;
    bool has_file_scalar_stats(int file_id) const;
    /// Member boundaries in file order, empty when the file was never
    /// indexed. Callers must fall back to a runtime member scan.
    std::vector<GzipMemberRecord> query_gzip_members(int file_id) const;

    /// Chunk spans in chunk-index order: `[i]` describes pruner chunk `i`,
    /// which is gzip member `i`.
    std::vector<ChunkSpan> query_chunk_spans(int file_id) const;

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

    // -----------------------------------------------------------------------
    // PID query API (for distributed aggregation); the distinct per-file PID
    // set is projected from FILE_PID_TID_COUNTS written by the bloom pass.
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
        FILE = 0,    ///< FH: file hash -> file name
        HOST = 1,    ///< HH: host hash -> host name
        STRING = 2,  ///< SH: string hash -> string value
        PROC = 3     ///< PR: proc hash -> proc metadata
    };

    /// Query all entries of a given hash type.
    /// Returns map of {hash_value -> resolved_name}.
    std::unordered_map<std::string, std::string> query_hash_table(
        HashType type) const;

    /// Resolve a single hash to its name.
    /// Returns nullopt if hash is not found.
    std::optional<std::string> resolve_hash(HashType type,
                                            std::string_view hash) const;

    /// Resolve one hash with a point lookup, without caching every hash table
    /// first. Preferred when only a few hashes are needed and the tables are
    /// large (a trace can declare millions of files).
    /// Number of hashes of `type`, counted by iterating rather than
    /// materialising the table: it can hold tens of millions of entries.
    std::uint64_t count_hash_entries(HashType type) const;

    std::optional<std::string> lookup_hash(HashType type,
                                           std::string_view hash) const;

    /// Resolve a name to its hash (reverse lookup for query DSL).
    /// Returns nullopt if name is not found.
    std::optional<std::string> resolve_name_to_hash(
        HashType type, std::string_view name) const;

    std::unique_ptr<IndexDatabaseWriterContext> begin_write();

    /// Ingest SST files produced by `IndexDatabaseSstWriterContext` instances.
    /// File-id ranges across input SSTs must be disjoint for the METADATA,
    /// MEMBERS, and MANIFEST column families (step-1 scope). No-op on an
    /// empty registry. Does NOT refresh root summaries; call
    /// `rebuild_root_summaries()` afterward once all ingest phases are done.
    ///
    /// `skip_cfs` optionally holds CF names (e.g. `cf::AGGREGATION`) whose
    /// SSTs must be left outside the unified DB. Distributed builds use this
    /// to keep per-worker AGGREGATION / SYSTEM_METRICS SSTs addressable by
    /// manifest for parallel reads at analyze time.
    /// Independent SSTs ingest concurrently on the process default runtime when
    /// set (aggregation/system-metrics stay in list order); sequential
    /// otherwise.
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
                                 std::uint32_t config_hash = 0,
                                 bool group_by_file = true);

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
    std::vector<int> register_files(const std::vector<std::string>& file_paths);

    /// Schema initialisation, idempotent
    void init_schema();

   private:
    void ensure_hash_tables_cached() const;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dftracer::utils::utilities::indexer

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_DATABASE_H
