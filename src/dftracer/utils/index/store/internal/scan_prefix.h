#ifndef DFTRACER_UTILS_INDEX_STORE_INTERNAL_SCAN_PREFIX_H
#define DFTRACER_UTILS_INDEX_STORE_INTERNAL_SCAN_PREFIX_H

#include <dftracer/utils/index/store/database.h>
#include <dftracer/utils/index/store/error.h>
#include <dftracer/utils/index/store/internal/db_error.h>
#include <rocksdb/iterator.h>
#include <rocksdb/slice.h>
#include <rocksdb/status.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace dftracer::utils::index::store::internal {

// Every read of the store goes through these. An iterator that fails mid-scan
// (for example "Too many open files" on an SST) turns invalid exactly like one
// that reached the end, so only the status tells the two apart.

// Calls fn(it) for each entry from the first key at or after `start` while fn
// returns true. Throws when the iterator ended on an error.
template <typename Fn>
void scan_from(std::string_view error_message,
               std::unique_ptr<::rocksdb::Iterator> it, std::string_view start,
               Fn&& fn) {
    for (it->Seek(::rocksdb::Slice(start.data(), start.size())); it->Valid();
         it->Next()) {
        if (!fn(*it)) break;
    }

    const auto status = it->status();
    if (!status.ok()) {
        throw_db_error(error_message, status);
    }
}

template <typename IteratorFactory, typename Fn>
void scan_prefix_iterator(std::string_view error_message,
                          std::string_view prefix,
                          IteratorFactory&& make_iterator, Fn&& fn) {
    scan_from(error_message, make_iterator(), prefix,
              [&](::rocksdb::Iterator& it) {
                  if (!std::string_view(it.key().data(), it.key().size())
                           .starts_with(prefix))
                      return false;
                  fn(it);
                  return true;
              });
}

// Whether any key starts with `prefix`; an empty prefix asks whether the
// family holds anything.
inline bool any_key_with_prefix(std::string_view error_message,
                                std::unique_ptr<::rocksdb::Iterator> it,
                                std::string_view prefix) {
    bool found = false;
    scan_from(error_message, std::move(it), prefix,
              [&](::rocksdb::Iterator& at) {
                  found = std::string_view(at.key().data(), at.key().size())
                              .starts_with(prefix);
                  return false;
              });
    return found;
}

// Reads `key` into `value`. False when the key is absent; any other failure
// throws.
inline bool get_checked(
    std::string_view error_message, const RocksDatabase& db,
    std::string_view key, std::string* value,
    std::string_view column_family = index::store::cf::DEFAULT) {
    const auto status = db.get(key, value, column_family);
    if (status.IsNotFound()) return false;
    if (!status.ok()) throw_db_error(error_message, status);
    return true;
}

}  // namespace dftracer::utils::index::store::internal

#endif  // DFTRACER_UTILS_INDEX_STORE_INTERNAL_SCAN_PREFIX_H
