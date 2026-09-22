#include <dftracer/utils/trace/views/batch_bridge.h>
#include <dftracer/utils/trace/views/native_row_fold.h>

#include <utility>

namespace dftracer::utils::trace::views::detail {

namespace {

dataframe::DataFrame base_frame(const std::vector<FoldEvent>& events,
                                const dftracer::utils::StringIntern& intern,
                                const ColumnSpec& spec) {
    return build_row_frame(events, intern, spec.select, spec.time_scale,
                           spec.resolver);
}

}  // namespace

dataframe::DataFrame events_to_frame(
    const std::vector<FoldEvent>& events,
    const dftracer::utils::StringIntern& intern, const ColumnSpec& spec) {
    dataframe::DataFrame f = base_frame(events, intern, spec);
    if (spec.emit_dyn)
        for (auto& [name, col] : build_dyn_numeric_columns(events, intern)) {
            f.names.push_back(std::move(name));
            f.columns.push_back(std::move(col));
        }
    return f;
}

dataframe::Morsel events_to_morsel(
    const std::vector<FoldEvent>& events,
    std::shared_ptr<dftracer::utils::StringIntern> intern,
    const ColumnSpec& spec) {
    dataframe::DataFrame f = base_frame(events, *intern, spec);

    dataframe::Morsel m;
    m.rows = f.num_rows();
    m.columns = std::move(f.columns);
    m.name_ids.reserve(f.names.size());
    for (const std::string& name : f.names)
        m.name_ids.push_back(intern->get_or_insert(name));

    if (spec.emit_dyn) {
        auto dyn = build_dyn_numeric_columns(events, *intern);
        m.dyn_names.reserve(dyn.size());
        m.dyn_columns.reserve(dyn.size());
        for (auto& [name, col] : dyn) {
            m.dyn_names.push_back(std::move(name));
            m.dyn_columns.push_back(std::move(col));
        }
    }
    m.intern = std::move(intern);
    return m;
}

}  // namespace dftracer::utils::trace::views::detail
