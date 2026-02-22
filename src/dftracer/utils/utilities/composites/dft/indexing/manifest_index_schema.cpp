#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/dft/indexing/manifest_index_schema.h>
#include <dftracer/utils/utilities/indexer/internal/error.h>
#include <dftracer/utils/utilities/indexer/internal/sqlite/statement.h>

namespace dftracer::utils::utilities::composites::dft::indexing {

using indexer::internal::IndexerError;
using indexer::internal::SqliteStmt;

static const char* MANIFEST_INDEX_SCHEMA = R"(
    PRAGMA journal_mode=WAL;
    PRAGMA busy_timeout=5000;
    PRAGMA foreign_keys=ON;

    CREATE TABLE IF NOT EXISTS file_info (
        id INTEGER PRIMARY KEY,
        file_path TEXT UNIQUE NOT NULL,
        file_hash INTEGER NOT NULL,
        indexed_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
    );

    CREATE TABLE IF NOT EXISTS checkpoint_event_ranges (
        checkpoint_idx  INTEGER NOT NULL,
        file_info_id    INTEGER NOT NULL REFERENCES file_info(id) ON DELETE CASCADE,
        cat             TEXT NOT NULL,
        name            TEXT NOT NULL,
        line_numbers    BLOB NOT NULL,
        event_count     INTEGER NOT NULL,
        PRIMARY KEY (file_info_id, checkpoint_idx, cat, name)
    );

    CREATE TABLE IF NOT EXISTS checkpoint_metadata_lines (
        checkpoint_idx  INTEGER NOT NULL,
        file_info_id    INTEGER NOT NULL REFERENCES file_info(id) ON DELETE CASCADE,
        meta_type       TEXT NOT NULL,
        line_numbers    BLOB NOT NULL,
        PRIMARY KEY (file_info_id, checkpoint_idx, meta_type)
    );

    CREATE INDEX IF NOT EXISTS idx_event_ranges_checkpoint
        ON checkpoint_event_ranges(file_info_id, checkpoint_idx);
    CREATE INDEX IF NOT EXISTS idx_metadata_checkpoint
        ON checkpoint_metadata_lines(file_info_id, checkpoint_idx);

    CREATE TABLE IF NOT EXISTS provenance_info (
        key     TEXT PRIMARY KEY,
        value   TEXT NOT NULL
    );

    CREATE TABLE IF NOT EXISTS provenance_sources (
        source_idx      INTEGER PRIMARY KEY,
        file_info_id    INTEGER NOT NULL
            REFERENCES file_info(id) ON DELETE CASCADE,
        path            TEXT NOT NULL,
        num_checkpoints INTEGER NOT NULL,
        event_hash      TEXT NOT NULL
    );

    CREATE TABLE IF NOT EXISTS provenance_group (
        name        TEXT NOT NULL,
        predicate   TEXT
    );

    CREATE TABLE IF NOT EXISTS provenance_segments (
        segment_id          INTEGER PRIMARY KEY,
        source_idx          INTEGER NOT NULL
            REFERENCES provenance_sources(source_idx),
        source_checkpoint   INTEGER NOT NULL,
        output_line_start   INTEGER NOT NULL,
        output_line_end     INTEGER NOT NULL,
        event_count         INTEGER NOT NULL
    );

    CREATE INDEX IF NOT EXISTS idx_provenance_source
        ON provenance_segments(
            source_idx, source_checkpoint);
)";

ManifestIndexDatabase::ManifestIndexDatabase(const std::string& midx_path)
    : db_(midx_path) {}

void ManifestIndexDatabase::init_schema() {
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_.get(), MANIFEST_INDEX_SCHEMA, nullptr, nullptr,
                          &err_msg);
    if (rc != SQLITE_OK) {
        std::string error =
            err_msg ? std::string(err_msg) : "Unknown schema error";
        if (err_msg) sqlite3_free(err_msg);
        throw IndexerError(
            IndexerError::Type::DATABASE_ERROR,
            "Failed to initialize manifest index schema: " + error);
    }
}

int ManifestIndexDatabase::get_or_create_file_info(const std::string& file_path,
                                                   std::uint64_t file_hash) {
    // Try to find existing
    {
        SqliteStmt stmt(db_,
                        "SELECT id, file_hash FROM file_info "
                        "WHERE file_path = ?;");
        stmt.bind_text(1, file_path);
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            int id = sqlite3_column_int(stmt, 0);
            auto stored_hash =
                static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 1));
            if (stored_hash == file_hash) {
                return id;
            }
            // Hash changed -- delete old entry (cascade deletes
            // related data)
            SqliteStmt del(db_, "DELETE FROM file_info WHERE id = ?;");
            del.bind_int(1, id);
            sqlite3_step(del);
        }
    }

    // Insert new
    SqliteStmt stmt(db_,
                    "INSERT INTO file_info(file_path, file_hash) "
                    "VALUES(?, ?);");
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

int ManifestIndexDatabase::get_file_info_id(
    const std::string& file_path) const {
    SqliteStmt stmt(db_, "SELECT id FROM file_info WHERE file_path = ?;");
    stmt.bind_text(1, file_path);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        return sqlite3_column_int(stmt, 0);
    }
    return -1;
}

void ManifestIndexDatabase::begin_transaction() {
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

void ManifestIndexDatabase::commit_transaction() {
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_.get(), "COMMIT;", nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string error = err_msg ? std::string(err_msg) : "Unknown error";
        if (err_msg) sqlite3_free(err_msg);
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to commit transaction: " + error);
    }
}

std::string determine_manifest_index_path(const std::string& file_path,
                                          const std::string& index_dir) {
    fs::path data_path(file_path);
    std::string filename = data_path.filename().string() + ".midx";

    if (!index_dir.empty()) {
        fs::path dir(index_dir);
        return (dir / filename).string();
    }

    // Default: same directory as the data file
    return (data_path.parent_path() / filename).string();
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing
