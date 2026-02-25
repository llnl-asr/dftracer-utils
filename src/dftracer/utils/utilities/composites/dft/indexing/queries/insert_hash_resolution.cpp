#include <dftracer/utils/core/sqlite/statement.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/indexer/internal/error.h>

namespace dftracer::utils::utilities::composites::dft::indexing::queries {

using dftracer::utils::sqlite::SqliteStmt;
using indexer::internal::IndexerError;

void insert_hash_resolution(const SqliteDatabase& db, int file_info_id,
                            const std::string& dimension,
                            const std::string& hash_value,
                            const std::string& resolved_value) {
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

}  // namespace dftracer::utils::utilities::composites::dft::indexing::queries
