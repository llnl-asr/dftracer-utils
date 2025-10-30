#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/indexer/internal/gzip/queries/queries.h>
#include <dftracer/utils/utilities/indexer/internal/sqlite/statement.h>

namespace dftracer::utils::utilities::indexer::internal::gzip {

bool delete_file_record(const SqliteDatabase &db, int file_id) {
    const char *cleanup_queries[] = {
        "DELETE FROM checkpoints WHERE file_id = ?;",
        "DELETE FROM metadata WHERE file_id = ?;"};

    for (const char *query : cleanup_queries) {
        try {
            SqliteStmt stmt(db, query);
            stmt.bind_int(1, file_id);
            int result = sqlite3_step(stmt);
            if (result != SQLITE_DONE) {
                DFTRACER_UTILS_LOG_ERROR(
                    "Failed to execute cleanup statement '%s' for file_id %d: "
                    "%d - "
                    "%s",
                    query, file_id, result, sqlite3_errmsg(db.get()));
                return false;
            }
        } catch (const IndexerError &e) {
            DFTRACER_UTILS_LOG_ERROR(
                "Failed to prepare cleanup statement '%s': %s", query,
                e.what());
            return false;
        }
    }
    DFTRACER_UTILS_LOG_DEBUG(
        "Successfully cleaned up existing data for file_id %d", file_id);
    return true;
}

}  // namespace dftracer::utils::utilities::indexer::internal::gzip
