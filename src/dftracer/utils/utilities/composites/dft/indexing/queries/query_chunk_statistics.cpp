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
        "SELECT checkpoint_idx, total_events, "
        "min_timestamp_us, max_timestamp_us, "
        "duration_sum_us, duration_min_us, duration_max_us, duration_count, "
        "duration_m2, duration_sketch, duration_histogram, "
        "name_duration_sketches, name_duration_histograms, "
        "name_duration_sums, name_duration_sum_sqs, name_category "
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

        // Timestamps (may be NULL)
        if (sqlite3_column_type(stmt, 2) != SQLITE_NULL) {
            r.stats.min_timestamp_us =
                static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 2));
        } else {
            r.stats.min_timestamp_us =
                std::numeric_limits<std::uint64_t>::max();
        }

        if (sqlite3_column_type(stmt, 3) != SQLITE_NULL) {
            r.stats.max_timestamp_us =
                static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 3));
        } else {
            r.stats.max_timestamp_us = 0;
        }

        r.stats.duration_sum_us = sqlite3_column_int64(stmt, 4);

        if (sqlite3_column_type(stmt, 5) != SQLITE_NULL) {
            r.stats.duration_min_us =
                static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 5));
        } else {
            r.stats.duration_min_us = std::numeric_limits<std::uint64_t>::max();
        }

        if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) {
            r.stats.duration_max_us =
                static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 6));
        } else {
            r.stats.duration_max_us = 0;
        }

        r.stats.duration_count =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 7));
        r.stats.duration_m2 = sqlite3_column_double(stmt, 8);

        // duration_sketch BLOB (column 9)
        if (sqlite3_column_type(stmt, 9) != SQLITE_NULL) {
            auto* blob =
                static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 9));
            auto blob_len =
                static_cast<std::size_t>(sqlite3_column_bytes(stmt, 9));
            if (blob && blob_len > 0) {
                using dftracer::utils::utilities::common::statistics::DDSketch;
                r.stats.duration_sketch = DDSketch::deserialize(blob, blob_len);
            }
        }

        // duration_histogram TEXT (column 10)
        const char* dh_text =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        if (dh_text) {
            using dftracer::utils::utilities::common::statistics::Log2Histogram;
            r.stats.duration_histogram = Log2Histogram::from_json(dh_text);
        }

        // name_duration_sketches BLOB (column 11)
        if (sqlite3_column_type(stmt, 11) != SQLITE_NULL) {
            auto* blob =
                static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 11));
            auto blob_len =
                static_cast<std::size_t>(sqlite3_column_bytes(stmt, 11));
            if (blob && blob_len > 0) {
                r.stats.name_duration_sketches =
                    ChunkStatistics::deserialize_name_duration_sketches(
                        blob, blob_len);
            }
        }

        // name_duration_histograms TEXT (column 12)
        const char* ndh_text =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
        if (ndh_text) {
            r.stats.name_duration_histograms =
                ChunkStatistics::parse_histogram_map_json(ndh_text);
        }

        // name_duration_sums TEXT (column 13)
        const char* nds_text =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
        if (nds_text) {
            r.stats.name_duration_sums =
                ChunkStatistics::parse_double_map_json(nds_text);
        }

        // name_duration_sum_sqs TEXT (column 14)
        const char* ndss_text =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 14));
        if (ndss_text) {
            r.stats.name_duration_sum_sqs =
                ChunkStatistics::parse_double_map_json(ndss_text);
        }

        // name_category TEXT (column 15)
        const char* nc_text =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 15));
        if (nc_text) {
            r.stats.name_category =
                ChunkStatistics::parse_string_map_json(nc_text);
        }

        results.push_back(std::move(r));
    }

    return results;
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing::queries
