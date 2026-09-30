#include <dftracer/utils/dataframe/frame_ops.h>
#include <dftracer/utils/dataframe/internal/dataframe_handle.h>

#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace df = dftracer::utils::dataframe;

namespace {

std::vector<std::string> names_of(const char* const* items, std::int32_t n) {
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(n));
    for (std::int32_t i = 0; i < n; ++i) out.emplace_back(items[i]);
    return out;
}

bool valid_window_func(dftu_window_func f) {
    return f >= DFTU_WINDOW_ROW_NUMBER && f <= DFTU_WINDOW_FRAME_COLLECT;
}

}  // namespace

extern "C" {

dftu_dataframe* dftu_dataframe_window(
    const dftu_dataframe* frame, const char* const* partition_by,
    int32_t n_part, const char* const* order_by, int32_t n_order,
    const dftu_window_spec* specs, int32_t n_specs) {
    if (!frame || (n_part > 0 && !partition_by) || (n_order > 0 && !order_by) ||
        (n_specs > 0 && !specs))
        return nullptr;
    try {
        std::vector<df::WindowColumn> cols;
        cols.reserve(static_cast<std::size_t>(n_specs));
        for (int32_t i = 0; i < n_specs; ++i) {
            const dftu_window_spec& s = specs[i];
            if (!valid_window_func(s.func) || !s.out) return nullptr;
            cols.push_back(df::window_column(s));
        }
        return df::dataframe_handle_wrap(df::window(
            df::dataframe_handle_view(frame), names_of(partition_by, n_part),
            names_of(order_by, n_order), cols));
    } catch (const std::exception&) {
        return nullptr;
    }
}

dftu_dataframe* dftu_dataframe_gap_fill(
    const dftu_dataframe* frame, const char* const* partition_by,
    int32_t n_part, const char* time, int64_t bucket, const char* const* values,
    int32_t n_values, dftu_gap_fill_mode mode, const int64_t* range,
    int32_t n_range) {
    if (!frame || !time || (n_part > 0 && !partition_by) ||
        (n_values > 0 && !values) || (n_range != 0 && n_range != 2) ||
        (n_range == 2 && !range))
        return nullptr;
    if (mode != DFTU_GAP_FILL_NONE && mode != DFTU_GAP_FILL_LOCF &&
        mode != DFTU_GAP_FILL_LINEAR)
        return nullptr;
    try {
        std::optional<std::pair<int64_t, int64_t>> r;
        if (n_range == 2) r = std::make_pair(range[0], range[1]);
        return df::dataframe_handle_wrap(df::gap_fill(
            df::dataframe_handle_view(frame), names_of(partition_by, n_part),
            time, bucket, names_of(values, n_values),
            static_cast<df::GapFillMode>(mode), r));
    } catch (const std::exception&) {
        return nullptr;
    }
}

dftu_dataframe* dftu_dataframe_asof(const dftu_dataframe* left,
                                    const dftu_dataframe* right, const char* on,
                                    const char* const* by, int32_t n_by,
                                    dftu_asof_direction direction,
                                    double tolerance) {
    if (!left || !right || !on || (n_by > 0 && !by)) return nullptr;
    if (direction != DFTU_ASOF_BACKWARD && direction != DFTU_ASOF_FORWARD &&
        direction != DFTU_ASOF_NEAREST)
        return nullptr;
    try {
        std::optional<double> tol;
        if (tolerance >= 0) tol = tolerance;
        return df::dataframe_handle_wrap(
            df::asof(df::dataframe_handle_view(left),
                     df::dataframe_handle_view(right), on, names_of(by, n_by),
                     static_cast<df::AsofDirection>(direction), tol));
    } catch (const std::exception&) {
        return nullptr;
    }
}

dftu_dataframe* dftu_dataframe_interval(const dftu_dataframe* left,
                                        const dftu_dataframe* right,
                                        const char* point, const char* lo,
                                        const char* hi, const char* const* by,
                                        int32_t n_by, int32_t outer) {
    if (!left || !right || !point || !lo || !hi || (n_by > 0 && !by))
        return nullptr;
    try {
        return df::dataframe_handle_wrap(df::interval(
            df::dataframe_handle_view(left), df::dataframe_handle_view(right),
            point, lo, hi, names_of(by, n_by), outer != 0));
    } catch (const std::exception&) {
        return nullptr;
    }
}

}  // extern "C"
