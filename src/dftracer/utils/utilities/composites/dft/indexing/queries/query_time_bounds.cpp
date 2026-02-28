#include <dftracer/utils/core/sqlite/statement.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>

#include <limits>

namespace dftracer::utils::utilities::composites::dft::indexing::queries {

using dftracer::utils::sqlite::SqliteStmt;

TimeBounds query_time_bounds(const SqliteDatabase& db, int file_info_id) {
    SqliteStmt stmt(db,
                    "SELECT MIN(min_timestamp_us), MAX(max_timestamp_us) "
                    "FROM chunk_statistics WHERE file_info_id = ? "
                    "AND min_timestamp_us IS NOT NULL "
                    "AND max_timestamp_us IS NOT NULL;");

    stmt.bind_int(1, file_info_id);

    TimeBounds result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            result.min_timestamp_us =
                static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
        }
        if (sqlite3_column_type(stmt, 1) != SQLITE_NULL) {
            result.max_timestamp_us =
                static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 1));
        }
        result.valid = (result.min_timestamp_us !=
                        std::numeric_limits<std::uint64_t>::max());
    }

    return result;
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing::queries
