#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_SCAN_PREFIX_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_SCAN_PREFIX_H

#include <dftracer/utils/utilities/indexer/error.h>
#include <dftracer/utils/utilities/indexer/internal/db_error.h>
#include <rocksdb/iterator.h>
#include <rocksdb/slice.h>
#include <rocksdb/status.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace dftracer::utils::utilities::indexer::internal {

template <typename IteratorFactory, typename Fn>
void scan_prefix_iterator(std::string_view error_message,
                          std::string_view prefix,
                          IteratorFactory&& make_iterator, Fn&& fn) {
    auto it = make_iterator();
    for (it->Seek(::rocksdb::Slice(prefix.data(), prefix.size()));
         it->Valid() && std::string_view(it->key().data(), it->key().size())
                            .starts_with(prefix);
         it->Next()) {
        fn(*it);
    }

    const auto status = it->status();
    if (!status.ok()) {
        throw_db_error(error_message, status);
    }
}

}  // namespace dftracer::utils::utilities::indexer::internal

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_SCAN_PREFIX_H
