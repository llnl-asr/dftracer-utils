#include <dftracer/utils/utilities/indexer/internal/gzip/queries/queries.h>
#include <dftracer/utils/utilities/indexer/internal/sqlite/statement.h>

#include <cstring>

namespace dftracer::utils::utilities::indexer::internal::gzip {

std::vector<IndexerCheckpoint> query_checkpoints(const SqliteDatabase &db,
                                                 int file_id) {
    std::vector<IndexerCheckpoint> checkpoints;

    SqliteStmt stmt(
        db,
        "SELECT checkpoint_idx, uc_offset, uc_size, c_offset, c_size, bits, "
        "dict_compressed, num_lines, first_line_num, last_line_num "
        "FROM checkpoints WHERE file_id = ? ORDER BY uc_offset");

    stmt.bind_int(1, file_id);

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

        std::size_t dict_size =
            static_cast<std::size_t>(sqlite3_column_bytes(stmt, 6));
        checkpoint.dict_compressed.resize(dict_size);
        std::memcpy(checkpoint.dict_compressed.data(),
                    sqlite3_column_blob(stmt, 6), dict_size);

        checkpoint.num_lines =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 7));
        checkpoint.first_line_num =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 8));
        checkpoint.last_line_num =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 9));

        checkpoints.push_back(std::move(checkpoint));
    }

    return checkpoints;
}

std::vector<IndexerCheckpoint> query_checkpoints_for_line_range(
    const SqliteDatabase &db, int file_id, std::uint64_t start_line,
    std::uint64_t end_line) {
    std::vector<IndexerCheckpoint> checkpoints;

    SqliteStmt stmt(
        db,
        "SELECT checkpoint_idx, uc_offset, uc_size, c_offset, c_size, bits, "
        "dict_compressed, num_lines, first_line_num, last_line_num "
        "FROM checkpoints WHERE file_id = ? AND "
        "(first_line_num <= ? AND last_line_num >= ?) OR "
        "(first_line_num <= ? AND last_line_num >= ?) "
        "ORDER BY uc_offset");

    stmt.bind_int(1, file_id);
    stmt.bind_int64(2, static_cast<int64_t>(end_line));
    stmt.bind_int64(3, static_cast<int64_t>(start_line));
    stmt.bind_int64(4, static_cast<int64_t>(start_line));
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

        size_t dict_size = static_cast<size_t>(sqlite3_column_bytes(stmt, 6));
        checkpoint.dict_compressed.resize(dict_size);
        std::memcpy(checkpoint.dict_compressed.data(),
                    sqlite3_column_blob(stmt, 6), dict_size);

        checkpoint.num_lines =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 7));
        checkpoint.first_line_num =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 8));
        checkpoint.last_line_num =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 9));

        checkpoints.push_back(std::move(checkpoint));
    }

    return checkpoints;
}

}  // namespace dftracer::utils::utilities::indexer::internal::gzip
