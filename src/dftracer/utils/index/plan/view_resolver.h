#ifndef DFTRACER_UTILS_INDEX_PLAN_VIEW_RESOLVER_H
#define DFTRACER_UTILS_INDEX_PLAN_VIEW_RESOLVER_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/index/plan/rowsets.h>

#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::index::plan {

/// Value columns of stored row sets, each keyed by another of its columns,
/// loaded once for group keys. Lookups are in memory; a key with no row, or
/// a row set a file's index does not hold, resolves to "".
class GroupResolver {
   public:
    /// Column `name` maps each row's `key` to its `value` in row set
    /// `rowset`, both read as text.
    struct Column {
        std::string name;
        std::string rowset;
        std::string key;
        std::string value;
    };

    GroupResolver(const std::vector<RowSetFile>& files,
                  const std::vector<Column>& columns);

    /// key -> value of column `name`; null when not loaded.
    const StringViewMap<std::string>* column(std::string_view name) const;
    const std::string& value(std::string_view name, std::string_view key) const;

   private:
    static const std::string EMPTY;
    std::vector<std::pair<std::string, StringViewMap<std::string>>> columns_;
};

}  // namespace dftracer::utils::index::plan

#endif  // DFTRACER_UTILS_INDEX_PLAN_VIEW_RESOLVER_H
