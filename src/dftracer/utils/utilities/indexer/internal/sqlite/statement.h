#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_SQLITE_STATEMENT_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_SQLITE_STATEMENT_H

// Forwarding header: SqliteStmt has moved to core/sqlite/.
// This header re-exports it into the indexer::internal namespace
// for backward compatibility.
#include <dftracer/utils/core/sqlite/statement.h>

namespace dftracer::utils::utilities::indexer::internal {
using dftracer::utils::sqlite::SqliteStmt;
}  // namespace dftracer::utils::utilities::indexer::internal

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_SQLITE_STATEMENT_H
