#include <dftracer/utils/core/sqlite/statement.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_dimension_stats.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/indexer/internal/error.h>

namespace dftracer::utils::utilities::composites::dft::indexing::queries {

using dftracer::utils::sqlite::SqliteStmt;
using indexer::internal::IndexerError;

void insert_chunk_dimension_stats(const SqliteDatabase& db, int file_info_id,
                                  std::uint64_t checkpoint_idx,
                                  const ChunkDimensionStats& stats,
                                  std::size_t value_counts_cap) {
    SqliteStmt stmt(db,
                    "INSERT OR REPLACE INTO chunk_dimension_stats"
                    "(file_info_id, checkpoint_idx, dimension, distinct_count, "
                    "value_counts, min_value, max_value, value_type) "
                    "VALUES(?, ?, ?, ?, ?, ?, ?, ?);");

    stmt.bind_int(1, file_info_id);
    stmt.bind_int64(2, static_cast<std::int64_t>(checkpoint_idx));
    stmt.bind_text_static(3, stats.dimension);
    stmt.bind_int64(4, static_cast<std::int64_t>(stats.distinct_count));

    auto compressed = stats.compress_value_counts(value_counts_cap);
    if (compressed) {
        stmt.bind_blob_static(5, compressed->data(),
                              static_cast<int>(compressed->size()));
    } else {
        stmt.bind_null(5);
    }

    stmt.bind_text_static(6, stats.min_value);
    stmt.bind_text_static(7, stats.max_value);
    stmt.bind_text_static(8, stats.value_type);

    int result = sqlite3_step(stmt);
    if (result != SQLITE_DONE) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to insert chunk dimension stats: " +
                               std::string(sqlite3_errmsg(db.get())));
    }
}

SqliteStmt prepare_insert_chunk_dimension_stats(const SqliteDatabase& db) {
    return SqliteStmt(
        db,
        "INSERT OR REPLACE INTO chunk_dimension_stats"
        "(file_info_id, checkpoint_idx, dimension, distinct_count, "
        "value_counts, min_value, max_value, value_type) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?);");
}

void insert_chunk_dimension_stats(SqliteStmt& stmt, int file_info_id,
                                  std::uint64_t checkpoint_idx,
                                  const ChunkDimensionStats& stats,
                                  std::size_t value_counts_cap) {
    stmt.reset();
    stmt.bind_int(1, file_info_id);
    stmt.bind_int64(2, static_cast<std::int64_t>(checkpoint_idx));
    stmt.bind_text_static(3, stats.dimension);
    stmt.bind_int64(4, static_cast<std::int64_t>(stats.distinct_count));

    auto compressed = stats.compress_value_counts(value_counts_cap);
    if (compressed) {
        stmt.bind_blob_static(5, compressed->data(),
                              static_cast<int>(compressed->size()));
    } else {
        stmt.bind_null(5);
    }

    stmt.bind_text_static(6, stats.min_value);
    stmt.bind_text_static(7, stats.max_value);
    stmt.bind_text_static(8, stats.value_type);

    int result = sqlite3_step(stmt);
    if (result != SQLITE_DONE) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to insert chunk dimension stats");
    }
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing::queries
