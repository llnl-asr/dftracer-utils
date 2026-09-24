#ifndef DFTRACER_UTILS_INDEX_STORE_INTERNAL_ITERATOR_CODEC_H
#define DFTRACER_UTILS_INDEX_STORE_INTERNAL_ITERATOR_CODEC_H

#include <rocksdb/iterator.h>

#include <string>

namespace dftracer::utils::index::store::internal {

// Generic rocksdb iterator slice copies. Kept apart from registry_codec so
// consumers that only need these (e.g. provenance_database) do not pull in the
// registry key helpers, which would clash with their own local key encoders.
inline std::string iterator_value(::rocksdb::Iterator& it) {
    const auto slice = it.value();
    return std::string(slice.data(), slice.size());
}

inline std::string iterator_key(::rocksdb::Iterator& it) {
    const auto slice = it.key();
    return std::string(slice.data(), slice.size());
}

}  // namespace dftracer::utils::index::store::internal

#endif  // DFTRACER_UTILS_INDEX_STORE_INTERNAL_ITERATOR_CODEC_H
