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
        "(file_info_id, checkpoint_idx, total_events, category_counts, "
        "name_counts, pid_tid_counts, min_timestamp_us, max_timestamp_us, "
        "duration_sum_us, duration_min_us, duration_max_us, duration_count, "
        "duration_m2, duration_sketch, name_category) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");

    stmt.bind_int(1, file_info_id);
    stmt.bind_int64(2, static_cast<std::int64_t>(checkpoint_idx));
    stmt.bind_int64(3, static_cast<std::int64_t>(stats.total_events));
    stmt.bind_text(4, stats.category_counts_json());
    stmt.bind_text(5, stats.name_counts_json());
    stmt.bind_text(6, stats.pid_tid_counts_json());

    if (stats.min_timestamp_us != std::numeric_limits<std::uint64_t>::max()) {
        stmt.bind_int64(7, static_cast<std::int64_t>(stats.min_timestamp_us));
    } else {
        stmt.bind_null(7);
    }

    if (stats.max_timestamp_us != 0) {
        stmt.bind_int64(8, static_cast<std::int64_t>(stats.max_timestamp_us));
    } else {
        stmt.bind_null(8);
    }

    stmt.bind_int64(9, stats.duration_sum_us);

    if (stats.duration_min_us != std::numeric_limits<std::uint64_t>::max()) {
        stmt.bind_int64(10, static_cast<std::int64_t>(stats.duration_min_us));
    } else {
        stmt.bind_null(10);
    }

    if (stats.duration_max_us != 0) {
        stmt.bind_int64(11, static_cast<std::int64_t>(stats.duration_max_us));
    } else {
        stmt.bind_null(11);
    }

    stmt.bind_int64(12, static_cast<std::int64_t>(stats.duration_count));
    stmt.bind_double(13, stats.duration_m2);

    if (!stats.duration_sketch.empty()) {
        auto blob = stats.duration_sketch.serialize();
        stmt.bind_blob(14, blob.data(), static_cast<int>(blob.size()));
    } else {
        stmt.bind_null(14);
    }

    stmt.bind_text(15, stats.name_category_json());

    int result = sqlite3_step(stmt);
    if (result != SQLITE_DONE) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to insert chunk statistics: " +
                               std::string(sqlite3_errmsg(db.get())));
    }
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing::queries
