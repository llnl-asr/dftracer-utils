#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_DB_ERROR_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_DB_ERROR_H

#include <dftracer/utils/utilities/indexer/error.h>
#include <rocksdb/status.h>

#include <string>
#include <string_view>

namespace dftracer::utils::utilities::indexer {

// Throw a DATABASE_ERROR carrying the RocksDB status text. Shared by the index
// database / writer contexts / provenance so the message format stays uniform.
[[noreturn]] inline void throw_db_error(std::string_view message,
                                        const ::rocksdb::Status& status) {
    throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                       std::string(message) + ": " + status.ToString());
}

}  // namespace dftracer::utils::utilities::indexer

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_DB_ERROR_H
