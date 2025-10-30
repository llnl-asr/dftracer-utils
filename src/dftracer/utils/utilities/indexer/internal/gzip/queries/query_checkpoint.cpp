#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/indexer/internal/gzip/queries/queries.h>
#include <dftracer/utils/utilities/indexer/internal/sqlite/statement.h>

#include <cstring>

namespace dftracer::utils::utilities::indexer::internal::gzip {

bool query_checkpoint(const SqliteDatabase& db, std::size_t target_offset,
                      int file_id, IndexerCheckpoint& checkpoint) {
    DFTRACER_UTILS_LOG_DEBUG(
        "query_checkpoint called: target_offset=%zu, file_id=%d", target_offset,
        file_id);

    // For target offset 0, always decompress from beginning of file (no
    // checkpoint)
    if (target_offset == 0) {
        DFTRACER_UTILS_LOG_DEBUG(
            "%s", "query_checkpoint: target_offset is 0, returning false");
        return false;
    }

    if (file_id == -1) {
        DFTRACER_UTILS_LOG_DEBUG(
            "%s", "query_checkpoint: file_id is -1, returning false");
        return false;
    }

    SqliteStmt stmt(
        db,
        "SELECT checkpoint_idx, uc_offset, uc_size, c_offset, c_size, bits, "
        "dict_compressed, num_lines "
        "FROM checkpoints WHERE file_id = ? AND uc_offset <= ? "
        "ORDER BY uc_offset DESC LIMIT 1");
    bool found = false;

    stmt.bind_int(1, file_id);
    stmt.bind_int64(2, static_cast<int64_t>(target_offset));

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        checkpoint.checkpoint_idx =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
        checkpoint.uc_offset =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 1));
        checkpoint.uc_size =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 2));
        checkpoint.c_offset =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 3));
        checkpoint.c_size =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 4));
        checkpoint.bits = sqlite3_column_int(stmt, 5);
        std::size_t dict_size =
            static_cast<std::size_t>(sqlite3_column_bytes(stmt, 6));
        checkpoint.dict_compressed.resize(dict_size);
        std::memcpy(checkpoint.dict_compressed.data(),
                    sqlite3_column_blob(stmt, 6), dict_size);
        checkpoint.num_lines =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 7));
        found = true;

        DFTRACER_UTILS_LOG_DEBUG(
            "query_checkpoint: found checkpoint idx=%llu, uc_offset=%llu, "
            "c_offset=%llu, bits=%d",
            checkpoint.checkpoint_idx, checkpoint.uc_offset,
            checkpoint.c_offset, checkpoint.bits);
    } else {
        DFTRACER_UTILS_LOG_DEBUG(
            "query_checkpoint: no checkpoint found for target_offset=%zu, "
            "file_id=%d",
            target_offset, file_id);
    }

    return found;
}

}  // namespace dftracer::utils::utilities::indexer::internal::gzip
