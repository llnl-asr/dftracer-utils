#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/sqlite/statement.h>
#include <dftracer/utils/utilities/indexer/internal/gzip/queries/queries.h>

namespace dftracer::utils::utilities::indexer::internal::gzip {

std::uint64_t query_max_bytes(const SqliteDatabase &db,
                              const std::string &gz_path_logical_path) {
    // Primary: metadata table has the authoritative total uncompressed size
    SqliteStmt metadata_stmt(
        db,
        "SELECT total_uc_size FROM metadata WHERE file_id = "
        "(SELECT id FROM files WHERE logical_name = ? LIMIT 1)");
    metadata_stmt.bind_text(1, gz_path_logical_path);
    if (sqlite3_step(metadata_stmt) == SQLITE_ROW) {
        std::uint64_t total =
            static_cast<std::uint64_t>(sqlite3_column_int64(metadata_stmt, 0));
        if (total > 0) {
            return total;
        }
    }

    // Fallback: derive from checkpoints if metadata is missing
    SqliteStmt stmt(
        db,
        "SELECT MAX(uc_offset + uc_size) FROM checkpoints WHERE file_id = "
        "(SELECT id FROM files WHERE logical_name = ? LIMIT 1)");
    std::uint64_t max_bytes = 0;
    stmt.bind_text(1, gz_path_logical_path);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        max_bytes = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
    }

    return max_bytes;
}

}  // namespace dftracer::utils::utilities::indexer::internal::gzip
