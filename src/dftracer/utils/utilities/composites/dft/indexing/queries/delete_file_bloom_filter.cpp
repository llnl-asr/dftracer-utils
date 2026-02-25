#include <dftracer/utils/core/sqlite/statement.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/indexer/internal/error.h>

namespace dftracer::utils::utilities::composites::dft::indexing::queries {

using dftracer::utils::sqlite::SqliteStmt;
using indexer::internal::IndexerError;

void delete_file_bloom_filter(const SqliteDatabase& db, int file_info_id,
                              const std::string& dimension) {
    SqliteStmt stmt(db,
                    "DELETE FROM file_bloom_filters "
                    "WHERE file_info_id = ? AND dimension = ?;");

    stmt.bind_int(1, file_info_id);
    stmt.bind_text(2, dimension);

    int result = sqlite3_step(stmt);
    if (result != SQLITE_DONE) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to delete file bloom filter: " +
                               std::string(sqlite3_errmsg(db.get())));
    }
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing::queries
