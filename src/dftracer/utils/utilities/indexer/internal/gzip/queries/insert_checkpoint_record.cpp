#include <dftracer/utils/utilities/indexer/internal/gzip/queries/queries.h>
#include <dftracer/utils/utilities/indexer/internal/sqlite/statement.h>

namespace dftracer::utils::utilities::indexer::internal::gzip {

void insert_checkpoint_record(const SqliteDatabase &db, int file_id,
                              const InsertCheckpointData &data) {
    SqliteStmt stmt(
        db,
        "INSERT INTO checkpoints(file_id, checkpoint_idx, uc_offset, "
        "uc_size, c_offset, c_size, bits, dict_compressed, num_lines, "
        "first_line_num, last_line_num) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");

    stmt.bind_int(1, file_id);
    stmt.bind_int64(2, static_cast<std::int64_t>(data.idx));
    stmt.bind_int64(3, static_cast<std::int64_t>(data.uc_offset));
    stmt.bind_int64(4, static_cast<std::int64_t>(data.uc_size));
    stmt.bind_int64(5, static_cast<std::int64_t>(data.c_offset));
    stmt.bind_int64(6, static_cast<std::int64_t>(data.c_size));
    stmt.bind_int(7, data.bits);
    stmt.bind_blob(8, data.compressed_dict,
                   static_cast<int>(data.compressed_dict_size));
    stmt.bind_int64(9, static_cast<std::int64_t>(data.num_lines));
    stmt.bind_int64(10, static_cast<std::int64_t>(data.first_line_num));
    stmt.bind_int64(11, static_cast<std::int64_t>(data.last_line_num));

    int result = sqlite3_step(stmt);
    if (result != SQLITE_DONE) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to insert checkpoint: " +
                               std::string(sqlite3_errmsg(db.get())));
    }
}

}  // namespace dftracer::utils::utilities::indexer::internal::gzip
