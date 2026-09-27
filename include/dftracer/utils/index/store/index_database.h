#ifndef DFTRACER_UTILS_INDEX_STORE_INDEX_DATABASE_H
#define DFTRACER_UTILS_INDEX_STORE_INDEX_DATABASE_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/index/store/file.h>
#include <dftracer/utils/index/store/types.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::index::store {
class RocksDatabase;
}

namespace dftracer::utils::index::store {

class IndexDatabaseWriterContext;
class SstArtifactRegistry;

/// How an index database is opened. Mapped to the RocksDB open mode inside the
/// implementation; kept RocksDB-free here so the public header does not pull in
/// the RocksDB layer.
enum class IndexOpenMode { ReadOnly, ReadWrite };

/// The type a column reports in a schema, derived from the path catalog.
enum class ColumnType : std::uint8_t {
    Unknown = 0,
    Int64 = 1,
    Float64 = 2,
    String = 3,
    /// Text in some records and numbers or bools in others: a JSON column.
    Json = 4,
};

/// Fold two observations of the same column's type into one: an Unknown yields
/// to the other, Int64 and Float64 widen to Float64, and a mix of String with a
/// number, or anything with Json, is Json. Commutative and associative, so it
/// merges across files.
ColumnType merge_column_type(ColumnType a, ColumnType b);

/// Canonical lowercase name for a ColumnType ("int64"/"float64"/"string"/
/// "json");
/// empty string for Unknown.
const char* column_type_name(ColumnType t);

/// The index database: the read/query surface plus the write/ingest/format
/// surface over one on-disk `.dftindex`. The RocksDB engine and all mutable
/// state live behind an opaque `Impl`. Reads are `const`; a
/// `const IndexDatabase&` is therefore a read-only handle.
class IndexDatabase {
   public:
    /// On-disk layout version. An index with another stored version is
    /// rebuilt, never read.
    static constexpr std::uint32_t FORMAT_VERSION = 1;

    explicit IndexDatabase(const std::string& index_path,
                           IndexOpenMode open_mode = IndexOpenMode::ReadWrite);

    IndexDatabase(const IndexDatabase&) = delete;
    IndexDatabase& operator=(const IndexDatabase&) = delete;
    IndexDatabase(IndexDatabase&&) noexcept;
    IndexDatabase& operator=(IndexDatabase&&) noexcept;
    ~IndexDatabase();

    /// The underlying RocksDB handle. Internal callers only; external users
    /// should stay on the typed query/write API.
    std::shared_ptr<index::store::RocksDatabase> db() const;

    /// The manifest entry of `ext` for `file_id`; nullopt when never built.
    std::optional<ExtensionState> extension_state(int file_id,
                                                  IndexExtension ext) const;

    /// The manifest entry of plugin extension `name`; nullopt when never
    /// built.
    std::optional<ExtensionState> plugin_extension_state(
        int file_id, std::string_view name) const;
    /// Every plugin extension built for the file, by name.
    std::vector<std::pair<std::string, ExtensionState>> plugin_extension_states(
        int file_id) const;

    /// Built, not failed, and at the current code version. Data of an
    /// extension that is not current is never read for pruning.
    bool extension_current(int file_id, IndexExtension ext) const;

    /// The tier was built for the file: dft.stats, which every tier build
    /// writes, is current. Each pruning extension is checked on its own when
    /// read, so a subset build still counts.
    bool pruning_tier_current(int file_id) const;

    /// Paths a path-keyed extension indexed for the file.
    std::vector<std::string> extension_paths(int file_id,
                                             IndexExtension ext) const;
    /// (granule, payload) of `path` for the file, in granule order. Values
    /// with another type or version header are left out.
    std::vector<std::pair<std::uint64_t, std::string>> path_granules(
        int file_id, IndexExtension ext, std::string_view path) const;
    /// The file-level payload of `path`, if any.
    std::optional<std::string> path_file_value(int file_id, IndexExtension ext,
                                               std::string_view path) const;
    /// Postings of `path`: whether the file has any, whether it holds
    /// `value_hash`, and the granules that do.
    bool has_postings(int file_id, std::string_view path) const;
    bool has_posting(int file_id, std::string_view path,
                     std::uint64_t value_hash) const;
    std::vector<std::uint64_t> posting_granules(int file_id,
                                                std::string_view path,
                                                std::uint64_t value_hash) const;

    int get_file_info_id(std::string_view path) const;
    std::optional<std::uint64_t> get_file_hash(std::string_view path) const;

    struct FileStat {
        std::uint64_t mtime;
        std::uint64_t size;
    };
    /// Stored mtime/size for `path`; nullopt if not registered.
    std::optional<FileStat> get_file_stat(std::string_view path) const;

    /// Stored format version, 0 if unset or from an older layout.
    std::uint32_t get_format_version() const;

    /// True if the stored format differs from FORMAT_VERSION.
    bool format_outdated() const;

    struct StaleCheckResult {
        std::vector<std::string> changed;
        std::vector<std::string> added;
        std::vector<std::string> removed;
        bool format_outdated = false;
        bool stale() const {
            return format_outdated || !changed.empty() || !added.empty() ||
                   !removed.empty();
        }
    };
    /// Compare on-disk trace files against the index. A file whose registry
    /// record is missing goes in `added`, one present but no longer matching
    /// (see check_freshness) in `changed`, and a registered path absent from
    /// the inputs in `removed`. Sets `format_outdated` when the stored format
    /// differs from this build's.
    StaleCheckResult find_stale_files(
        const std::vector<std::string>& current_paths) const;

    /// Outcome of a freshness check for one trace file against this index.
    enum class Freshness {
        Fresh,          ///< Index is current for the file.
        Stale,          ///< File is unregistered, or its mtime/size changed.
        FormatOutdated  ///< Whole index has another format; rebuild all.
    };

    /// Freshness predicate shared by the read path and the server stale scan.
    /// Stat-only by design: it decides on format version, registry presence
    /// and mtime/size, and never reads the file body, since it runs on every
    /// index open and stale scan. A same-size edit with a restored mtime is
    /// therefore not detected. Build-option changes are the Resolver's
    /// concern (the manifest), not this check's.
    Freshness check_freshness(const std::string& file_path) const;

    StringViewMap<int> query_all_file_info_ids() const;
    StringViewMap<FileRegistryEntry> query_all_file_registry() const;

    int find_file(std::string_view file_path) const;

    std::uint64_t get_checkpoint_size(int file_id) const;
    std::uint64_t get_num_lines(int file_id) const;
    std::uint64_t get_max_bytes(int file_id) const;

    /// Distinct column names across all files in this index, as the schema
    /// names them: catalog paths with a leading "args." removed.
    std::vector<std::string> query_all_columns() const;

    /// As query_all_columns, with each column's type: BOOL and INT alone are
    /// Int64, any STRING or MIXED is String, other numbers are Float64,
    /// folded across files with merge_column_type. Null-only paths are left
    /// out. Sorted by name.
    std::vector<std::pair<std::string, ColumnType>> query_all_column_types()
        const;

    /// The id of the schema the file was indexed with; nullopt when it has
    /// no current schema record.
    std::optional<std::string> file_schema(int file_id) const;

    /// The file's catalog paths as written, with query_all_column_types'
    /// types, sorted by path.
    std::vector<std::pair<std::string, ColumnType>> file_column_types(
        int file_id) const;

    /// The file's path catalog, sorted by path; empty when it has none.
    std::vector<std::pair<std::string, PathStat>> catalog(int file_id) const;

    std::vector<index::schemas::dft::ChunkStatisticsResult>
    query_chunk_statistics(int file_id) const;
    /// Metadata counts by chunk; nullopt when dft.metadata is not current
    /// for the file, so no chunk's metadata is known.
    std::optional<ankerl::unordered_dense::map<std::uint64_t, ChunkMetadata>>
    chunk_metadata(int file_id) const;
    ankerl::unordered_dense::map<
        int, std::vector<index::schemas::dft::ChunkStatisticsResult>>
    query_chunk_statistics_batch(const std::vector<int>& file_ids) const;
    ankerl::unordered_dense::map<int,
                                 index::schemas::dft::MergedStatisticsResult>
    query_file_scalar_stats_batch(const std::vector<int>& file_ids) const;
    ankerl::unordered_dense::map<int, FileMetadataResult>
    query_file_metadata_batch(const std::vector<int>& file_ids) const;
    ankerl::unordered_dense::map<int, StringViewMap<std::uint64_t>>
    query_file_pid_tid_counts_batch(const std::vector<int>& file_ids) const;

    /// Member boundaries in file order, empty when the file has no current
    /// member table. Callers must fall back to a runtime member scan.
    std::vector<index::gzip::GzipMemberRecord> query_gzip_members(
        int file_id) const;

    /// Chunk spans in chunk-index order: `[i]` describes pruner chunk `i`,
    /// which is gzip member `i`.
    std::vector<index::gzip::ChunkSpan> query_chunk_spans(int file_id) const;

    // -----------------------------------------------------------------------
    // PID query API (for distributed aggregation); the distinct per-file PID
    // set is projected from the file's pid:tid counts.
    // -----------------------------------------------------------------------

    /// Query the set of PIDs observed in a specific file.
    ankerl::unordered_dense::set<std::uint64_t> query_file_pids(
        int file_id) const;

    /// Query the PIDs for all files at once.
    /// Returns {file_id -> set of PIDs}.
    ankerl::unordered_dense::map<int,
                                 ankerl::unordered_dense::set<std::uint64_t>>
    query_all_file_pids() const;

    /// The rows of row set `name` the build stored for `file_id`, as an
    /// Arrow IPC frame; nullopt when the file's row sets are not current.
    std::optional<std::string> rowset(int file_id, std::string_view name) const;

    std::unique_ptr<IndexDatabaseWriterContext> begin_write();

    /// Ingest, in one atomic call, the SSTs of `IndexDatabaseSstWriterContext`
    /// instances with disjoint file ids. No-op on an empty registry.
    ///
    /// `skip_cfs` optionally holds CF names (e.g. `cf::AGGREGATION`) whose
    /// SSTs must be left outside the unified DB. Distributed builds use this
    /// to keep per-worker AGGREGATION / SYSTEM_METRICS SSTs addressable by
    /// manifest for parallel reads at analyze time.
    void bulk_ingest(const SstArtifactRegistry& registry,
                     const StringViewSet& skip_cfs = {});

    /// Atomically reserve `count` contiguous file_ids, returning the first
    /// id in the range `[first, first + count)`. Intended for the
    /// distributed indexer: coordinator hands each worker its own disjoint
    /// range up front so workers need no cross-worker coordination.
    int reserve_file_id_range(std::size_t count);

    /// The file ids of `file_paths` (parallel): the registered id of a known
    /// path, a newly reserved one otherwise. Only the id counter is written;
    /// the file record is written with the file's data.
    std::vector<int> assign_file_ids(
        const std::vector<std::string>& file_paths);

    /// Writes the format version and extension registry when absent.
    void init_schema();

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dftracer::utils::index::store

#endif  // DFTRACER_UTILS_INDEX_STORE_INDEX_DATABASE_H
