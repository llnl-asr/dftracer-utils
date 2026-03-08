#include <dftracer/utils/core/sqlite/statement.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/indexer/internal/error.h>

#include <limits>

namespace dftracer::utils::utilities::composites::dft::indexing::queries {

using dftracer::utils::sqlite::SqliteStmt;

std::vector<ChunkStatisticsResult> query_chunk_statistics(
    const SqliteDatabase& db, int file_info_id) {
    SqliteStmt stmt(
        db,
        "SELECT checkpoint_idx, total_events, category_counts, "
        "name_counts, pid_tid_counts, min_timestamp_us, max_timestamp_us, "
        "duration_sum_us, duration_min_us, duration_max_us, duration_count, "
        "duration_m2, duration_sketch, name_category "
        "FROM chunk_statistics WHERE file_info_id = ? "
        "ORDER BY checkpoint_idx;");

    stmt.bind_int(1, file_info_id);

    std::vector<ChunkStatisticsResult> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ChunkStatisticsResult r;
        r.checkpoint_idx =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));

        r.stats.total_events =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 1));

        // Parse JSON text columns for maps
        const char* cat_text =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        if (cat_text) {
            r.stats.category_counts =
                ChunkStatistics::parse_counts_json(cat_text);
        }

        const char* name_text =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        if (name_text) {
            r.stats.name_counts = ChunkStatistics::parse_counts_json(name_text);
        }

        const char* pt_text =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        if (pt_text) {
            r.stats.pid_tid_counts =
                ChunkStatistics::parse_counts_json(pt_text);
        }

        // Timestamps (may be NULL)
        if (sqlite3_column_type(stmt, 5) != SQLITE_NULL) {
            r.stats.min_timestamp_us =
                static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 5));
        } else {
            r.stats.min_timestamp_us =
                std::numeric_limits<std::uint64_t>::max();
        }

        if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) {
            r.stats.max_timestamp_us =
                static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 6));
        } else {
            r.stats.max_timestamp_us = 0;
        }

        // Duration fields
        r.stats.duration_sum_us = sqlite3_column_int64(stmt, 7);

        if (sqlite3_column_type(stmt, 8) != SQLITE_NULL) {
            r.stats.duration_min_us =
                static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 8));
        } else {
            r.stats.duration_min_us = std::numeric_limits<std::uint64_t>::max();
        }

        if (sqlite3_column_type(stmt, 9) != SQLITE_NULL) {
            r.stats.duration_max_us =
                static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 9));
        } else {
            r.stats.duration_max_us = 0;
        }

        r.stats.duration_count =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 10));
        r.stats.duration_m2 = sqlite3_column_double(stmt, 11);

        // Deserialize duration_sketch BLOB (column 12)
        if (sqlite3_column_type(stmt, 12) != SQLITE_NULL) {
            auto* blob =
                static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 12));
            auto blob_len =
                static_cast<std::size_t>(sqlite3_column_bytes(stmt, 12));
            if (blob && blob_len > 0) {
                using dftracer::utils::utilities::common::statistics::DDSketch;
                r.stats.duration_sketch = DDSketch::deserialize(blob, blob_len);
            }
        }

        // Deserialize name_category TEXT (column 13)
        const char* nc_text =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
        if (nc_text) {
            r.stats.name_category =
                ChunkStatistics::parse_string_map_json(nc_text);
        }

        results.push_back(std::move(r));
    }

    return results;
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing::queries
