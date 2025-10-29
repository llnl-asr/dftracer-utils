#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/indexer/gzip/queries/queries.h>
#include <dftracer/utils/indexer/sqlite/statement.h>

namespace dftracer::utils::gzip_indexer {

void insert_file_metadata_record(const SqliteDatabase &db, int file_id,
                                 std::size_t ckpt_size,
                                 std::uint64_t total_lines,
                                 std::uint64_t total_uc_size) {
    SqliteStmt stmt(db,
                    "INSERT INTO metadata(file_id, checkpoint_size, "
                    "total_lines, total_uc_size) VALUES(?, ?, ?, ?);");

    stmt.bind_int(1, file_id);
    stmt.bind_int64(2, static_cast<int64_t>(ckpt_size));
    stmt.bind_int64(3, static_cast<int64_t>(total_lines));
    stmt.bind_int64(4, static_cast<int64_t>(total_uc_size));

    int result = sqlite3_step(stmt);
    if (result != SQLITE_DONE) {
        throw IndexerError(
            IndexerError::Type::DATABASE_ERROR,
            "Insert failed: " + std::string(sqlite3_errmsg(db.get())));
    }
    DFTRACER_UTILS_LOG_DEBUG(
        "Successfully inserted metadata for file_id %d: "
        "checkpoint_size=%zu, "
        "total_lines=%llu, total_uc_size=%llu",
        file_id, ckpt_size, total_lines, total_uc_size);
}

}  // namespace dftracer::utils::gzip_indexer
