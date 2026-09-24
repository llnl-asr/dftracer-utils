#include <dftracer/utils/index/extensions/dict_fold.h>

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace dftracer::utils::index::extensions {

namespace {

// The value of `path` in a metadata record as text (a number as its JSON
// text): an args path is stored under its bare key, unless the record was
// decoded by path.
std::string text_at(const trace::views::detail::FoldEvent& e,
                    std::string_view path,
                    const dftracer::utils::StringIntern& intern) {
    if (!e.by_path && path.starts_with("args.")) path.remove_prefix(5);
    for (const auto& [k, v] : e.args) {
        if (intern.resolve(k) != path) continue;
        if (const auto* id = std::get_if<std::uint32_t>(&v))
            return std::string(intern.resolve(*id));
        if (const auto* i = std::get_if<std::int64_t>(&v))
            return std::to_string(*i);
        char buf[32];
        const auto r =
            std::to_chars(buf, buf + sizeof(buf), std::get<double>(v));
        return std::string(buf, r.ptr);
    }
    return {};
}

}  // namespace

void DictFold::step(const trace::views::detail::FoldBatch& batch) {
    dict_.set_index_path(batch.unit.index_path);
    for (const auto& e : batch.events) {
        if (e.phase != trace::RecordPhase::METADATA) continue;
        const std::string_view record = intern_->resolve(e.name_id);
        for (const auto& d : dictionaries_) {
            if (d.rows != record) continue;
            const std::string key = text_at(e, d.key, *intern_);
            if (key.empty()) continue;
            DictionaryRows::Fields fields;
            for (const auto& [name, path] : d.fields) {
                std::string v = text_at(e, path, *intern_);
                if (!v.empty()) fields.emplace_back(name, std::move(v));
            }
            if (!fields.empty()) dict_.add(d.name, key, fields);
        }
    }
}

}  // namespace dftracer::utils::index::extensions
