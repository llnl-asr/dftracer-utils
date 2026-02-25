#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_SQLITE_DATABASE_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_SQLITE_DATABASE_H

// Forwarding header: SqliteDatabase has moved to core/sqlite/.
// This header re-exports it into the indexer::internal namespace
// for backward compatibility.
#include <dftracer/utils/core/sqlite/database.h>

namespace dftracer::utils::utilities::indexer::internal {
using dftracer::utils::sqlite::SqliteDatabase;
}  // namespace dftracer::utils::utilities::indexer::internal

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_SQLITE_DATABASE_H
