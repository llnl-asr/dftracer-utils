#ifndef DFTRACER_UTILS_INDEX_STORE_INTERNAL_DB_ERROR_H
#define DFTRACER_UTILS_INDEX_STORE_INTERNAL_DB_ERROR_H

#include <dftracer/utils/index/store/error.h>
#include <rocksdb/status.h>

#include <string>
#include <string_view>

namespace dftracer::utils::index::store::internal {

// Throw a DATABASE_ERROR carrying the RocksDB status text. Shared by the index
// database / writer contexts / provenance so the message format stays uniform.
[[noreturn]] inline void throw_db_error(std::string_view message,
                                        const ::rocksdb::Status& status) {
    throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                       std::string(message) + ": " + status.ToString());
}

}  // namespace dftracer::utils::index::store::internal

#endif  // DFTRACER_UTILS_INDEX_STORE_INTERNAL_DB_ERROR_H
