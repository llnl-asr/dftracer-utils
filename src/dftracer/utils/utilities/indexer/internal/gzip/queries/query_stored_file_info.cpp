#include <dftracer/utils/utilities/indexer/internal/gzip/queries/queries.h>
#include <dftracer/utils/utilities/indexer/internal/sqlite/statement.h>

namespace dftracer::utils::utilities::indexer::internal::gzip {

bool query_stored_file_info(const SqliteDatabase &db,
                            const std::string &gz_path,
                            std::uint64_t &stored_hash, time_t &stored_mtime) {
    SqliteStmt stmt(db,
                    "SELECT hash, mtime_unix FROM files WHERE "
                    "logical_name = ? LIMIT 1");

    stmt.bind_text(1, gz_path);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        std::uint64_t hash =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
        if (hash == 0) {
            return false;
        }
        stored_hash = hash;
        stored_mtime = static_cast<time_t>(sqlite3_column_int64(stmt, 1));
        return true;
    }

    return false;
}

}  // namespace dftracer::utils::utilities::indexer::internal::gzip
