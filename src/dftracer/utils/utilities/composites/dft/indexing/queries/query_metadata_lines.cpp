#include <dftracer/utils/core/sqlite/statement.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/manifest_queries.h>

namespace dftracer::utils::utilities::composites::dft::indexing::queries {

using dftracer::utils::sqlite::SqliteStmt;

std::vector<MetadataLinesResult> query_metadata_lines(const SqliteDatabase& db,
                                                      int file_info_id) {
    SqliteStmt stmt(db,
                    "SELECT checkpoint_idx, meta_type, line_numbers "
                    "FROM checkpoint_metadata_lines "
                    "WHERE file_info_id = ? "
                    "ORDER BY checkpoint_idx, meta_type;");
    stmt.bind_int(1, file_info_id);

    std::vector<MetadataLinesResult> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MetadataLinesResult r;
        r.checkpoint_idx =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
        r.meta_type =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));

        const auto* blob_data =
            static_cast<const unsigned char*>(sqlite3_column_blob(stmt, 2));
        int blob_size = sqlite3_column_bytes(stmt, 2);
        r.line_numbers =
            unpack_line_numbers(blob_data, static_cast<std::size_t>(blob_size));

        results.push_back(std::move(r));
    }
    return results;
}

std::vector<MetadataLinesResult> query_metadata_lines_for_checkpoint(
    const SqliteDatabase& db, int file_info_id, std::uint64_t checkpoint_idx) {
    SqliteStmt stmt(db,
                    "SELECT checkpoint_idx, meta_type, line_numbers "
                    "FROM checkpoint_metadata_lines "
                    "WHERE file_info_id = ? AND checkpoint_idx = ? "
                    "ORDER BY meta_type;");
    stmt.bind_int(1, file_info_id);
    stmt.bind_int64(2, static_cast<std::int64_t>(checkpoint_idx));

    std::vector<MetadataLinesResult> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MetadataLinesResult r;
        r.checkpoint_idx =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
        r.meta_type =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));

        const auto* blob_data =
            static_cast<const unsigned char*>(sqlite3_column_blob(stmt, 2));
        int blob_size = sqlite3_column_bytes(stmt, 2);
        r.line_numbers =
            unpack_line_numbers(blob_data, static_cast<std::size_t>(blob_size));

        results.push_back(std::move(r));
    }
    return results;
}

}  // namespace
   // dftracer::utils::utilities::composites::dft::indexing::queries
