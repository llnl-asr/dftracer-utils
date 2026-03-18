#include <dftracer/utils/core/sqlite/statement.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/indexer/internal/error.h>

#include <cstring>
#include <string_view>

namespace dftracer::utils::utilities::composites::dft::indexing::queries {

using dftracer::utils::sqlite::SqliteStmt;

std::optional<FileBloomResult> query_file_bloom_filter(
    const SqliteDatabase& db, int file_info_id, std::string_view dimension) {
    SqliteStmt stmt(db,
                    "SELECT bloom_data, num_entries "
                    "FROM file_bloom_filters "
                    "WHERE file_info_id = ? AND dimension = ?;");

    stmt.bind_int(1, file_info_id);
    stmt.bind_text(2, dimension);

    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        FileBloomResult r;
        const void* blob = sqlite3_column_blob(stmt, 0);
        int blob_size = sqlite3_column_bytes(stmt, 0);
        if (blob && blob_size > 0) {
            r.bloom_data.resize(static_cast<std::size_t>(blob_size));
            std::memcpy(r.bloom_data.data(), blob,
                        static_cast<std::size_t>(blob_size));
        }
        r.num_entries =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 1));
        return r;
    }

    return std::nullopt;
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing::queries
