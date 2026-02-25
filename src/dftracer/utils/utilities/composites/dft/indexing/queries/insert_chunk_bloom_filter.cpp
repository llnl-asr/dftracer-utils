#include <dftracer/utils/core/sqlite/statement.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/indexer/internal/error.h>

namespace dftracer::utils::utilities::composites::dft::indexing::queries {

using dftracer::utils::sqlite::SqliteStmt;
using indexer::internal::IndexerError;

void insert_chunk_bloom_filter(const SqliteDatabase& db, int file_info_id,
                               std::uint64_t checkpoint_idx,
                               const std::string& dimension,
                               const void* blob_data, int blob_size,
                               std::uint64_t num_entries) {
    SqliteStmt stmt(
        db,
        "INSERT OR REPLACE INTO chunk_bloom_filters"
        "(file_info_id, checkpoint_idx, dimension, bloom_data, num_entries) "
        "VALUES(?, ?, ?, ?, ?);");

    stmt.bind_int(1, file_info_id);
    stmt.bind_int64(2, static_cast<std::int64_t>(checkpoint_idx));
    stmt.bind_text(3, dimension);
    stmt.bind_blob(4, blob_data, blob_size);
    stmt.bind_int64(5, static_cast<std::int64_t>(num_entries));

    int result = sqlite3_step(stmt);
    if (result != SQLITE_DONE) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to insert chunk bloom filter: " +
                               std::string(sqlite3_errmsg(db.get())));
    }
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing::queries
