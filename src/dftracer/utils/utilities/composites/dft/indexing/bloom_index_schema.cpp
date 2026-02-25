#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/sqlite/statement.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_index_schema.h>
#include <dftracer/utils/utilities/indexer/internal/error.h>

namespace dftracer::utils::utilities::composites::dft::indexing {

using dftracer::utils::sqlite::SqliteStmt;
using indexer::internal::IndexerError;

static const char* BLOOM_INDEX_SCHEMA = R"(
    PRAGMA journal_mode=WAL;
    PRAGMA busy_timeout=5000;
    PRAGMA foreign_keys=ON;

    CREATE TABLE IF NOT EXISTS file_info (
        id INTEGER PRIMARY KEY,
        file_path TEXT UNIQUE NOT NULL,
        file_hash INTEGER NOT NULL,
        indexed_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
    );

    CREATE TABLE IF NOT EXISTS chunk_bloom_filters (
        id INTEGER PRIMARY KEY,
        file_info_id INTEGER NOT NULL REFERENCES file_info(id) ON DELETE CASCADE,
        checkpoint_idx INTEGER NOT NULL,
        dimension TEXT NOT NULL,
        bloom_data BLOB NOT NULL,
        num_entries INTEGER NOT NULL,
        UNIQUE(file_info_id, checkpoint_idx, dimension)
    );

    CREATE TABLE IF NOT EXISTS file_bloom_filters (
        id INTEGER PRIMARY KEY,
        file_info_id INTEGER NOT NULL REFERENCES file_info(id) ON DELETE CASCADE,
        dimension TEXT NOT NULL,
        bloom_data BLOB NOT NULL,
        num_entries INTEGER NOT NULL,
        UNIQUE(file_info_id, dimension)
    );

    CREATE TABLE IF NOT EXISTS chunk_statistics (
        id INTEGER PRIMARY KEY,
        file_info_id INTEGER NOT NULL REFERENCES file_info(id) ON DELETE CASCADE,
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
        UNIQUE(file_info_id, checkpoint_idx)
    );

    CREATE TABLE IF NOT EXISTS index_dimensions (
        id INTEGER PRIMARY KEY,
        file_info_id INTEGER NOT NULL REFERENCES file_info(id) ON DELETE CASCADE,
        dimension TEXT NOT NULL,
        built_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
        UNIQUE(file_info_id, dimension)
    );

    CREATE TABLE IF NOT EXISTS hash_resolutions (
        id INTEGER PRIMARY KEY,
        file_info_id INTEGER NOT NULL REFERENCES file_info(id) ON DELETE CASCADE,
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

BloomIndexDatabase::BloomIndexDatabase(const std::string& bidx_path)
    : db_(bidx_path) {}

void BloomIndexDatabase::init_schema() {
    char* err_msg = nullptr;
    int rc =
        sqlite3_exec(db_.get(), BLOOM_INDEX_SCHEMA, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string error =
            err_msg ? std::string(err_msg) : "Unknown schema error";
        if (err_msg) sqlite3_free(err_msg);
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to initialize bloom index schema: " + error);
    }
}

int BloomIndexDatabase::get_or_create_file_info(const std::string& file_path,
                                                std::uint64_t file_hash) {
    // Try to find existing
    {
        SqliteStmt stmt(
            db_, "SELECT id, file_hash FROM file_info WHERE file_path = ?;");
        stmt.bind_text(1, file_path);
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            int id = sqlite3_column_int(stmt, 0);
            auto stored_hash =
                static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 1));
            if (stored_hash == file_hash) {
                return id;
            }
            // Hash changed -- delete old entry (cascade deletes related data)
            SqliteStmt del(db_, "DELETE FROM file_info WHERE id = ?;");
            del.bind_int(1, id);
            sqlite3_step(del);
        }
    }

    // Insert new
    SqliteStmt stmt(
        db_, "INSERT INTO file_info(file_path, file_hash) VALUES(?, ?);");
    stmt.bind_text(1, file_path);
    stmt.bind_int64(2, static_cast<std::int64_t>(file_hash));
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to insert file_info: " +
                               std::string(sqlite3_errmsg(db_.get())));
    }
    return static_cast<int>(sqlite3_last_insert_rowid(db_.get()));
}

int BloomIndexDatabase::get_file_info_id(const std::string& file_path) const {
    SqliteStmt stmt(db_, "SELECT id FROM file_info WHERE file_path = ?;");
    stmt.bind_text(1, file_path);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        return sqlite3_column_int(stmt, 0);
    }
    return -1;
}

void BloomIndexDatabase::begin_transaction() {
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_.get(), "BEGIN TRANSACTION;", nullptr, nullptr,
                          &err_msg);
    if (rc != SQLITE_OK) {
        std::string error = err_msg ? std::string(err_msg) : "Unknown error";
        if (err_msg) sqlite3_free(err_msg);
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to begin transaction: " + error);
    }
}

void BloomIndexDatabase::commit_transaction() {
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_.get(), "COMMIT;", nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string error = err_msg ? std::string(err_msg) : "Unknown error";
        if (err_msg) sqlite3_free(err_msg);
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to commit transaction: " + error);
    }
}

std::string determine_bloom_index_path(const std::string& file_path,
                                       const std::string& index_dir) {
    fs::path data_path(file_path);
    std::string filename = data_path.filename().string() + ".bidx";

    if (!index_dir.empty()) {
        fs::path dir(index_dir);
        return (dir / filename).string();
    }

    // Default: same directory as the data file
    return (data_path.parent_path() / filename).string();
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing
