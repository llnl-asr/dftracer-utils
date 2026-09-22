#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/op.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::dataframe {

namespace {

constexpr const char* ROW = "__dftu_row__";

std::int32_t index_of(const LazyFrame& plan, const std::string& name) {
    const std::vector<std::string> sch = plan.schema();
    auto it = std::find(sch.begin(), sch.end(), name);
    if (it == sch.end())
        throw std::out_of_range("group_by transform: no column named " + name);
    return static_cast<std::int32_t>(it - sch.begin());
}

Expr ref(const LazyFrame& plan, const std::string& name) {
    return expr_col(index_of(plan, name));
}

dftu_window_spec spec(dftu_window_func func, const char* value, const char* out,
                      std::int64_t offset = 0) {
    dftu_window_spec s{};
    s.func = func;
    s.value = value;
    s.time = nullptr;
    s.out = out;
    s.offset = offset;
    s.threshold = 0.0;
    s.counter = 0;
    s.preceding = DFTU_WINDOW_UNBOUNDED;
    s.following = DFTU_WINDOW_UNBOUNDED;
    return s;
}

LazyFrame window(const LazyFrame& plan, const std::vector<std::string>& part,
                 const std::vector<std::string>& order,
                 const std::vector<dftu_window_spec>& specs) {
    std::vector<const char*> pc;
    std::vector<const char*> oc;
    for (const std::string& s : part) pc.push_back(s.c_str());
    for (const std::string& s : order) oc.push_back(s.c_str());
    OpArgs a;
    a.strlist(1, pc.data(), static_cast<std::int32_t>(pc.size()))
        .strlist(2, oc.data(), static_cast<std::int32_t>(oc.size()))
        .winlist(3, specs);
    std::vector<std::string> names = plan.schema();
    for (const dftu_window_spec& s : specs) names.emplace_back(s.out);
    return plan.frame_op("dftu.frame.window", a, {}, std::move(names));
}

std::vector<std::string> value_columns(const LazyFrame& plan,
                                       const std::vector<std::string>& keys,
                                       Agg agg) {
    std::vector<std::string> out;
    for (const GroupAgg& g : plan.reduce_specs(agg, keys))
        out.push_back(g.column);
    if (out.empty())
        throw std::invalid_argument("group_by transform: no eligible columns");
    return out;
}

std::string tmp(const char* stem, std::size_t i) {
    return std::string("__dftu_") + stem + "_" + std::to_string(i) + "__";
}

Expr keep_null(const LazyFrame& p, const std::string& c,
               const std::string& out) {
    return ref(p, out) + (ref(p, c) - ref(p, c));
}

// One window column per value column, then the rows back in input order and
// `post` under the column's own name.
template <class Spec, class Post>
LazyFrame per_column(const LazyFrame& plan,
                     const std::vector<std::string>& keys,
                     const std::vector<std::string>& columns, Spec make_spec,
                     Post post) {
    LazyFrame out = plan.with_row_index(ROW);
    std::vector<std::string> outs;
    std::vector<dftu_window_spec> specs;
    for (std::size_t i = 0; i < columns.size(); ++i)
        outs.push_back(tmp("w", i));
    for (std::size_t i = 0; i < columns.size(); ++i)
        specs.push_back(make_spec(columns[i].c_str(), outs[i].c_str()));
    out = window(out, keys, {ROW}, specs).sort_by(ROW);
    for (std::size_t i = 0; i < columns.size(); ++i)
        out = out.with_column(columns[i], post(out, columns[i], outs[i]));
    return out.select(columns);
}

LazyFrame ranked(const LazyFrame& plan, const std::vector<std::string>& keys,
                 RankMethod method, bool ascending) {
    const std::vector<std::string> columns =
        value_columns(plan, keys, Agg::Sum);
    LazyFrame out = plan.with_row_index(ROW);
    for (std::size_t i = 0; i < columns.size(); ++i) {
        const std::string& c = columns[i];
        std::string key = c;
        if (!ascending) {
            key = tmp("neg", i);
            out = out.with_column(key, ref(out, c) * lit(std::int64_t{-1}));
        }
        const std::string rk = tmp("rk", i);
        Expr value = expr_col(0);
        switch (method) {
            // Ties share a rank only when the row index is not part of the
            // order, so Min and Dense order by the value alone.
            case RankMethod::Dense:
                out =
                    window(out, keys, {key},
                           {spec(DFTU_WINDOW_DENSE_RANK, nullptr, rk.c_str())});
                value = ref(out, rk);
                break;
            case RankMethod::Ordinal:
                out =
                    window(out, keys, {key, ROW},
                           {spec(DFTU_WINDOW_ROW_NUMBER, nullptr, rk.c_str())});
                value = ref(out, rk);
                break;
            case RankMethod::Min:
                out = window(out, keys, {key},
                             {spec(DFTU_WINDOW_RANK, nullptr, rk.c_str())});
                value = ref(out, rk);
                break;
            case RankMethod::Max:
            case RankMethod::Average: {
                // The max rank is the min rank in the opposite order, mirrored
                // through the group's count of present values; average is the
                // mean of the two.
                const std::string rev = tmp("rev", i);
                const std::string cnt = tmp("n", i);
                const std::string rk2 = tmp("rk2", i);
                out =
                    out.with_column(rev, ref(out, key) * lit(std::int64_t{-1}));
                out = window(
                    out, keys, {key},
                    {spec(DFTU_WINDOW_RANK, nullptr, rk.c_str()),
                     spec(DFTU_WINDOW_FRAME_COUNT, c.c_str(), cnt.c_str())});
                out = window(out, keys, {rev},
                             {spec(DFTU_WINDOW_RANK, nullptr, rk2.c_str())});
                Expr max_rank =
                    ref(out, cnt) + lit(std::int64_t{1}) - ref(out, rk2);
                value = method == RankMethod::Max
                            ? max_rank
                            : (ref(out, rk) + max_rank) / lit(std::int64_t{2});
                break;
            }
        }
        out = out.with_column(tmp("res", i), expr_cast(TypeId::Float64, value) +
                                                 (ref(out, c) - ref(out, c)));
    }
    out = out.sort_by(ROW);
    for (std::size_t i = 0; i < columns.size(); ++i)
        out = out.with_column(columns[i], ref(out, tmp("res", i)));
    return out.select(columns);
}

LazyFrame counter(const LazyFrame& plan, const std::vector<std::string>& part,
                  const std::vector<std::string>& order, dftu_window_func func,
                  const char* out_name) {
    const std::string w = "__dftu_rn__";
    LazyFrame out = plan.with_row_index(ROW);
    out =
        window(out, part, order, {spec(func, nullptr, w.c_str())}).sort_by(ROW);
    out = out.with_column(out_name, ref(out, w) - lit(std::int64_t{1}));
    return out.select({out_name});
}

// The nearest present value before (or, over the reversed row order, after)
// each row within its group.
LazyFrame filled(const LazyFrame& plan, const std::vector<std::string>& keys,
                 bool backward) {
    const std::vector<std::string> columns =
        value_columns(plan, keys, Agg::First);
    LazyFrame out = plan.with_row_index(ROW);
    std::string order = ROW;
    if (backward) {
        order = "__dftu_rev_row__";
        out = out.with_column(order, ref(out, ROW) * lit(std::int64_t{-1}));
    }
    std::vector<std::string> outs;
    std::vector<dftu_window_spec> specs;
    for (std::size_t i = 0; i < columns.size(); ++i)
        outs.push_back(tmp("w", i));
    for (std::size_t i = 0; i < columns.size(); ++i)
        specs.push_back(spec(DFTU_WINDOW_FILL_FORWARD, columns[i].c_str(),
                             outs[i].c_str()));
    out = window(out, keys, {order}, specs).sort_by(ROW);
    for (std::size_t i = 0; i < columns.size(); ++i)
        out = out.with_column(columns[i], ref(out, outs[i]));
    return out.select(columns);
}

// A trailing frame of `n` rows within the group, null until it holds `n`
// present values (the FRAME_* minimum count).
LazyFrame rolling(const LazyFrame& plan, const std::vector<std::string>& keys,
                  dftu_window_func func, std::int64_t n) {
    if (n < 1)
        throw std::invalid_argument("rolling: the window must be at least 1");
    return per_column(
        plan, keys, value_columns(plan, keys, Agg::Sum),
        [func, n](const char* c, const char* o) {
            dftu_window_spec s = spec(func, c, o, n);
            s.preceding = n - 1;
            s.following = 0;
            return s;
        },
        [](const LazyFrame& p, const std::string&, const std::string& o) {
            return ref(p, o);
        });
}

LazyFrame positional(const LazyFrame& plan,
                     const std::vector<std::string>& keys, CmpOp cmp,
                     std::int64_t rn, bool from_end) {
    const std::vector<std::string> input = plan.schema();
    LazyFrame out = plan.with_row_index(ROW);
    std::string order = ROW;
    if (from_end) {
        order = "__dftu_rev_row__";
        out = out.with_column(order, ref(out, ROW) * lit(std::int64_t{-1}));
    }
    const std::string w = "__dftu_rn__";
    out = window(out, keys, {order},
                 {spec(DFTU_WINDOW_ROW_NUMBER, nullptr, w.c_str())});
    out = out.filter(
                 expr_cmp(cmp, ref(out, w), Scalar{detail::expr_scalar_i(rn)}))
              .sort_by(ROW);
    return out.select(input);
}

}  // namespace

LazyFrame LazyGroupBy::transform(GroupwiseOp kind, std::int64_t n,
                                 RankMethod method, bool ascending) const {
    switch (kind) {
        case GroupwiseOp::CumSum:
            return per_column(
                plan_, keys_, value_columns(plan_, keys_, Agg::Sum),
                [](const char* c, const char* o) {
                    return spec(DFTU_WINDOW_RUNNING_SUM, c, o);
                },
                keep_null);
        case GroupwiseOp::CumProd:
            return per_column(
                plan_, keys_, value_columns(plan_, keys_, Agg::Sum),
                [](const char* c, const char* o) {
                    return spec(DFTU_WINDOW_RUNNING_PROD, c, o);
                },
                keep_null);
        case GroupwiseOp::CumMax:
            return per_column(
                plan_, keys_, value_columns(plan_, keys_, Agg::Max),
                [](const char* c, const char* o) {
                    return spec(DFTU_WINDOW_RUNNING_MAX, c, o);
                },
                keep_null);
        case GroupwiseOp::CumMin:
            return per_column(
                plan_, keys_, value_columns(plan_, keys_, Agg::Min),
                [](const char* c, const char* o) {
                    return spec(DFTU_WINDOW_RUNNING_MIN, c, o);
                },
                keep_null);
        case GroupwiseOp::CumCount:
            return counter(plan_, keys_, {ROW}, DFTU_WINDOW_ROW_NUMBER,
                           "cumcount");
        case GroupwiseOp::Shift: {
            const dftu_window_func func =
                n >= 0 ? DFTU_WINDOW_LAG : DFTU_WINDOW_LEAD;
            const std::int64_t offset = n >= 0 ? n : -n;
            return per_column(
                plan_, keys_, value_columns(plan_, keys_, Agg::First),
                [func, offset](const char* c, const char* o) {
                    return spec(func, c, o, offset);
                },
                [](const LazyFrame& p, const std::string&,
                   const std::string& o) { return ref(p, o); });
        }
        case GroupwiseOp::Diff:
            return per_column(
                plan_, keys_, value_columns(plan_, keys_, Agg::Sum),
                [](const char* c, const char* o) {
                    return spec(DFTU_WINDOW_DELTA, c, o);
                },
                [](const LazyFrame& p, const std::string&,
                   const std::string& o) { return ref(p, o); });
        case GroupwiseOp::PctChange:
            return per_column(
                plan_, keys_, value_columns(plan_, keys_, Agg::Sum),
                [](const char* c, const char* o) {
                    return spec(DFTU_WINDOW_LAG, c, o, 1);
                },
                [](const LazyFrame& p, const std::string& c,
                   const std::string& o) {
                    return (ref(p, c) - ref(p, o)) / ref(p, o);
                });
        case GroupwiseOp::Rank:
            return ranked(plan_, keys_, method, ascending);
        case GroupwiseOp::NGroup:
            if (keys_.empty())
                throw std::invalid_argument("ngroup: the group-by has no keys");
            return counter(plan_, {}, keys_, DFTU_WINDOW_DENSE_RANK, "ngroup");
        case GroupwiseOp::Head:
            return positional(plan_, keys_, CmpOp::Le, n, false);
        case GroupwiseOp::Tail:
            return positional(plan_, keys_, CmpOp::Le, n, true);
        case GroupwiseOp::Nth:
            return n >= 0 ? positional(plan_, keys_, CmpOp::Eq, n + 1, false)
                          : positional(plan_, keys_, CmpOp::Eq, -n, true);
        case GroupwiseOp::FFill:
            return filled(plan_, keys_, false);
        case GroupwiseOp::BFill:
            return filled(plan_, keys_, true);
        case GroupwiseOp::RollingSum:
            return rolling(plan_, keys_, DFTU_WINDOW_FRAME_SUM, n);
        case GroupwiseOp::RollingMean:
            return rolling(plan_, keys_, DFTU_WINDOW_FRAME_MEAN, n);
        case GroupwiseOp::RollingMin:
            return rolling(plan_, keys_, DFTU_WINDOW_FRAME_MIN, n);
        case GroupwiseOp::RollingMax:
            return rolling(plan_, keys_, DFTU_WINDOW_FRAME_MAX, n);
    }
    throw std::invalid_argument("group_by transform: unknown transform " +
                                std::to_string(static_cast<int>(kind)));
}

DataFrame GroupBy::transform(GroupwiseOp kind, std::int64_t n,
                             RankMethod method, bool ascending) const {
    return dftracer::utils::default_runtime()
        .submit(LazyGroupBy(frame_.lazy(), keys_)
                    .transform(kind, n, method, ascending)
                    .collect())
        .get();
}

}  // namespace dftracer::utils::dataframe
