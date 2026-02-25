#include <dftracer/utils/core/sqlite/statement.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/manifest_queries.h>

namespace dftracer::utils::utilities::composites::dft::indexing::queries {

using dftracer::utils::sqlite::SqliteStmt;

std::vector<EventRangeResult> query_event_ranges(const SqliteDatabase& db,
                                                 int file_info_id) {
    SqliteStmt stmt(db,
                    "SELECT checkpoint_idx, cat, name, line_numbers, "
                    "event_count "
                    "FROM checkpoint_event_ranges "
                    "WHERE file_info_id = ? "
                    "ORDER BY checkpoint_idx, cat, name;");
    stmt.bind_int(1, file_info_id);

    std::vector<EventRangeResult> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        EventRangeResult r;
        r.checkpoint_idx =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
        r.cat = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        r.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

        const auto* blob_data =
            static_cast<const unsigned char*>(sqlite3_column_blob(stmt, 3));
        int blob_size = sqlite3_column_bytes(stmt, 3);
        r.line_numbers =
            unpack_line_numbers(blob_data, static_cast<std::size_t>(blob_size));

        r.event_count =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 4));
        results.push_back(std::move(r));
    }
    return results;
}

std::vector<EventRangeResult> query_event_ranges_for_checkpoint(
    const SqliteDatabase& db, int file_info_id, std::uint64_t checkpoint_idx) {
    SqliteStmt stmt(db,
                    "SELECT checkpoint_idx, cat, name, line_numbers, "
                    "event_count "
                    "FROM checkpoint_event_ranges "
                    "WHERE file_info_id = ? AND checkpoint_idx = ? "
                    "ORDER BY cat, name;");
    stmt.bind_int(1, file_info_id);
    stmt.bind_int64(2, static_cast<std::int64_t>(checkpoint_idx));

    std::vector<EventRangeResult> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        EventRangeResult r;
        r.checkpoint_idx =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
        r.cat = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        r.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

        const auto* blob_data =
            static_cast<const unsigned char*>(sqlite3_column_blob(stmt, 3));
        int blob_size = sqlite3_column_bytes(stmt, 3);
        r.line_numbers =
            unpack_line_numbers(blob_data, static_cast<std::size_t>(blob_size));

        r.event_count =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 4));
        results.push_back(std::move(r));
    }
    return results;
}

}  // namespace
   // dftracer::utils::utilities::composites::dft::indexing::queries
