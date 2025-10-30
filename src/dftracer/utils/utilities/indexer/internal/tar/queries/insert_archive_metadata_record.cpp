#include <dftracer/utils/utilities/indexer/internal/sqlite/statement.h>
#include <dftracer/utils/utilities/indexer/internal/tar/queries/queries.h>

namespace dftracer::utils::utilities::indexer::internal::tar {

void insert_archive_metadata_record(const SqliteDatabase &db, int archive_id,
                                    std::size_t ckpt_size,
                                    std::uint64_t total_lines,
                                    std::uint64_t total_uc_size) {
    SqliteStmt stmt(db,
                    "INSERT INTO metadata(archive_id, checkpoint_size, "
                    "total_lines, total_uc_size) "
                    "VALUES(?, ?, ?, ?) "
                    "ON CONFLICT(archive_id) DO UPDATE SET "
                    "checkpoint_size=excluded.checkpoint_size, "
                    "total_lines=excluded.total_lines, "
                    "total_uc_size=excluded.total_uc_size;");

    stmt.bind_int(1, archive_id);
    stmt.bind_int64(2, static_cast<int64_t>(ckpt_size));
    stmt.bind_int64(3, static_cast<int64_t>(total_lines));
    stmt.bind_int64(4, static_cast<int64_t>(total_uc_size));

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Insert archive metadata failed: " +
                               std::string(sqlite3_errmsg(db.get())));
    }
}

}  // namespace dftracer::utils::utilities::indexer::internal::tar