#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/trace/views/batch_bridge.h>
#include <dftracer/utils/trace/views/native_row_fold.h>

#include <utility>

namespace dftracer::utils::trace::views::detail {

namespace {

dataframe::DataFrame base_frame(std::span<const FoldEvent> events,
                                const dftracer::utils::StringIntern& intern,
                                const ColumnSpec& spec) {
    dataframe::DataFrame f = build_row_frame(events, intern, spec.select,
                                             spec.time_scale, spec.by_path);
    if (spec.json)
        for (std::size_t i = 0; i < f.names.size(); ++i) {
            const auto it = spec.json->find(f.names[i]);
            dataframe::Series& c = f.columns[i];
            if (it == spec.json->end() || c.is_json()) continue;
            c = it->second && c.type() == dataframe::TypeId::String
                    ? c.as_json()
                    : dataframe::to_json_series(c);
        }
    return f;
}

}  // namespace

dataframe::DataFrame events_to_frame(
    std::span<const FoldEvent> events,
    const dftracer::utils::StringIntern& intern, const ColumnSpec& spec) {
    dataframe::DataFrame f = base_frame(events, intern, spec);
    if (spec.emit_dyn)
        for (auto& [name, col] :
             build_dyn_numeric_columns(events, intern, spec.select)) {
            f.names.push_back(std::move(name));
            f.columns.push_back(std::move(col));
        }
    return f;
}

dataframe::Morsel events_to_morsel(
    std::span<const FoldEvent> events,
    std::shared_ptr<dftracer::utils::StringIntern> intern,
    const ColumnSpec& spec) {
    dataframe::DataFrame f = base_frame(events, *intern, spec);

    dataframe::Morsel m;
    m.rows = f.num_rows();
    m.columns = std::move(f.columns);
    dataframe::MorselDyn& d = m.dyn_state();
    d.name_ids.reserve(f.names.size());
    for (const std::string& name : f.names)
        d.name_ids.push_back(intern->get_or_insert(name));

    if (spec.emit_dyn) {
        auto dyn = build_dyn_numeric_columns(events, *intern, spec.select);
        d.dyn_names.reserve(dyn.size());
        d.dyn_columns.reserve(dyn.size());
        for (auto& [name, col] : dyn) {
            d.dyn_names.push_back(std::move(name));
            d.dyn_columns.push_back(std::move(col));
        }
    }
    d.intern = std::move(intern);
    return m;
}

}  // namespace dftracer::utils::trace::views::detail
