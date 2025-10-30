#include <dftracer/utils/utilities/indexer/internal/gzip/queries/queries.h>
#include <dftracer/utils/utilities/indexer/internal/sqlite/statement.h>

namespace dftracer::utils::utilities::indexer::internal::gzip {

int query_file_id(const SqliteDatabase &db,
                  const std::string &gz_path_logical_path) {
    SqliteStmt stmt(db, "SELECT id FROM files WHERE logical_name = ? LIMIT 1");
    int file_id = -1;

    stmt.bind_text(1, gz_path_logical_path);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        file_id = sqlite3_column_int(stmt, 0);
    }

    return file_id;
}

}  // namespace dftracer::utils::utilities::indexer::internal::gzip
