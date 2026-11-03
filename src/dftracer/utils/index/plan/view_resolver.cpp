// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/plan/view_resolver.h>

namespace dftracer::utils::index::plan {

namespace {

namespace df = dftracer::utils::dataframe;

dstd::String cell_text(const df::Series& s, std::int64_t r) {
    switch (s.type()) {
        case df::TypeId::String:
            return dstd::String(s.string_at(r));
        case df::TypeId::Int64:
            return dstd::to_string(s.data<std::int64_t>()[r]);
        case df::TypeId::Uint64:
            return dstd::to_string(s.data<std::uint64_t>()[r]);
        case df::TypeId::Float64:
            return dstd::to_string(s.data<double>()[r]);
        default:
            return {};
    }
}

}  // namespace

const dstd::String GroupResolver::EMPTY;

GroupResolver::GroupResolver(const dstd::Vector<RowSetFile>& files,
                             const dstd::Vector<Column>& columns) {
    for (const auto& c : columns) {
        if (column(c.name)) continue;
        dstd::StringViewMap<dstd::String> values;
        if (auto f = stored_rowset(files, c.rowset)) {
            const auto k = f->column_index(c.key);
            const auto v = f->column_index(c.value);
            if (k >= 0 && v >= 0) {
                const df::Series keys =
                    f->columns[static_cast<std::size_t>(k)].materialize();
                const df::Series vals =
                    f->columns[static_cast<std::size_t>(v)].materialize();
                for (std::int64_t r = 0; r < f->num_rows(); ++r)
                    if (!keys.is_null(r) && !vals.is_null(r))
                        values.emplace(cell_text(keys, r), cell_text(vals, r));
            }
        }
        columns_.emplace_back(c.name, std::move(values));
    }
}

const dstd::StringViewMap<dstd::String>* GroupResolver::column(
    dstd::StringView name) const {
    for (const auto& [n, values] : columns_)
        if (n == name) return &values;
    return nullptr;
}

const dstd::String& GroupResolver::value(dstd::StringView name,
                                         dstd::StringView key) const {
    const auto* c = column(name);
    if (!c) return EMPTY;
    auto it = c->find(key);
    return it != c->end() ? it->second : EMPTY;
}

}  // namespace dftracer::utils::index::plan
