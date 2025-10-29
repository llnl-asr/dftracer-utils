#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/indexer/gzip/queries/queries.h>
#include <dftracer/utils/indexer/sqlite/statement.h>

namespace dftracer::utils::gzip_indexer {

std::uint64_t query_max_bytes(const SqliteDatabase &db,
                              const std::string &gz_path_logical_path) {
    SqliteStmt stmt(
        db,
        "SELECT MAX(uc_offset + uc_size) FROM checkpoints WHERE file_id = "
        "(SELECT id FROM files WHERE logical_name = ? LIMIT 1)");
    std::uint64_t max_bytes = 0;
    stmt.bind_text(1, gz_path_logical_path);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        max_bytes = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
    }

    // If no checkpoints exist (max_bytes is 0), fall back to metadata table
    if (max_bytes == 0) {
        SqliteStmt metadata_stmt(
            db,
            "SELECT total_uc_size FROM metadata WHERE file_id = "
            "(SELECT id FROM files WHERE logical_name = ? LIMIT 1)");
        metadata_stmt.bind_text(1, gz_path_logical_path);
        if (sqlite3_step(metadata_stmt) == SQLITE_ROW) {
            max_bytes =
                static_cast<uint64_t>(sqlite3_column_int64(metadata_stmt, 0));
            DFTRACER_UTILS_LOG_DEBUG(
                "No checkpoints found, using metadata total_uc_size: %llu",
                max_bytes);
        }
    }

    return max_bytes;
}

}  // namespace dftracer::utils::gzip_indexer
