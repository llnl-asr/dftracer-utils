#include <dftracer/utils/utilities/indexer/internal/sqlite/statement.h>
#include <dftracer/utils/utilities/indexer/internal/tar/queries/queries.h>

namespace dftracer::utils::utilities::indexer::internal::tar {

std::vector<TarIndexer::TarFileInfo> query_tar_files(const SqliteDatabase &db,
                                                     int archive_id) {
    std::vector<TarIndexer::TarFileInfo> files;

    SqliteStmt stmt(db,
                    "SELECT file_name, file_size, file_mtime, typeflag, "
                    "data_offset, uncompressed_offset "
                    "FROM tar_files "
                    "WHERE archive_id = ? "
                    "ORDER BY uncompressed_offset;");

    stmt.bind_int(1, archive_id);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        TarIndexer::TarFileInfo file_info;
        file_info.file_name =
            reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
        file_info.file_size =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 1));
        file_info.file_mtime =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 2));

        const char *typeflag_str =
            reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
        file_info.typeflag = typeflag_str ? typeflag_str[0] : '0';

        file_info.data_offset =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 4));
        file_info.uncompressed_offset =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 5));

        files.push_back(file_info);
    }

    return files;
}

bool query_tar_file(const SqliteDatabase &db, int archive_id,
                    const std::string &file_name,
                    TarIndexer::TarFileInfo &file_info) {
    SqliteStmt stmt(db,
                    "SELECT file_name, file_size, file_mtime, typeflag, "
                    "data_offset, uncompressed_offset "
                    "FROM tar_files "
                    "WHERE archive_id = ? AND file_name = ?;");

    stmt.bind_int(1, archive_id);
    stmt.bind_text(2, file_name);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        file_info.file_name =
            reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
        file_info.file_size =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 1));
        file_info.file_mtime =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 2));

        const char *typeflag_str =
            reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
        file_info.typeflag = typeflag_str ? typeflag_str[0] : '0';

        file_info.data_offset =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 4));
        file_info.uncompressed_offset =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 5));

        return true;
    }

    return false;
}

std::vector<TarIndexer::TarFileInfo> query_tar_files_in_range(
    const SqliteDatabase &db, int archive_id, std::uint64_t start_offset,
    std::uint64_t end_offset) {
    std::vector<TarIndexer::TarFileInfo> files;

    SqliteStmt stmt(db,
                    "SELECT file_name, file_size, file_mtime, typeflag, "
                    "data_offset, uncompressed_offset "
                    "FROM tar_files "
                    "WHERE archive_id = ? AND uncompressed_offset >= ? AND "
                    "uncompressed_offset < ? "
                    "ORDER BY uncompressed_offset;");

    stmt.bind_int(1, archive_id);
    stmt.bind_int64(2, static_cast<int64_t>(start_offset));
    stmt.bind_int64(3, static_cast<int64_t>(end_offset));

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        TarIndexer::TarFileInfo file_info;
        file_info.file_name =
            reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
        file_info.file_size =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 1));
        file_info.file_mtime =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 2));

        const char *typeflag_str =
            reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
        file_info.typeflag = typeflag_str ? typeflag_str[0] : '0';

        file_info.data_offset =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 4));
        file_info.uncompressed_offset =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 5));

        files.push_back(file_info);
    }

    return files;
}

}  // namespace dftracer::utils::utilities::indexer::internal::tar