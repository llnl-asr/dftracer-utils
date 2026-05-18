#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_TRANSACTION_SCOPE_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_TRANSACTION_SCOPE_H

// TransactionScope has been removed.
// IndexDatabase no longer has begin_transaction/commit_transaction.
// Use the IndexBatchSink API on IndexDatabaseWriterContext (obtained via
// IndexDatabase::begin_write()) or individual insert methods followed by
// commit().

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_TRANSACTION_SCOPE_H
