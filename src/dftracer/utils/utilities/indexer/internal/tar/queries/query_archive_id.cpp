#include <dftracer/utils/utilities/indexer/internal/sqlite/statement.h>
#include <dftracer/utils/utilities/indexer/internal/tar/queries/queries.h>

namespace dftracer::utils::utilities::indexer::internal::tar {

int query_archive_id(const SqliteDatabase &db,
                     const std::string &tar_gz_path_logical_path) {
    SqliteStmt stmt(db,
                    "SELECT ta.id "
                    "FROM tar_archives ta "
                    "JOIN files f ON ta.file_id = f.id "
                    "WHERE f.logical_name = ?;");

    stmt.bind_text(1, tar_gz_path_logical_path);

    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        return sqlite3_column_int(stmt, 0);
    } else if (rc == SQLITE_DONE) {
        return -1;  // Not found
    } else {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Query archive ID failed: " +
                               std::string(sqlite3_errmsg(db.get())));
    }
}

}  // namespace dftracer::utils::utilities::indexer::internal::tar