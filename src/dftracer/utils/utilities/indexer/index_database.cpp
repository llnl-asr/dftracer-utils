#include <dftracer/utils/core/sqlite/statement.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/manifest_queries.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/error.h>

namespace dftracer::utils::utilities::indexer {

namespace queries = composites::dft::indexing::queries;

using dftracer::utils::sqlite::SqliteStmt;
using internal::IndexerError;

// ---------------------------------------------------------------------------
// Schema strings
// ---------------------------------------------------------------------------

// Matches GzipIndexer schema (gzip/constants.cpp) so IndexDatabase
// can open .idx files created by the existing indexer.
static const char* BASE_SCHEMA = R"(
    PRAGMA journal_mode=WAL;
    PRAGMA busy_timeout=5000;
    PRAGMA foreign_keys=ON;

    CREATE TABLE IF NOT EXISTS files (
        id            INTEGER PRIMARY KEY,
        logical_name  TEXT UNIQUE NOT NULL,
        byte_size     INTEGER NOT NULL DEFAULT 0,
        mtime_unix    INTEGER NOT NULL DEFAULT 0,
        hash          INTEGER NOT NULL DEFAULT 0
    );

    CREATE TABLE IF NOT EXISTS checkpoints (
        id              INTEGER PRIMARY KEY,
        file_id         INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
        checkpoint_idx  INTEGER NOT NULL,
        uc_offset       INTEGER NOT NULL DEFAULT 0,
        uc_size         INTEGER NOT NULL DEFAULT 0,
        c_offset        INTEGER NOT NULL DEFAULT 0,
        c_size          INTEGER NOT NULL DEFAULT 0,
        bits            INTEGER NOT NULL DEFAULT 0,
        dict_compressed BLOB,
        num_lines       INTEGER NOT NULL DEFAULT 0,
        first_line_num  INTEGER NOT NULL DEFAULT 0,
        last_line_num   INTEGER NOT NULL DEFAULT 0
    );

    CREATE INDEX IF NOT EXISTS checkpoints_file_idx
        ON checkpoints(file_id, checkpoint_idx);
    CREATE INDEX IF NOT EXISTS checkpoints_file_uc_off_idx
        ON checkpoints(file_id, uc_offset);
    CREATE INDEX IF NOT EXISTS checkpoints_line_range_idx
        ON checkpoints(file_id, first_line_num, last_line_num);

    CREATE TABLE IF NOT EXISTS metadata (
        file_id         INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
        checkpoint_size INTEGER NOT NULL DEFAULT 0,
        total_lines     INTEGER NOT NULL DEFAULT 0,
        total_uc_size   INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY(file_id)
    );
)";

static const char* BLOOM_SCHEMA = R"(
    CREATE TABLE IF NOT EXISTS chunk_bloom_filters (
        id INTEGER PRIMARY KEY,
        file_info_id INTEGER NOT NULL,
        checkpoint_idx INTEGER NOT NULL,
        dimension TEXT NOT NULL,
        bloom_data BLOB NOT NULL,
        num_entries INTEGER NOT NULL,
        UNIQUE(file_info_id, checkpoint_idx, dimension)
    );

    CREATE TABLE IF NOT EXISTS file_bloom_filters (
        id INTEGER PRIMARY KEY,
        file_info_id INTEGER NOT NULL,
        dimension TEXT NOT NULL,
        bloom_data BLOB NOT NULL,
        num_entries INTEGER NOT NULL,
        UNIQUE(file_info_id, dimension)
    );

    CREATE TABLE IF NOT EXISTS chunk_statistics (
        id INTEGER PRIMARY KEY,
        file_info_id INTEGER NOT NULL,
        checkpoint_idx INTEGER NOT NULL,
        total_events INTEGER NOT NULL DEFAULT 0,
        category_counts TEXT NOT NULL DEFAULT '{}',
        name_counts TEXT NOT NULL DEFAULT '{}',
        pid_tid_counts TEXT NOT NULL DEFAULT '{}',
        min_timestamp_us INTEGER,
        max_timestamp_us INTEGER,
        duration_sum_us INTEGER NOT NULL DEFAULT 0,
        duration_min_us INTEGER,
        duration_max_us INTEGER,
        duration_count INTEGER NOT NULL DEFAULT 0,
        duration_m2 REAL NOT NULL DEFAULT 0,
        duration_sketch BLOB,
        duration_histogram TEXT NOT NULL DEFAULT '[]',
        name_duration_sketches BLOB,
        name_duration_histograms TEXT NOT NULL DEFAULT '{}',
        name_duration_sums TEXT NOT NULL DEFAULT '{}',
        name_duration_sum_sqs TEXT NOT NULL DEFAULT '{}',
        name_category TEXT NOT NULL DEFAULT '{}',
        UNIQUE(file_info_id, checkpoint_idx)
    );

    CREATE TABLE IF NOT EXISTS index_dimensions (
        id INTEGER PRIMARY KEY,
        file_info_id INTEGER NOT NULL,
        dimension TEXT NOT NULL,
        UNIQUE(file_info_id, dimension)
    );

    CREATE TABLE IF NOT EXISTS hash_resolutions (
        id INTEGER PRIMARY KEY,
        file_info_id INTEGER NOT NULL,
        dimension TEXT NOT NULL,
        hash_value TEXT NOT NULL,
        resolved_value TEXT NOT NULL,
        UNIQUE(file_info_id, dimension, hash_value)
    );

    CREATE INDEX IF NOT EXISTS chunk_bloom_file_dim_idx
        ON chunk_bloom_filters(file_info_id, dimension);
    CREATE INDEX IF NOT EXISTS chunk_stats_file_idx
        ON chunk_statistics(file_info_id, checkpoint_idx);
    CREATE INDEX IF NOT EXISTS hash_res_dim_val_idx
        ON hash_resolutions(dimension, resolved_value);
)";

static const char* MANIFEST_SCHEMA = R"(
    CREATE TABLE IF NOT EXISTS checkpoint_event_ranges (
        checkpoint_idx  INTEGER NOT NULL,
        file_info_id    INTEGER NOT NULL,
        cat             TEXT NOT NULL,
        name            TEXT NOT NULL,
        line_numbers    BLOB NOT NULL,
        event_count     INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (file_info_id, checkpoint_idx, cat, name)
    );

    CREATE TABLE IF NOT EXISTS checkpoint_metadata_lines (
        checkpoint_idx  INTEGER NOT NULL,
        file_info_id    INTEGER NOT NULL,
        meta_type       TEXT NOT NULL,
        line_numbers    BLOB NOT NULL,
        PRIMARY KEY (file_info_id, checkpoint_idx, meta_type)
    );

    CREATE INDEX IF NOT EXISTS idx_event_ranges_checkpoint
        ON checkpoint_event_ranges(file_info_id, checkpoint_idx);
    CREATE INDEX IF NOT EXISTS idx_metadata_checkpoint
        ON checkpoint_metadata_lines(file_info_id, checkpoint_idx);
)";

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

IndexDatabase::IndexDatabase(const std::string& idx_path) : db_(idx_path) {}

// ---------------------------------------------------------------------------
// Schema initialisation
// ---------------------------------------------------------------------------

static void exec_schema(sqlite3* db, const char* sql, const char* label) {
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string error = err_msg ? std::string(err_msg) : "unknown error";
        if (err_msg) sqlite3_free(err_msg);
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           std::string(label) + ": " + error);
    }
}

void IndexDatabase::init_base_schema() {
    exec_schema(db_.get(), BASE_SCHEMA, "init_base_schema");
}

void IndexDatabase::init_bloom_schema() {
    exec_schema(db_.get(), BLOOM_SCHEMA, "init_bloom_schema");
}

void IndexDatabase::init_manifest_schema() {
    exec_schema(db_.get(), MANIFEST_SCHEMA, "init_manifest_schema");
}

// ---------------------------------------------------------------------------
// Query helpers
// ---------------------------------------------------------------------------

// Returns true if the named table exists in the database.
static bool table_exists(sqlite3* db, const char* table_name) {
    SqliteStmt stmt(db,
                    "SELECT 1 FROM sqlite_master "
                    "WHERE type='table' AND name=?;");
    stmt.bind_text(1, table_name);
    return sqlite3_step(stmt) == SQLITE_ROW;
}

bool IndexDatabase::has_bloom_data(int file_id) const {
    if (!table_exists(db_.get(), "chunk_bloom_filters")) return false;
    SqliteStmt stmt(db_.get(),
                    "SELECT 1 FROM chunk_bloom_filters "
                    "WHERE file_info_id=? LIMIT 1;");
    stmt.bind_int(1, file_id);
    return sqlite3_step(stmt) == SQLITE_ROW;
}

bool IndexDatabase::has_manifest_data(int file_id) const {
    if (!table_exists(db_.get(), "checkpoint_event_ranges")) return false;
    SqliteStmt stmt(db_.get(),
                    "SELECT 1 FROM checkpoint_event_ranges "
                    "WHERE file_info_id=? LIMIT 1;");
    stmt.bind_int(1, file_id);
    return sqlite3_step(stmt) == SQLITE_ROW;
}

int IndexDatabase::get_or_create_file_info(const std::string& path,
                                           std::uint64_t file_hash) {
    {
        SqliteStmt stmt(db_.get(),
                        "SELECT id, hash FROM files WHERE logical_name=?;");
        stmt.bind_text(1, path);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int id = sqlite3_column_int(stmt, 0);
            auto stored =
                static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 1));
            if (stored == file_hash) return id;
            SqliteStmt del(db_.get(), "DELETE FROM files WHERE id=?;");
            del.bind_int(1, id);
            sqlite3_step(del);
        }
    }

    SqliteStmt stmt(
        db_.get(),
        "INSERT INTO files(logical_name, byte_size, mtime_unix, hash)"
        " VALUES(?, 0, 0, ?);");
    stmt.bind_text(1, path);
    stmt.bind_int64(2, static_cast<std::int64_t>(file_hash));
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to insert into files: " +
                               std::string(sqlite3_errmsg(db_.get())));
    }
    return static_cast<int>(sqlite3_last_insert_rowid(db_.get()));
}

int IndexDatabase::get_file_info_id(const std::string& path) const {
    SqliteStmt stmt(db_.get(), "SELECT id FROM files WHERE logical_name=?;");
    stmt.bind_text(1, path);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        return sqlite3_column_int(stmt, 0);
    }
    return -1;
}

void IndexDatabase::begin_transaction() {
    exec_schema(db_.get(), "BEGIN TRANSACTION;", "begin_transaction");
}

void IndexDatabase::commit_transaction() {
    exec_schema(db_.get(), "COMMIT;", "commit_transaction");
}

// ---------------------------------------------------------------------------
// Bloom insert operations
// ---------------------------------------------------------------------------

void IndexDatabase::insert_chunk_bloom_filter(
    int file_id, std::uint64_t checkpoint_idx, std::string_view dimension,
    std::span<const unsigned char> blob_data, std::uint64_t num_entries) {
    queries::insert_chunk_bloom_filter(
        db_, file_id, checkpoint_idx, dimension, blob_data.data(),
        static_cast<int>(blob_data.size()), num_entries);
}

void IndexDatabase::insert_chunk_bloom_filter(
    int file_id, std::uint64_t checkpoint_idx, std::string_view dimension,
    const void* blob_data, int blob_size, std::uint64_t num_entries) {
    queries::insert_chunk_bloom_filter(db_, file_id, checkpoint_idx, dimension,
                                       blob_data, blob_size, num_entries);
}

void IndexDatabase::insert_file_bloom_filter(
    int file_id, std::string_view dimension,
    std::span<const unsigned char> blob_data, std::uint64_t num_entries) {
    queries::insert_file_bloom_filter(db_, file_id, dimension, blob_data.data(),
                                      static_cast<int>(blob_data.size()),
                                      num_entries);
}

void IndexDatabase::insert_file_bloom_filter(int file_id,
                                             std::string_view dimension,
                                             const void* blob_data,
                                             int blob_size,
                                             std::uint64_t num_entries) {
    queries::insert_file_bloom_filter(db_, file_id, dimension, blob_data,
                                      blob_size, num_entries);
}

void IndexDatabase::insert_chunk_statistics(int file_id,
                                            std::uint64_t checkpoint_idx,
                                            const ChunkStatistics& stats) {
    queries::insert_chunk_statistics(db_, file_id, checkpoint_idx, stats);
}

void IndexDatabase::insert_index_dimension(int file_id,
                                           std::string_view dimension) {
    queries::insert_index_dimension(db_, file_id, dimension);
}

void IndexDatabase::insert_hash_resolution(int file_id,
                                           std::string_view dimension,
                                           std::string_view hash_value,
                                           std::string_view resolved_value) {
    queries::insert_hash_resolution(db_, file_id, dimension, hash_value,
                                    resolved_value);
}

// ---------------------------------------------------------------------------
// Bloom query operations
// ---------------------------------------------------------------------------

std::vector<IndexDatabase::ChunkBloomResult>
IndexDatabase::query_chunk_bloom_filters(int file_id,
                                         std::string_view dimension) const {
    return queries::query_chunk_bloom_filters(db_, file_id, dimension);
}

std::unordered_map<std::string, std::vector<IndexDatabase::ChunkBloomResult>>
IndexDatabase::query_chunk_bloom_filters_batch(
    int file_id, const std::vector<std::string>& dimensions) const {
    return queries::query_chunk_bloom_filters_batch(db_, file_id, dimensions);
}

std::optional<IndexDatabase::FileBloomResult>
IndexDatabase::query_file_bloom_filter(int file_id,
                                       std::string_view dimension) const {
    return queries::query_file_bloom_filter(db_, file_id, dimension);
}

std::unordered_map<std::string, IndexDatabase::FileBloomResult>
IndexDatabase::query_file_bloom_filters_batch(
    int file_id, const std::vector<std::string>& dimensions) const {
    return queries::query_file_bloom_filters_batch(db_, file_id, dimensions);
}

std::vector<std::string> IndexDatabase::query_index_dimensions(
    int file_id) const {
    return queries::query_index_dimensions(db_, file_id);
}

bool IndexDatabase::has_index_dimension(int file_id,
                                        std::string_view dimension) const {
    return queries::has_index_dimension(db_, file_id, dimension);
}

std::vector<IndexDatabase::ChunkStatisticsResult>
IndexDatabase::query_chunk_statistics(int file_id) const {
    return queries::query_chunk_statistics(db_, file_id);
}

IndexDatabase::TimeBounds IndexDatabase::query_time_bounds(int file_id) const {
    return queries::query_time_bounds(db_, file_id);
}

std::optional<std::string> IndexDatabase::query_resolved_by_hash(
    std::string_view dimension, std::string_view hash_value) const {
    return queries::query_resolved_by_hash(db_, dimension, hash_value);
}

std::vector<std::string> IndexDatabase::query_hash_by_resolved(
    std::string_view dimension, std::string_view resolved_value) const {
    return queries::query_hash_by_resolved(db_, dimension, resolved_value);
}

// ---------------------------------------------------------------------------
// Bloom delete operations
// ---------------------------------------------------------------------------

void IndexDatabase::delete_chunk_bloom_filters(int file_id,
                                               std::string_view dimension) {
    queries::delete_chunk_bloom_filters(db_, file_id, dimension);
}

void IndexDatabase::delete_file_bloom_filter(int file_id,
                                             std::string_view dimension) {
    queries::delete_file_bloom_filter(db_, file_id, dimension);
}

void IndexDatabase::delete_chunk_statistics(int file_id) {
    queries::delete_chunk_statistics(db_, file_id);
}

void IndexDatabase::delete_hash_resolutions(int file_id) {
    queries::delete_hash_resolutions(db_, file_id);
}

// ---------------------------------------------------------------------------
// Manifest insert operations
// ---------------------------------------------------------------------------

void IndexDatabase::insert_event_range(
    int file_id, std::uint64_t checkpoint_idx, std::string_view cat,
    std::string_view name, std::span<const std::uint32_t> line_numbers) {
    queries::insert_event_range(db_, file_id, checkpoint_idx, cat, name,
                                line_numbers);
}

void IndexDatabase::insert_event_range(
    int file_id, std::uint64_t checkpoint_idx, std::string_view cat,
    std::string_view name, const std::vector<std::uint32_t>& line_numbers) {
    queries::insert_event_range(db_, file_id, checkpoint_idx, cat, name,
                                line_numbers);
}

void IndexDatabase::insert_metadata_lines(
    int file_id, std::uint64_t checkpoint_idx, std::string_view meta_type,
    std::span<const std::uint32_t> line_numbers) {
    queries::insert_metadata_lines(db_, file_id, checkpoint_idx, meta_type,
                                   line_numbers);
}

void IndexDatabase::insert_metadata_lines(
    int file_id, std::uint64_t checkpoint_idx, std::string_view meta_type,
    const std::vector<std::uint32_t>& line_numbers) {
    queries::insert_metadata_lines(db_, file_id, checkpoint_idx, meta_type,
                                   line_numbers);
}

// ---------------------------------------------------------------------------
// Manifest query operations
// ---------------------------------------------------------------------------

std::vector<IndexDatabase::EventRangeResult> IndexDatabase::query_event_ranges(
    int file_id) const {
    return queries::query_event_ranges(db_, file_id);
}

std::vector<IndexDatabase::EventRangeResult>
IndexDatabase::query_event_ranges_for_checkpoint(
    int file_id, std::uint64_t checkpoint_idx) const {
    return queries::query_event_ranges_for_checkpoint(db_, file_id,
                                                      checkpoint_idx);
}

std::vector<IndexDatabase::MetadataLinesResult>
IndexDatabase::query_metadata_lines(int file_id) const {
    return queries::query_metadata_lines(db_, file_id);
}

std::vector<IndexDatabase::MetadataLinesResult>
IndexDatabase::query_metadata_lines_for_checkpoint(
    int file_id, std::uint64_t checkpoint_idx) const {
    return queries::query_metadata_lines_for_checkpoint(db_, file_id,
                                                        checkpoint_idx);
}

// ---------------------------------------------------------------------------
// Manifest delete operations
// ---------------------------------------------------------------------------

void IndexDatabase::delete_event_ranges(int file_id) {
    queries::delete_event_ranges(db_, file_id);
}

void IndexDatabase::delete_metadata_lines(int file_id) {
    queries::delete_metadata_lines(db_, file_id);
}

}  // namespace dftracer::utils::utilities::indexer
