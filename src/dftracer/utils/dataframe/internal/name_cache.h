// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_NAME_CACHE_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_NAME_CACHE_H

#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/shared_string.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <algorithm>

namespace dftracer::utils::dataframe {

// The names a cursor puts on its frames. A name is built once and every later
// frame copies its reference count, so a stream of many morsels allocates no
// name per morsel. One cursor owns one cache; it is not thread safe.
class NameCache {
   public:
    dstd::SharedString shared(dstd::StringView name) {
        const auto it = names_.find(name);
        if (it != names_.end()) return *it;
        return *names_.insert(dstd::SharedString{name}).first;
    }

    // The names of a schema that stays the same for the life of the cache:
    // built on the first call, copied by reference count afterwards. `names`
    // is read only on the first call.
    const dstd::Vector<dstd::SharedString>& fixed(
        dstd::Span<const dstd::String> names) {
        if (!has_fixed_) {
            for (const dstd::String& n : names) fixed_.push_back(shared(n));
            has_fixed_ = true;
        }
        return fixed_;
    }

    // The names of `key`, rebuilt only when `key` differs from the last one.
    // For a schema that can change between morsels.
    const dstd::Vector<dstd::SharedString>& schema(
        dstd::Span<const dstd::String> key) {
        if (!std::equal(key.begin(), key.end(), key_.begin(), key_.end())) {
            key_.assign(key.begin(), key.end());
            schema_.clear();
            for (const dstd::String& n : key) schema_.push_back(shared(n));
        }
        return schema_;
    }

   private:
    dstd::HashSet<dstd::SharedString> names_;
    dstd::Vector<dstd::String> key_;
    dstd::Vector<dstd::SharedString> schema_;
    dstd::Vector<dstd::SharedString> fixed_;
    bool has_fixed_ = false;
};

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_NAME_CACHE_H
