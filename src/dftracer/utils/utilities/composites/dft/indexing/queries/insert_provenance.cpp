#include <dftracer/utils/core/sqlite/statement.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/manifest_queries.h>
#include <dftracer/utils/utilities/indexer/internal/error.h>

namespace dftracer::utils::utilities::composites::dft::indexing::queries {

using dftracer::utils::sqlite::SqliteStmt;
using indexer::internal::IndexerError;

void insert_provenance_info(const SqliteDatabase& db, const std::string& key,
                            const std::string& value) {
    SqliteStmt stmt(db,
                    "INSERT OR REPLACE INTO provenance_info(key, value) "
                    "VALUES(?, ?);");

    stmt.bind_text(1, key);
    stmt.bind_text(2, value);

    int result = sqlite3_step(stmt);
    if (result != SQLITE_DONE) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to insert provenance info: " +
                               std::string(sqlite3_errmsg(db.get())));
    }
}

void insert_provenance_source(const SqliteDatabase& db, int file_info_id,
                              int source_idx, const std::string& path,
                              int num_checkpoints,
                              const std::string& event_hash) {
    SqliteStmt stmt(db,
                    "INSERT OR REPLACE INTO provenance_sources"
                    "(source_idx, file_info_id, path, "
                    "num_checkpoints, event_hash) "
                    "VALUES(?, ?, ?, ?, ?);");

    stmt.bind_int(1, source_idx);
    stmt.bind_int(2, file_info_id);
    stmt.bind_text(3, path);
    stmt.bind_int(4, num_checkpoints);
    stmt.bind_text(5, event_hash);

    int result = sqlite3_step(stmt);
    if (result != SQLITE_DONE) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to insert provenance source: " +
                               std::string(sqlite3_errmsg(db.get())));
    }
}

void insert_provenance_group(const SqliteDatabase& db, const std::string& name,
                             const std::string& predicate) {
    SqliteStmt stmt(db,
                    "INSERT INTO provenance_group(name, predicate) "
                    "VALUES(?, ?);");

    stmt.bind_text(1, name);
    stmt.bind_text(2, predicate);

    int result = sqlite3_step(stmt);
    if (result != SQLITE_DONE) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to insert provenance group: " +
                               std::string(sqlite3_errmsg(db.get())));
    }
}

void insert_provenance_segment(const SqliteDatabase& db, int source_idx,
                               int source_checkpoint, int output_line_start,
                               int output_line_end, int event_count) {
    SqliteStmt stmt(db,
                    "INSERT INTO provenance_segments"
                    "(source_idx, source_checkpoint, "
                    "output_line_start, output_line_end, "
                    "event_count) "
                    "VALUES(?, ?, ?, ?, ?);");

    stmt.bind_int(1, source_idx);
    stmt.bind_int(2, source_checkpoint);
    stmt.bind_int(3, output_line_start);
    stmt.bind_int(4, output_line_end);
    stmt.bind_int(5, event_count);

    int result = sqlite3_step(stmt);
    if (result != SQLITE_DONE) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to insert provenance segment: " +
                               std::string(sqlite3_errmsg(db.get())));
    }
}

}  // namespace
   // dftracer::utils::utilities::composites::dft::indexing::queries
