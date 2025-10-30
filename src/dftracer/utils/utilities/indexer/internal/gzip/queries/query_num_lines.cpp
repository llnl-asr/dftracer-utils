#include <dftracer/utils/utilities/indexer/internal/gzip/queries/queries.h>
#include <dftracer/utils/utilities/indexer/internal/sqlite/statement.h>

namespace dftracer::utils::utilities::indexer::internal::gzip {

std::uint64_t query_num_lines(const SqliteDatabase &db,
                              const std::string &gz_path_logical_path) {
    SqliteStmt stmt(db,
                    "SELECT total_lines FROM metadata WHERE file_id = "
                    "(SELECT id FROM files WHERE logical_name = ? LIMIT 1)");
    std::uint64_t total_lines = 0;

    stmt.bind_text(1, gz_path_logical_path);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        total_lines = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
    }

    return total_lines;
}

}  // namespace dftracer::utils::utilities::indexer::internal::gzip
