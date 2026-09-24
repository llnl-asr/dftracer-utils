#include <dftracer/utils/index/plan/view_resolver.h>
#include <dftracer/utils/index/store/index_database.h>

namespace dftracer::utils::index::plan {

const std::string GroupResolver::EMPTY;

GroupResolver::GroupResolver(const std::vector<std::string>& index_paths,
                             const RecordSchema& schema,
                             const std::vector<std::string>& resolved) {
    for (const auto& name : resolved) {
        if (column(name)) continue;
        const ResolvedColumn rc = schema.resolved_column(name);
        std::size_t t = 0;
        while (t < tables_.size() &&
               (tables_[t].dictionary != rc.dictionary->name ||
                tables_[t].field != rc.field))
            ++t;
        if (t == tables_.size())
            tables_.push_back({rc.dictionary->name, rc.field, {}});
        columns_.emplace_back(name, t);
    }
    if (tables_.empty()) return;
    for (const auto& path : index_paths) {
        if (path.empty()) continue;
        try {
            index::store::IndexDatabase db(
                path, index::store::IndexOpenMode::ReadOnly);
            for (auto& t : tables_)
                for (auto& [k, v] : db.dict_field(t.dictionary, t.field))
                    t.values.emplace(k, std::move(v));
        } catch (const std::exception&) {
            // Unreadable index: its keys just resolve to "".
        }
    }
}

const StringViewMap<std::string>* GroupResolver::column(
    std::string_view name) const {
    for (const auto& [n, t] : columns_)
        if (n == name) return &tables_[t].values;
    return nullptr;
}

const std::string& GroupResolver::value(std::string_view name,
                                        std::string_view key) const {
    const auto* c = column(name);
    if (!c) return EMPTY;
    auto it = c->find(key);
    return it != c->end() ? it->second : EMPTY;
}

const std::string& GroupResolver::rank(const std::string& pid) const {
    auto it = rank_.find(pid);
    return it != rank_.end() ? it->second : EMPTY;
}

}  // namespace dftracer::utils::index::plan
