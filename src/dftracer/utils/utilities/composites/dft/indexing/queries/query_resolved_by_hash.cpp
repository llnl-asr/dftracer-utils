#include <dftracer/utils/core/sqlite/statement.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>

#include <string_view>

namespace dftracer::utils::utilities::composites::dft::indexing::queries {

using dftracer::utils::sqlite::SqliteStmt;

std::optional<std::string> query_resolved_by_hash(const SqliteDatabase& db,
                                                  std::string_view dimension,
                                                  std::string_view hash_value) {
    SqliteStmt stmt(db,
                    "SELECT resolved_value FROM hash_resolutions "
                    "WHERE dimension = ? AND hash_value = ? LIMIT 1;");

    stmt.bind_text(1, dimension);
    stmt.bind_text(2, hash_value);

    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const char* text =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (text) {
            return std::string(text);
        }
    }

    return std::nullopt;
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing::queries
