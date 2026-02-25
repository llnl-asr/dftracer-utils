#include <dftracer/utils/core/sqlite/statement.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>

namespace dftracer::utils::utilities::composites::dft::indexing::queries {

using dftracer::utils::sqlite::SqliteStmt;

std::vector<std::string> query_index_dimensions(const SqliteDatabase& db,
                                                int file_info_id) {
    SqliteStmt stmt(
        db, "SELECT dimension FROM index_dimensions WHERE file_info_id = ?;");

    stmt.bind_int(1, file_info_id);

    std::vector<std::string> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* text =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (text) {
            results.emplace_back(text);
        }
    }

    return results;
}

bool has_index_dimension(const SqliteDatabase& db, int file_info_id,
                         const std::string& dimension) {
    SqliteStmt stmt(db,
                    "SELECT 1 FROM index_dimensions "
                    "WHERE file_info_id = ? AND dimension = ? LIMIT 1;");

    stmt.bind_int(1, file_info_id);
    stmt.bind_text(2, dimension);

    return sqlite3_step(stmt) == SQLITE_ROW;
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing::queries
