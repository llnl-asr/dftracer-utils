#include <dftracer/utils/core/sqlite/statement.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/indexer/internal/error.h>

#include <cstring>
#include <string_view>

namespace dftracer::utils::utilities::composites::dft::indexing::queries {

using dftracer::utils::sqlite::SqliteStmt;

std::vector<ChunkBloomResult> query_chunk_bloom_filters(
    const SqliteDatabase& db, int file_info_id, std::string_view dimension) {
    SqliteStmt stmt(db,
                    "SELECT checkpoint_idx, bloom_data, num_entries "
                    "FROM chunk_bloom_filters "
                    "WHERE file_info_id = ? AND dimension = ? "
                    "ORDER BY checkpoint_idx;");

    stmt.bind_int(1, file_info_id);
    stmt.bind_text(2, dimension);

    std::vector<ChunkBloomResult> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ChunkBloomResult r;
        r.checkpoint_idx =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));

        const void* blob = sqlite3_column_blob(stmt, 1);
        int blob_size = sqlite3_column_bytes(stmt, 1);
        if (blob && blob_size > 0) {
            r.bloom_data.resize(static_cast<std::size_t>(blob_size));
            std::memcpy(r.bloom_data.data(), blob,
                        static_cast<std::size_t>(blob_size));
        }

        r.num_entries =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 2));
        results.push_back(std::move(r));
    }

    return results;
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing::queries
