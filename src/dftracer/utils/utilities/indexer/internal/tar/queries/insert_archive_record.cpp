#include <dftracer/utils/utilities/indexer/internal/sqlite/statement.h>
#include <dftracer/utils/utilities/indexer/internal/tar/queries/queries.h>

namespace dftracer::utils::utilities::indexer::internal::tar {

void insert_archive_record(const SqliteDatabase &db, int file_id,
                           const std::string &archive_name,
                           std::uint64_t uncompressed_size,
                           std::uint64_t total_files, int &archive_id) {
    SqliteStmt stmt(db,
                    "INSERT INTO tar_archives(file_id, archive_name, "
                    "uncompressed_size, total_files) "
                    "VALUES(?, ?, ?, ?) "
                    "ON CONFLICT(file_id) DO UPDATE SET "
                    "archive_name=excluded.archive_name, "
                    "uncompressed_size=excluded.uncompressed_size, "
                    "total_files=excluded.total_files "
                    "RETURNING id;");

    stmt.bind_int(1, file_id);
    stmt.bind_text(2, archive_name);
    stmt.bind_int64(3, static_cast<int64_t>(uncompressed_size));
    stmt.bind_int64(4, static_cast<int64_t>(total_files));

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        throw IndexerError(
            IndexerError::Type::DATABASE_ERROR,
            "Insert archive failed: " + std::string(sqlite3_errmsg(db.get())));
    }

    archive_id = sqlite3_column_int(stmt, 0);
}

}  // namespace dftracer::utils::utilities::indexer::internal::tar