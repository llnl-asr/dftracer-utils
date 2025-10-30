#include <dftracer/utils/utilities/indexer/internal/checkpoint.h>
#include <dftracer/utils/utilities/indexer/internal/sqlite/statement.h>
#include <dftracer/utils/utilities/indexer/internal/tar/queries/queries.h>

#include <cstring>

namespace dftracer::utils::utilities::indexer::internal::tar {

bool query_tar_checkpoint(const SqliteDatabase &db, std::size_t target_offset,
                          int archive_id, IndexerCheckpoint &checkpoint) {
    SqliteStmt stmt(
        db,
        "SELECT checkpoint_idx, uc_offset, uc_size, c_offset, c_size, "
        "bits, dict_compressed, num_lines, first_line_num, last_line_num "
        "FROM tar_gzip_checkpoints "
        "WHERE archive_id = ? AND uc_offset <= ? "
        "ORDER BY uc_offset DESC "
        "LIMIT 1;");

    stmt.bind_int(1, archive_id);
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

        // Copy compressed dictionary
        const void *dict_data = sqlite3_column_blob(stmt, 6);
        int dict_size = sqlite3_column_bytes(stmt, 6);
        if (dict_data && dict_size > 0) {
            checkpoint.dict_compressed.resize(dict_size);
            std::memcpy(checkpoint.dict_compressed.data(), dict_data,
                        dict_size);
        }

        checkpoint.num_lines =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 7));
        checkpoint.first_line_num =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 8));
        checkpoint.last_line_num =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 9));

        return true;
    }

    return false;
}

std::vector<IndexerCheckpoint> query_tar_checkpoints(const SqliteDatabase &db,
                                                     int archive_id) {
    std::vector<IndexerCheckpoint> checkpoints;

    SqliteStmt stmt(
        db,
        "SELECT checkpoint_idx, uc_offset, uc_size, c_offset, c_size, "
        "bits, dict_compressed, num_lines, first_line_num, last_line_num "
        "FROM tar_gzip_checkpoints "
        "WHERE archive_id = ? "
        "ORDER BY checkpoint_idx;");

    stmt.bind_int(1, archive_id);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        IndexerCheckpoint checkpoint;
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

        // Copy compressed dictionary
        const void *dict_data = sqlite3_column_blob(stmt, 6);
        int dict_size = sqlite3_column_bytes(stmt, 6);
        if (dict_data && dict_size > 0) {
            checkpoint.dict_compressed.resize(dict_size);
            std::memcpy(checkpoint.dict_compressed.data(), dict_data,
                        dict_size);
        }

        checkpoint.num_lines =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 7));
        checkpoint.first_line_num =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 8));
        checkpoint.last_line_num =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 9));

        checkpoints.push_back(checkpoint);
    }

    return checkpoints;
}

std::vector<IndexerCheckpoint> query_tar_checkpoints_for_line_range(
    const SqliteDatabase &db, int archive_id, std::uint64_t start_line,
    std::uint64_t end_line) {
    std::vector<IndexerCheckpoint> checkpoints;

    SqliteStmt stmt(
        db,
        "SELECT checkpoint_idx, uc_offset, uc_size, c_offset, c_size, "
        "bits, dict_compressed, num_lines, first_line_num, last_line_num "
        "FROM tar_gzip_checkpoints "
        "WHERE archive_id = ? AND "
        "((first_line_num <= ? AND last_line_num >= ?) OR "
        " (first_line_num <= ? AND last_line_num >= ?)) "
        "ORDER BY checkpoint_idx;");

    stmt.bind_int(1, archive_id);
    stmt.bind_int64(2, static_cast<int64_t>(start_line));
    stmt.bind_int64(3, static_cast<int64_t>(start_line));
    stmt.bind_int64(4, static_cast<int64_t>(end_line));
    stmt.bind_int64(5, static_cast<int64_t>(end_line));

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        IndexerCheckpoint checkpoint;
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

        // Copy compressed dictionary
        const void *dict_data = sqlite3_column_blob(stmt, 6);
        int dict_size = sqlite3_column_bytes(stmt, 6);
        if (dict_data && dict_size > 0) {
            checkpoint.dict_compressed.resize(dict_size);
            std::memcpy(checkpoint.dict_compressed.data(), dict_data,
                        dict_size);
        }

        checkpoint.num_lines =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 7));
        checkpoint.first_line_num =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 8));
        checkpoint.last_line_num =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 9));

        checkpoints.push_back(checkpoint);
    }

    return checkpoints;
}

}  // namespace dftracer::utils::utilities::indexer::internal::tar