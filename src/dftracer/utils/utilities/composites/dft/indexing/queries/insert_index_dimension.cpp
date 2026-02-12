#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/indexer/internal/error.h>
#include <dftracer/utils/utilities/indexer/internal/sqlite/statement.h>

namespace dftracer::utils::utilities::composites::dft::indexing::queries {

using indexer::internal::IndexerError;
using indexer::internal::SqliteStmt;

void insert_index_dimension(const SqliteDatabase& db, int file_info_id,
                            const std::string& dimension) {
    SqliteStmt stmt(db,
                    "INSERT OR IGNORE INTO index_dimensions"
                    "(file_info_id, dimension) VALUES(?, ?);");

    stmt.bind_int(1, file_info_id);
    stmt.bind_text(2, dimension);

    int result = sqlite3_step(stmt);
    if (result != SQLITE_DONE) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to insert index dimension: " +
                               std::string(sqlite3_errmsg(db.get())));
    }
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing::queries
