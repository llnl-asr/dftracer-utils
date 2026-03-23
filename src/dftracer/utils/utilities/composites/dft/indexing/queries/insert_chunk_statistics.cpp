#include <dftracer/utils/core/sqlite/statement.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/indexer/internal/error.h>

namespace dftracer::utils::utilities::composites::dft::indexing::queries {

using dftracer::utils::sqlite::SqliteStmt;
using indexer::internal::IndexerError;

void insert_chunk_statistics(const SqliteDatabase& db, int file_info_id,
                             std::uint64_t checkpoint_idx,
                             const ChunkStatistics& stats) {
    SqliteStmt stmt(
        db,
        "INSERT OR REPLACE INTO chunk_statistics"
        "(file_info_id, checkpoint_idx, total_events, "
        "min_timestamp_us, max_timestamp_us, "
        "duration_sum_us, duration_min_us, duration_max_us, duration_count, "
        "duration_m2, duration_sketch, duration_histogram, "
        "name_duration_sketches, name_duration_histograms, "
        "name_duration_sums, name_duration_sum_sqs, name_category) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");

    stmt.bind_int(1, file_info_id);
    stmt.bind_int64(2, static_cast<std::int64_t>(checkpoint_idx));
    stmt.bind_int64(3, static_cast<std::int64_t>(stats.total_events));

    if (stats.min_timestamp_us != std::numeric_limits<std::uint64_t>::max()) {
        stmt.bind_int64(4, static_cast<std::int64_t>(stats.min_timestamp_us));
    } else {
        stmt.bind_null(4);
    }

    if (stats.max_timestamp_us != 0) {
        stmt.bind_int64(5, static_cast<std::int64_t>(stats.max_timestamp_us));
    } else {
        stmt.bind_null(5);
    }

    stmt.bind_int64(6, stats.duration_sum_us);

    if (stats.duration_min_us != std::numeric_limits<std::uint64_t>::max()) {
        stmt.bind_int64(7, static_cast<std::int64_t>(stats.duration_min_us));
    } else {
        stmt.bind_null(7);
    }

    if (stats.duration_max_us != 0) {
        stmt.bind_int64(8, static_cast<std::int64_t>(stats.duration_max_us));
    } else {
        stmt.bind_null(8);
    }

    stmt.bind_int64(9, static_cast<std::int64_t>(stats.duration_count));
    stmt.bind_double(10, stats.duration_m2);

    if (!stats.duration_sketch.empty()) {
        auto blob = stats.duration_sketch.serialize();
        stmt.bind_blob(11, blob.data(), static_cast<int>(blob.size()));
    } else {
        stmt.bind_null(11);
    }

    stmt.bind_text(12, stats.duration_histogram.to_json());

    if (!stats.name_duration_sketches.empty()) {
        auto blob = stats.serialize_name_duration_sketches();
        stmt.bind_blob(13, blob.data(), static_cast<int>(blob.size()));
    } else {
        stmt.bind_null(13);
    }

    stmt.bind_text(14, stats.name_duration_histograms_json());
    stmt.bind_text(15, stats.name_duration_sums_json());
    stmt.bind_text(16, stats.name_duration_sum_sqs_json());
    stmt.bind_text(17, stats.name_category_json());

    int result = sqlite3_step(stmt);
    if (result != SQLITE_DONE) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to insert chunk statistics: " +
                               std::string(sqlite3_errmsg(db.get())));
    }
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing::queries
