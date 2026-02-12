#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/indexer/internal/error.h>
#include <dftracer/utils/utilities/indexer/internal/sqlite/statement.h>

namespace dftracer::utils::utilities::composites::dft::indexing::queries {

using indexer::internal::IndexerError;
using indexer::internal::SqliteStmt;

void delete_chunk_statistics(const SqliteDatabase& db, int file_info_id) {
    SqliteStmt stmt(db, "DELETE FROM chunk_statistics WHERE file_info_id = ?;");

    stmt.bind_int(1, file_info_id);

    int result = sqlite3_step(stmt);
    if (result != SQLITE_DONE) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to delete chunk statistics: " +
                               std::string(sqlite3_errmsg(db.get())));
    }
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing::queries
