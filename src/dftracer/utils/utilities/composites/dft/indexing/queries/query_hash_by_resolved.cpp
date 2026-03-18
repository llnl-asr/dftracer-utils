#include <dftracer/utils/core/sqlite/statement.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>

#include <string_view>

namespace dftracer::utils::utilities::composites::dft::indexing::queries {

using dftracer::utils::sqlite::SqliteStmt;

std::vector<std::string> query_hash_by_resolved(
    const SqliteDatabase& db, std::string_view dimension,
    std::string_view resolved_value) {
    SqliteStmt stmt(db,
                    "SELECT DISTINCT hash_value FROM hash_resolutions "
                    "WHERE dimension = ? AND resolved_value = ?;");

    stmt.bind_text(1, dimension);
    stmt.bind_text(2, resolved_value);

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

}  // namespace dftracer::utils::utilities::composites::dft::indexing::queries
