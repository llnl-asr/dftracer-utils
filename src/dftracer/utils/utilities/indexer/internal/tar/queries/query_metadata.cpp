#include <dftracer/utils/utilities/indexer/internal/sqlite/statement.h>
#include <dftracer/utils/utilities/indexer/internal/tar/queries/queries.h>

namespace dftracer::utils::utilities::indexer::internal::tar {

std::uint64_t query_max_bytes(const SqliteDatabase &db,
                              const std::string &tar_gz_path_logical_path) {
    SqliteStmt stmt(db,
                    "SELECT m.total_uc_size "
                    "FROM metadata m "
                    "JOIN tar_archives ta ON m.archive_id = ta.id "
                    "JOIN files f ON ta.file_id = f.id "
                    "WHERE f.logical_name = ?;");

    stmt.bind_text(1, tar_gz_path_logical_path);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        return static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
    }

    return 0;
}

std::uint64_t query_num_lines(const SqliteDatabase &db,
                              const std::string &tar_gz_path_logical_path) {
    SqliteStmt stmt(db,
                    "SELECT m.total_lines "
                    "FROM metadata m "
                    "JOIN tar_archives ta ON m.archive_id = ta.id "
                    "JOIN files f ON ta.file_id = f.id "
                    "WHERE f.logical_name = ?;");

    stmt.bind_text(1, tar_gz_path_logical_path);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        return static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
    }

    return 0;
}

std::uint64_t query_num_files(const SqliteDatabase &db,
                              const std::string &tar_gz_path_logical_path) {
    SqliteStmt stmt(db,
                    "SELECT ta.total_files "
                    "FROM tar_archives ta "
                    "JOIN files f ON ta.file_id = f.id "
                    "WHERE f.logical_name = ?;");

    stmt.bind_text(1, tar_gz_path_logical_path);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        return static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
    }

    return 0;
}

std::string query_archive_name(const SqliteDatabase &db,
                               const std::string &tar_gz_path_logical_path) {
    SqliteStmt stmt(db,
                    "SELECT ta.archive_name "
                    "FROM tar_archives ta "
                    "JOIN files f ON ta.file_id = f.id "
                    "WHERE f.logical_name = ?;");

    stmt.bind_text(1, tar_gz_path_logical_path);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *name =
            reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
        return name ? std::string(name) : "";
    }

    return "";
}

std::uint64_t query_checkpoint_size(const SqliteDatabase &db, int archive_id) {
    SqliteStmt stmt(db,
                    "SELECT checkpoint_size "
                    "FROM metadata "
                    "WHERE archive_id = ?;");

    stmt.bind_int(1, archive_id);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        return static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
    }

    return 0;
}

bool query_stored_file_info(const SqliteDatabase &db,
                            const std::string &tar_gz_path,
                            std::uint64_t &stored_hash,
                            std::time_t &stored_mtime) {
    SqliteStmt stmt(db,
                    "SELECT hash, mtime_unix "
                    "FROM files "
                    "WHERE logical_name = ?;");

    stmt.bind_text(1, tar_gz_path);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        std::uint64_t hash =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
        if (hash == 0) {
            return false;  // No valid hash stored
        }
        stored_hash = hash;
        stored_mtime = static_cast<std::time_t>(sqlite3_column_int64(stmt, 1));
        return true;
    }

    return false;
}

bool query_schema_validity(const SqliteDatabase &db) {
    try {
        SqliteStmt stmt(db,
                        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                        "AND name IN ('files', 'tar_archives', 'tar_files', "
                        "'tar_gzip_checkpoints', 'metadata');");

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int table_count = sqlite3_column_int(stmt, 0);
            return table_count == 5;  // Should have all 5 tables
        }
    } catch (...) {
        return false;
    }
    return false;
}

bool delete_archive_record(const SqliteDatabase &db, int archive_id) {
    SqliteStmt stmt(db, "DELETE FROM tar_archives WHERE id = ?;");
    stmt.bind_int(1, archive_id);

    int rc = sqlite3_step(stmt);
    return rc == SQLITE_DONE;
}

}  // namespace dftracer::utils::utilities::indexer::internal::tar
