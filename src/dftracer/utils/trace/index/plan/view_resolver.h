// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_PLAN_VIEW_RESOLVER_H
#define DFTRACER_UTILS_TRACE_INDEX_PLAN_VIEW_RESOLVER_H

#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/index/plan/rowsets.h>

namespace dftracer::utils::trace::index::plan {

/// Value columns of stored row sets, each keyed by another of its columns,
/// loaded once for group keys. Lookups are in memory; a key with no row, or
/// a row set a file's index does not hold, resolves to "".
class GroupResolver {
   public:
    /// Column `name` maps each row's `key` to its `value` in row set
    /// `rowset`, both read as text.
    struct Column {
        dstd::String name;
        dstd::String rowset;
        dstd::String key;
        dstd::String value;
    };

    GroupResolver(const dstd::Vector<RowSetFile>& files,
                  const dstd::Vector<Column>& columns);

    /// key -> value of column `name`; null when not loaded.
    const dstd::StringViewMap<dstd::String>* column(
        dstd::StringView name) const;
    const dstd::String& value(dstd::StringView name,
                              dstd::StringView key) const;

   private:
    static const dstd::String EMPTY;
    dstd::Vector<dstd::Pair<dstd::String, dstd::StringViewMap<dstd::String>>>
        columns_;
};

}  // namespace dftracer::utils::trace::index::plan

#endif  // DFTRACER_UTILS_TRACE_INDEX_PLAN_VIEW_RESOLVER_H
