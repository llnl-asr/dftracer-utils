#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/sqlite/statement.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/manifest_queries.h>
#include <dftracer/utils/utilities/indexer/internal/error.h>
#include <dftracer/utils/utilities/indexer/provenance_database.h>

namespace dftracer::utils::utilities::indexer {

namespace queries = composites::dft::indexing::queries;

using dftracer::utils::sqlite::SqliteStmt;
using internal::IndexerError;

static const char* PROVENANCE_SCHEMA = R"(
    PRAGMA journal_mode=WAL;
    PRAGMA busy_timeout=5000;
    PRAGMA foreign_keys=ON;

    CREATE TABLE IF NOT EXISTS file_info (
        id      INTEGER PRIMARY KEY,
        path    TEXT NOT NULL,
        hash    INTEGER
    );

    CREATE TABLE IF NOT EXISTS provenance_info (
        key     TEXT PRIMARY KEY,
        value   TEXT
    );

    CREATE TABLE IF NOT EXISTS provenance_sources (
        source_idx      INTEGER PRIMARY KEY,
        file_info_id    INTEGER NOT NULL DEFAULT 0,
        path            TEXT NOT NULL,
        num_checkpoints INTEGER,
        event_hash      TEXT NOT NULL DEFAULT ''
    );

    CREATE TABLE IF NOT EXISTS provenance_group (
        id          INTEGER PRIMARY KEY,
        name        TEXT,
        predicate   TEXT
    );

    CREATE TABLE IF NOT EXISTS provenance_segments (
        source_idx          INTEGER,
        source_checkpoint   INTEGER,
        output_line_start   INTEGER,
        output_line_end     INTEGER,
        event_count         INTEGER
    );
)";

ProvenanceDatabase::ProvenanceDatabase(const std::string& pidx_path)
    : db_(pidx_path) {}

void ProvenanceDatabase::init_schema() {
    char* err_msg = nullptr;
    int rc =
        sqlite3_exec(db_.get(), PROVENANCE_SCHEMA, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string error =
            err_msg ? std::string(err_msg) : "Unknown schema error";
        if (err_msg) sqlite3_free(err_msg);
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to initialize provenance schema: " + error);
    }
}

int ProvenanceDatabase::get_or_create_file_info(const std::string& path,
                                                std::uint64_t file_hash) {
    {
        SqliteStmt stmt(db_, "SELECT id, hash FROM file_info WHERE path = ?;");
        stmt.bind_text(1, path);
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            int id = sqlite3_column_int(stmt, 0);
            auto stored_hash =
                static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 1));
            if (stored_hash == file_hash) {
                return id;
            }
            SqliteStmt del(db_, "DELETE FROM file_info WHERE id = ?;");
            del.bind_int(1, id);
            sqlite3_step(del);
        }
    }

    SqliteStmt stmt(db_, "INSERT INTO file_info(path, hash) VALUES(?, ?);");
    stmt.bind_text(1, path);
    stmt.bind_int64(2, static_cast<std::int64_t>(file_hash));
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to insert file_info: " +
                               std::string(sqlite3_errmsg(db_.get())));
    }
    return static_cast<int>(sqlite3_last_insert_rowid(db_.get()));
}

int ProvenanceDatabase::get_file_info_id(const std::string& path) const {
    SqliteStmt stmt(db_, "SELECT id FROM file_info WHERE path = ?;");
    stmt.bind_text(1, path);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        return sqlite3_column_int(stmt, 0);
    }
    return -1;
}

void ProvenanceDatabase::begin_transaction() {
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

void ProvenanceDatabase::commit_transaction() {
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_.get(), "COMMIT;", nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string error = err_msg ? std::string(err_msg) : "Unknown error";
        if (err_msg) sqlite3_free(err_msg);
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to commit transaction: " + error);
    }
}

std::string determine_provenance_index_path(const std::string& data_path,
                                            const std::string& index_dir) {
    fs::path p(data_path);
    std::string filename = p.filename().string() + ".pidx";

    if (!index_dir.empty()) {
        return (fs::path(index_dir) / filename).string();
    }

    return (data_path + ".pidx");
}

// ---------------------------------------------------------------------------
// Provenance insert operations
// ---------------------------------------------------------------------------

void ProvenanceDatabase::insert_info(std::string_view key,
                                     std::string_view value) {
    queries::insert_provenance_info(db_, key, value);
}

void ProvenanceDatabase::insert_source(int file_info_id, int source_idx,
                                       std::string_view path,
                                       int num_checkpoints,
                                       std::string_view event_hash) {
    queries::insert_provenance_source(db_, file_info_id, source_idx, path,
                                      num_checkpoints, event_hash);
}

void ProvenanceDatabase::insert_group(std::string_view name,
                                      std::string_view predicate) {
    queries::insert_provenance_group(db_, name, predicate);
}

void ProvenanceDatabase::insert_segment(int source_idx, int source_checkpoint,
                                        int output_line_start,
                                        int output_line_end, int event_count) {
    queries::insert_provenance_segment(db_, source_idx, source_checkpoint,
                                       output_line_start, output_line_end,
                                       event_count);
}

// ---------------------------------------------------------------------------
// Provenance query operations
// ---------------------------------------------------------------------------

std::vector<ProvenanceDatabase::ProvenanceSource>
ProvenanceDatabase::query_sources(int file_info_id) const {
    return queries::query_provenance_sources(db_, file_info_id);
}

std::vector<ProvenanceDatabase::ProvenanceSegment>
ProvenanceDatabase::query_segments(int source_idx) const {
    return queries::query_provenance_segments(db_, source_idx);
}

std::vector<ProvenanceDatabase::ProvenanceSegment>
ProvenanceDatabase::query_all_segments() const {
    return queries::query_all_provenance_segments(db_);
}

std::string ProvenanceDatabase::query_info(std::string_view key) const {
    return queries::query_provenance_info(db_, key);
}

std::string ProvenanceDatabase::query_group_name() const {
    return queries::query_provenance_group_name(db_);
}

std::string ProvenanceDatabase::query_group_predicate() const {
    return queries::query_provenance_group_predicate(db_);
}

}  // namespace dftracer::utils::utilities::indexer
