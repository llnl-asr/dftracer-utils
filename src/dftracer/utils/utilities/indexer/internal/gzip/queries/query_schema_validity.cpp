#include <dftracer/utils/utilities/indexer/internal/gzip/queries/queries.h>
#include <dftracer/utils/utilities/indexer/internal/sqlite/statement.h>

namespace dftracer::utils::utilities::indexer::internal::gzip {

bool query_schema_validity(const SqliteDatabase &db) {
    SqliteStmt stmt(db,
                    "SELECT name FROM sqlite_master WHERE type='table' AND "
                    "name IN ('checkpoints', 'metadata', 'files')");
    int table_count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        table_count++;
    }

    return table_count >= 3;
}

}  // namespace dftracer::utils::utilities::indexer::internal::gzip
