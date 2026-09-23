#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

// nanoarrow must precede arrow.h: arrow_abi.h (pulled in by arrow.h) defines
// ARROW_FLAG_DICTIONARY_ORDERED, nanoarrow's outer include guard, which would
// otherwise suppress its ArrowArrayStream definition.
// clang-format off
#include <nanoarrow/nanoarrow.h>
// clang-format on

#include <dftracer/utils/dataframe/arrow.h>
#include <dftracer/utils/dataframe/internal/dataframe_handle.h>
#include <dftracer/utils/utilities/common/arrow/frame_ops.h>

#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::utilities::common::arrow {

static_assert(static_cast<int>(WindowFunc::ROW_NUMBER) ==
              DFTU_WINDOW_ROW_NUMBER);
static_assert(static_cast<int>(WindowFunc::RANK) == DFTU_WINDOW_RANK);
static_assert(static_cast<int>(WindowFunc::DENSE_RANK) ==
              DFTU_WINDOW_DENSE_RANK);
static_assert(static_cast<int>(WindowFunc::LAG) == DFTU_WINDOW_LAG);
static_assert(static_cast<int>(WindowFunc::LEAD) == DFTU_WINDOW_LEAD);
static_assert(static_cast<int>(WindowFunc::RUNNING_SUM) ==
              DFTU_WINDOW_RUNNING_SUM);
static_assert(static_cast<int>(WindowFunc::RUNNING_MIN) ==
              DFTU_WINDOW_RUNNING_MIN);
static_assert(static_cast<int>(WindowFunc::RUNNING_MAX) ==
              DFTU_WINDOW_RUNNING_MAX);
static_assert(static_cast<int>(WindowFunc::RUNNING_COUNT) ==
              DFTU_WINDOW_RUNNING_COUNT);
static_assert(static_cast<int>(WindowFunc::DELTA) == DFTU_WINDOW_DELTA);
static_assert(static_cast<int>(WindowFunc::RATE) == DFTU_WINDOW_RATE);
static_assert(static_cast<int>(WindowFunc::SESSIONIZE) ==
              DFTU_WINDOW_SESSIONIZE);
static_assert(static_cast<int>(WindowFunc::FRAME_SUM) == DFTU_WINDOW_FRAME_SUM);
static_assert(static_cast<int>(WindowFunc::FRAME_MIN) == DFTU_WINDOW_FRAME_MIN);
static_assert(static_cast<int>(WindowFunc::FRAME_MAX) == DFTU_WINDOW_FRAME_MAX);
static_assert(static_cast<int>(WindowFunc::FRAME_COUNT) ==
              DFTU_WINDOW_FRAME_COUNT);
static_assert(static_cast<int>(WindowFunc::FRAME_MEAN) ==
              DFTU_WINDOW_FRAME_MEAN);
static_assert(static_cast<int>(WindowFunc::NTILE) == DFTU_WINDOW_NTILE);
static_assert(static_cast<int>(WindowFunc::FIRST_VALUE) ==
              DFTU_WINDOW_FIRST_VALUE);
static_assert(static_cast<int>(WindowFunc::LAST_VALUE) ==
              DFTU_WINDOW_LAST_VALUE);
static_assert(static_cast<int>(WindowFunc::NTH_VALUE) == DFTU_WINDOW_NTH_VALUE);
static_assert(static_cast<int>(WindowFunc::PERCENT_RANK) ==
              DFTU_WINDOW_PERCENT_RANK);
static_assert(static_cast<int>(WindowFunc::CUME_DIST) == DFTU_WINDOW_CUME_DIST);
static_assert(static_cast<int>(WindowFunc::FILL_FORWARD) ==
              DFTU_WINDOW_FILL_FORWARD);
static_assert(static_cast<int>(WindowFunc::RUNNING_PROD) ==
              DFTU_WINDOW_RUNNING_PROD);
static_assert(WINDOW_UNBOUNDED == DFTU_WINDOW_UNBOUNDED);
static_assert(static_cast<int>(GapFillMode::NONE) == DFTU_GAP_FILL_NONE);
static_assert(static_cast<int>(GapFillMode::LOCF) == DFTU_GAP_FILL_LOCF);
static_assert(static_cast<int>(GapFillMode::LINEAR) == DFTU_GAP_FILL_LINEAR);
static_assert(static_cast<int>(AsofDirection::BACKWARD) == DFTU_ASOF_BACKWARD);
static_assert(static_cast<int>(AsofDirection::FORWARD) == DFTU_ASOF_FORWARD);
static_assert(static_cast<int>(AsofDirection::NEAREST) == DFTU_ASOF_NEAREST);

namespace {

using dataframe::DataFrame;

std::uint32_t col_index(const DataFrame& df, const std::string& name,
                        const char* who) {
    std::int64_t i = df.column_index(name);
    if (i < 0)
        throw std::out_of_range(std::string(who) + ": no column named " + name);
    return static_cast<std::uint32_t>(i);
}

std::vector<std::uint32_t> col_indices(const DataFrame& df,
                                       const std::vector<std::string>& names,
                                       const char* who) {
    std::vector<std::uint32_t> out;
    out.reserve(names.size());
    for (const std::string& n : names) out.push_back(col_index(df, n, who));
    return out;
}

DataFrame import_result(ArrowExportResult result, const char* who) {
    if (!result.valid())
        throw std::runtime_error(std::string(who) + ": invalid Arrow result");
    DataFrame df =
        DataFrame::from_arrow(result.get_schema(), result.get_array());
    if (df.num_columns() == 0 && result.num_columns() != 0)
        throw std::runtime_error(std::string(who) +
                                 ": failed to import result columns");
    return df;
}

std::vector<std::string> names_of(const char* const* items, std::int32_t n) {
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(n));
    for (std::int32_t i = 0; i < n; ++i) out.emplace_back(items[i]);
    return out;
}

bool valid_window_func(dftu_window_func f) {
    return f >= DFTU_WINDOW_ROW_NUMBER && f <= DFTU_WINDOW_RUNNING_PROD;
}

}  // namespace

DataFrame window(const DataFrame& df,
                 const std::vector<std::string>& partition_by,
                 const std::vector<std::string>& order_by,
                 const std::vector<WindowColumn>& specs) {
    std::vector<std::uint32_t> pcols = col_indices(df, partition_by, "window");
    std::vector<std::uint32_t> ocols = col_indices(df, order_by, "window");
    std::vector<WindowSpec> specv;
    specv.reserve(specs.size());
    for (const WindowColumn& c : specs) {
        WindowSpec s;
        s.func = c.func;
        s.value_col = c.value ? col_index(df, *c.value, "window") : 0;
        s.time_col = c.time ? col_index(df, *c.time, "window") : 0;
        s.offset = c.offset;
        s.name = c.out;
        s.threshold = c.threshold;
        s.counter = c.counter;
        s.frame_preceding = c.preceding;
        s.frame_following = c.following;
        specv.push_back(std::move(s));
    }
    dataframe::OwnedArrow in = df.to_arrow();
    return import_result(
        arrow::window(in.schema(), in.array(), pcols.data(),
                      static_cast<std::uint32_t>(pcols.size()), ocols.data(),
                      static_cast<std::uint32_t>(ocols.size()), specv.data(),
                      static_cast<std::uint32_t>(specv.size())),
        "window");
}

DataFrame gap_fill(const DataFrame& df,
                   const std::vector<std::string>& partition_by,
                   const std::string& time, std::int64_t bucket,
                   const std::vector<std::string>& values, GapFillMode mode,
                   std::optional<std::pair<std::int64_t, std::int64_t>> range) {
    std::vector<std::uint32_t> pcols =
        col_indices(df, partition_by, "gap_fill");
    std::uint32_t tcol = col_index(df, time, "gap_fill");
    std::vector<std::uint32_t> vcols = col_indices(df, values, "gap_fill");
    dataframe::OwnedArrow in = df.to_arrow();
    return import_result(
        arrow::gap_fill(in.schema(), in.array(), pcols.data(),
                        static_cast<std::uint32_t>(pcols.size()), tcol, bucket,
                        vcols.data(), static_cast<std::uint32_t>(vcols.size()),
                        mode, range.has_value(), range ? range->first : 0,
                        range ? range->second : 0),
        "gap_fill");
}

DataFrame asof(const DataFrame& left, const DataFrame& right,
               const std::string& on, const std::vector<std::string>& by,
               AsofDirection direction, std::optional<std::int64_t> tolerance) {
    std::uint32_t lts = col_index(left, on, "asof");
    std::uint32_t rts = col_index(right, on, "asof");
    std::vector<std::uint32_t> lequi = col_indices(left, by, "asof");
    std::vector<std::uint32_t> requi = col_indices(right, by, "asof");
    dataframe::OwnedArrow l = left.to_arrow();
    dataframe::OwnedArrow r = right.to_arrow();
    return import_result(
        asof_join(l.schema(), l.array(), lts, lequi.data(), r.schema(),
                  r.array(), rts, requi.data(),
                  static_cast<std::uint32_t>(lequi.size()), direction,
                  tolerance.has_value(), tolerance.value_or(0)),
        "asof");
}

DataFrame interval(const DataFrame& left, const DataFrame& right,
                   const std::string& point, const std::string& lo,
                   const std::string& hi, const std::vector<std::string>& by,
                   bool outer) {
    std::uint32_t pcol = col_index(left, point, "interval");
    std::uint32_t lcol = col_index(right, lo, "interval");
    std::uint32_t hcol = col_index(right, hi, "interval");
    std::vector<std::uint32_t> lequi = col_indices(left, by, "interval");
    std::vector<std::uint32_t> requi = col_indices(right, by, "interval");
    dataframe::OwnedArrow l = left.to_arrow();
    dataframe::OwnedArrow r = right.to_arrow();
    return import_result(
        interval_join(l.schema(), l.array(), pcol, lequi.data(), r.schema(),
                      r.array(), lcol, hcol, requi.data(),
                      static_cast<std::uint32_t>(lequi.size()), outer),
        "interval");
}

namespace {

const dftu_op_desc FRAME_OPS[] = {
    {"dftu.frame.window",
     DFTU_OP_SIG8(FRAME, FRAME, STRLIST, STRLIST, WINLIST, NONE, NONE, NONE),
     reinterpret_cast<const void*>(&dftu_dataframe_window)},
    {"dftu.frame.gap_fill",
     DFTU_OP_SIG8(FRAME, FRAME, STRLIST, STR, I64, STRLIST, I32, I64LIST),
     reinterpret_cast<const void*>(&dftu_dataframe_gap_fill)},
    {"dftu.frame.asof",
     DFTU_OP_SIG8(FRAME, FRAME, FRAME, STR, STRLIST, I32, I64, NONE),
     reinterpret_cast<const void*>(&dftu_dataframe_asof)},
    {"dftu.frame.interval",
     DFTU_OP_SIG8(FRAME, FRAME, FRAME, STR, STR, STR, STRLIST, I32),
     reinterpret_cast<const void*>(&dftu_dataframe_interval)},
};

}  // namespace

void register_frame_ops() {
    static std::once_flag once;
    std::call_once(once, [] {
        for (const dftu_op_desc& op : FRAME_OPS) dftu_op_register(&op);
    });
}

}  // namespace dftracer::utils::utilities::common::arrow

namespace arr = dftracer::utils::utilities::common::arrow;
using dftracer::utils::dataframe::dataframe_handle_view;
using dftracer::utils::dataframe::dataframe_handle_wrap;

extern "C" {

dftu_dataframe* dftu_dataframe_window(
    const dftu_dataframe* df, const char* const* partition_by, int32_t n_part,
    const char* const* order_by, int32_t n_order, const dftu_window_spec* specs,
    int32_t n_specs) {
    if (!df || (n_part > 0 && !partition_by) || (n_order > 0 && !order_by) ||
        (n_specs > 0 && !specs))
        return nullptr;
    try {
        std::vector<arr::WindowColumn> cols;
        cols.reserve(static_cast<std::size_t>(n_specs));
        for (int32_t i = 0; i < n_specs; ++i) {
            const dftu_window_spec& s = specs[i];
            if (!arr::valid_window_func(s.func) || !s.out) return nullptr;
            arr::WindowColumn c;
            c.func = static_cast<arr::WindowFunc>(s.func);
            if (s.value) c.value = std::string(s.value);
            if (s.time) c.time = std::string(s.time);
            c.out = s.out;
            c.offset = s.offset;
            c.threshold = s.threshold;
            c.counter = s.counter != 0;
            c.preceding = s.preceding;
            c.following = s.following;
            cols.push_back(std::move(c));
        }
        return dataframe_handle_wrap(arr::window(
            dataframe_handle_view(df), arr::names_of(partition_by, n_part),
            arr::names_of(order_by, n_order), cols));
    } catch (const std::exception&) {
        return nullptr;
    }
}

dftu_dataframe* dftu_dataframe_gap_fill(
    const dftu_dataframe* df, const char* const* partition_by, int32_t n_part,
    const char* time, int64_t bucket, const char* const* values,
    int32_t n_values, dftu_gap_fill_mode mode, const int64_t* range,
    int32_t n_range) {
    if (!df || !time || (n_part > 0 && !partition_by) ||
        (n_values > 0 && !values) || (n_range != 0 && n_range != 2) ||
        (n_range == 2 && !range))
        return nullptr;
    if (mode != DFTU_GAP_FILL_NONE && mode != DFTU_GAP_FILL_LOCF &&
        mode != DFTU_GAP_FILL_LINEAR)
        return nullptr;
    try {
        std::optional<std::pair<int64_t, int64_t>> r;
        if (n_range == 2) r = std::make_pair(range[0], range[1]);
        return dataframe_handle_wrap(arr::gap_fill(
            dataframe_handle_view(df), arr::names_of(partition_by, n_part),
            time, bucket, arr::names_of(values, n_values),
            static_cast<arr::GapFillMode>(mode), r));
    } catch (const std::exception&) {
        return nullptr;
    }
}

dftu_dataframe* dftu_dataframe_asof(const dftu_dataframe* left,
                                    const dftu_dataframe* right, const char* on,
                                    const char* const* by, int32_t n_by,
                                    dftu_asof_direction direction,
                                    int64_t tolerance) {
    if (!left || !right || !on || (n_by > 0 && !by)) return nullptr;
    if (direction != DFTU_ASOF_BACKWARD && direction != DFTU_ASOF_FORWARD &&
        direction != DFTU_ASOF_NEAREST)
        return nullptr;
    try {
        std::optional<int64_t> tol;
        if (tolerance >= 0) tol = tolerance;
        return dataframe_handle_wrap(
            arr::asof(dataframe_handle_view(left), dataframe_handle_view(right),
                      on, arr::names_of(by, n_by),
                      static_cast<arr::AsofDirection>(direction), tol));
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
        return dataframe_handle_wrap(arr::interval(
            dataframe_handle_view(left), dataframe_handle_view(right), point,
            lo, hi, arr::names_of(by, n_by), outer != 0));
    } catch (const std::exception&) {
        return nullptr;
    }
}

}  // extern "C"

#endif  // DFTRACER_UTILS_ENABLE_ARROW
