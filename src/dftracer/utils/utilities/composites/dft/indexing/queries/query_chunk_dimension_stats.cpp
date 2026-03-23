#include <dftracer/utils/core/sqlite/statement.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_dimension_stats.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/indexer/internal/error.h>

namespace dftracer::utils::utilities::composites::dft::indexing::queries {

using dftracer::utils::sqlite::SqliteStmt;

std::vector<ChunkDimensionStatsResult> query_chunk_dimension_stats(
    const SqliteDatabase& db, int file_info_id) {
    SqliteStmt stmt(db,
                    "SELECT checkpoint_idx, dimension, distinct_count, "
                    "min_value, max_value, value_type, value_counts "
                    "FROM chunk_dimension_stats WHERE file_info_id = ? "
                    "ORDER BY checkpoint_idx, dimension;");

    stmt.bind_int(1, file_info_id);

    std::vector<ChunkDimensionStatsResult> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ChunkDimensionStatsResult r;
        r.checkpoint_idx =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));

        const char* dim =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        r.dimension = dim ? dim : "";

        r.distinct_count =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 2));

        const char* min_val =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        r.min_value = min_val ? min_val : "";

        const char* max_val =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        r.max_value = max_val ? max_val : "";

        const char* vtype =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        r.value_type = vtype ? vtype : "string";

        // value_counts BLOB (compressed, may be NULL)
        if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) {
            auto* blob =
                static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 6));
            auto blob_len =
                static_cast<std::size_t>(sqlite3_column_bytes(stmt, 6));
            if (blob && blob_len > 0) {
                r.value_counts = ChunkDimensionStats::decompress_value_counts(
                    blob, blob_len);
            }
        }

        results.push_back(std::move(r));
    }

    return results;
}

std::vector<ChunkDimensionStatsResult>
query_chunk_dimension_stats_for_dimension(const SqliteDatabase& db,
                                          int file_info_id,
                                          std::string_view dimension) {
    SqliteStmt stmt(db,
                    "SELECT checkpoint_idx, dimension, distinct_count, "
                    "min_value, max_value, value_type, value_counts "
                    "FROM chunk_dimension_stats "
                    "WHERE file_info_id = ? AND dimension = ? "
                    "ORDER BY checkpoint_idx;");

    stmt.bind_int(1, file_info_id);
    stmt.bind_text(2, dimension);

    std::vector<ChunkDimensionStatsResult> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ChunkDimensionStatsResult r;
        r.checkpoint_idx =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));

        const char* dim =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        r.dimension = dim ? dim : "";

        r.distinct_count =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 2));

        const char* min_val =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        r.min_value = min_val ? min_val : "";

        const char* max_val =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        r.max_value = max_val ? max_val : "";

        const char* vtype =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        r.value_type = vtype ? vtype : "string";

        if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) {
            auto* blob =
                static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 6));
            auto blob_len =
                static_cast<std::size_t>(sqlite3_column_bytes(stmt, 6));
            if (blob && blob_len > 0) {
                r.value_counts = ChunkDimensionStats::decompress_value_counts(
                    blob, blob_len);
            }
        }

        results.push_back(std::move(r));
    }

    return results;
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing::queries
