#include <dftracer/utils/utilities/indexer/internal/sqlite/statement.h>
#include <dftracer/utils/utilities/indexer/internal/tar/queries/queries.h>

namespace dftracer::utils::utilities::indexer::internal::tar {

void insert_tar_file_record(const SqliteDatabase &db, int archive_id,
                            const InsertTarFileData &data) {
    SqliteStmt stmt(db,
                    "INSERT INTO tar_files(archive_id, file_name, file_size, "
                    "file_mtime, typeflag, data_offset, uncompressed_offset) "
                    "VALUES(?, ?, ?, ?, ?, ?, ?);");

    stmt.bind_int(1, archive_id);
    stmt.bind_text(2, data.file_name);
    stmt.bind_int64(3, static_cast<int64_t>(data.file_size));
    stmt.bind_int64(4, static_cast<int64_t>(data.file_mtime));
    stmt.bind_text(5, std::string(1, data.typeflag));
    stmt.bind_int64(6, static_cast<int64_t>(data.data_offset));
    stmt.bind_int64(7, static_cast<int64_t>(data.uncompressed_offset));

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Insert TAR file record failed: " +
                               std::string(sqlite3_errmsg(db.get())));
    }
}

}  // namespace dftracer::utils::utilities::indexer::internal::tar