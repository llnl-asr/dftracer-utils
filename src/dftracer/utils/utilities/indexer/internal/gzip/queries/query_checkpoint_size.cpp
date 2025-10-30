#include <dftracer/utils/utilities/indexer/internal/gzip/queries/queries.h>
#include <dftracer/utils/utilities/indexer/internal/sqlite/statement.h>

namespace dftracer::utils::utilities::indexer::internal::gzip {

std::uint64_t query_checkpoint_size(const SqliteDatabase &db, int file_id) {
    SqliteStmt stmt(db,
                    "SELECT checkpoint_size FROM metadata WHERE file_id = ?");
    stmt.bind_int(1, file_id);
    std::uint64_t ckpt_size = 0;

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        ckpt_size = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
    }

    return ckpt_size;
}

}  // namespace dftracer::utils::utilities::indexer::internal::gzip
