#include <dftracer/utils/core/sqlite/statement.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/indexer/internal/error.h>

#include <cstring>

namespace dftracer::utils::utilities::composites::dft::indexing::queries {

using dftracer::utils::sqlite::SqliteStmt;

std::unordered_map<std::string, FileBloomResult> query_file_bloom_filters_batch(
    const SqliteDatabase& db, int file_info_id,
    const std::vector<std::string>& dimensions) {
    std::unordered_map<std::string, FileBloomResult> results;
    if (dimensions.empty()) return results;

    std::string sql =
        "SELECT dimension, bloom_data, num_entries "
        "FROM file_bloom_filters "
        "WHERE file_info_id = ? AND dimension IN (";
    for (std::size_t i = 0; i < dimensions.size(); ++i) {
        if (i > 0) sql += ',';
        sql += '?';
    }
    sql += ");";

    SqliteStmt stmt(db, sql.c_str());
    stmt.bind_int(1, file_info_id);
    for (std::size_t i = 0; i < dimensions.size(); ++i) {
        stmt.bind_text(static_cast<int>(i + 2), dimensions[i]);
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* dim_text =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        std::string dim(dim_text ? dim_text : "");

        FileBloomResult r;
        const void* blob = sqlite3_column_blob(stmt, 1);
        int blob_size = sqlite3_column_bytes(stmt, 1);
        if (blob && blob_size > 0) {
            r.bloom_data.resize(static_cast<std::size_t>(blob_size));
            std::memcpy(r.bloom_data.data(), blob,
                        static_cast<std::size_t>(blob_size));
        }

        r.num_entries =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 2));
        results[dim] = std::move(r);
    }

    return results;
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing::queries
