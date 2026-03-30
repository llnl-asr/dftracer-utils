#include <dftracer/utils/core/sqlite/statement.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/indexer/internal/error.h>

#include <string_view>

namespace dftracer::utils::utilities::composites::dft::indexing::queries {

using dftracer::utils::sqlite::SqliteStmt;
using indexer::internal::IndexerError;

void insert_hash_resolution(const SqliteDatabase& db, int file_info_id,
                            std::string_view dimension,
                            std::string_view hash_value,
                            std::string_view resolved_value) {
    SqliteStmt stmt(db,
                    "INSERT OR IGNORE INTO hash_resolutions"
                    "(file_info_id, dimension, hash_value, resolved_value) "
                    "VALUES(?, ?, ?, ?);");

    stmt.bind_int(1, file_info_id);
    stmt.bind_text(2, dimension);
    stmt.bind_text(3, hash_value);
    stmt.bind_text(4, resolved_value);

    int result = sqlite3_step(stmt);
    if (result != SQLITE_DONE) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to insert hash resolution: " +
                               std::string(sqlite3_errmsg(db.get())));
    }
}

SqliteStmt prepare_insert_hash_resolution(const SqliteDatabase& db) {
    return SqliteStmt(db,
                      "INSERT OR IGNORE INTO hash_resolutions"
                      "(file_info_id, dimension, hash_value, resolved_value) "
                      "VALUES(?, ?, ?, ?);");
}

void insert_hash_resolution(SqliteStmt& stmt, int file_info_id,
                            std::string_view dimension,
                            std::string_view hash_value,
                            std::string_view resolved_value) {
    stmt.reset();
    stmt.bind_int(1, file_info_id);
    stmt.bind_text_static(2, dimension);
    stmt.bind_text_static(3, hash_value);
    stmt.bind_text_static(4, resolved_value);

    int result = sqlite3_step(stmt);
    if (result != SQLITE_DONE) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to insert hash resolution");
    }
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing::queries
