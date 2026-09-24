#ifndef DFTRACER_UTILS_INDEX_PLAN_VIEW_RESOLVER_H
#define DFTRACER_UTILS_INDEX_PLAN_VIEW_RESOLVER_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/index/record_schema.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::index::plan {

/// Dictionary fields loaded once from the given indexes, for resolved select
/// columns and group keys. Lookups are in memory; an unknown key resolves to
/// "" (dfanalyzer treats it as missing).
class GroupResolver {
   public:
    /// Loads what the `resolved` column names read. Throws DFTUtilsException
    /// INVALID_ARGUMENT for a name `schema` does not resolve.
    GroupResolver(const std::vector<std::string>& index_paths,
                  const RecordSchema& schema,
                  const std::vector<std::string>& resolved);

    /// key -> value of the resolved column `name`; null when not loaded.
    const StringViewMap<std::string>* column(std::string_view name) const;
    const std::string& value(std::string_view name, std::string_view key) const;
    /// pid (as text) -> rank, harvested from PR metadata at query time (not
    /// indexed), so it is populated separately from the constructor.
    const std::string& rank(const std::string& pid) const;
    void set_rank(const std::string& pid, std::string rank) const {
        rank_[pid] = std::move(rank);
    }

   private:
    struct Table {
        std::string dictionary;
        std::string field;
        StringViewMap<std::string> values;
    };
    static const std::string EMPTY;
    std::vector<Table> tables_;
    std::vector<std::pair<std::string, std::size_t>> columns_;
    // Filled after construction (query-time PR metadata), hence mutable.
    mutable StringViewMap<std::string> rank_;
};

}  // namespace dftracer::utils::index::plan

#endif  // DFTRACER_UTILS_INDEX_PLAN_VIEW_RESOLVER_H
