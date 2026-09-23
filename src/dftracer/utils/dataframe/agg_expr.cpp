#include <dftracer/utils/dataframe/agg_expr.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/expr_handle.h>

#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace dataframe = dftracer::utils::dataframe;

// The shared C ABI op codes (dftu_agg_op, used by both the dataframe agg C ABI
// and the plugin DFTU_SVC_AGG seam) must stay bit-identical to
// dataframe::AggOp, since both seams reinterpret the int as an AggOp.
#define DFTU_AGG_OP(id, code, name) \
    static_assert(code == static_cast<int>(dataframe::AggOp::id));
#include <dftracer/utils/dataframe/agg_ops.def>
#undef DFTU_AGG_OP

namespace dftracer::utils::dataframe {

AggExprSpec agg_count(std::string out) {
    return {AggOp::Count, Expr{}, std::move(out)};
}
AggExprSpec agg_sum(Expr value, std::string out) {
    return {AggOp::Sum, std::move(value), std::move(out)};
}
AggExprSpec agg_min(Expr value, std::string out) {
    return {AggOp::Min, std::move(value), std::move(out)};
}
AggExprSpec agg_max(Expr value, std::string out) {
    return {AggOp::Max, std::move(value), std::move(out)};
}
AggExprSpec agg_mean(Expr value, std::string out) {
    return {AggOp::Mean, std::move(value), std::move(out)};
}
AggExprSpec agg_var(Expr value, std::string out) {
    return {AggOp::Var, std::move(value), std::move(out)};
}
AggExprSpec agg_std(Expr value, std::string out) {
    return {AggOp::Std, std::move(value), std::move(out)};
}
AggExprSpec agg_skew(Expr value, std::string out) {
    return {AggOp::Skew, std::move(value), std::move(out)};
}
AggExprSpec agg_kurt(Expr value, std::string out) {
    return {AggOp::Kurt, std::move(value), std::move(out)};
}
AggExprSpec agg_pct(Expr value, double q, std::string out) {
    return {AggOp::Pct, std::move(value), std::move(out), q};
}
AggExprSpec agg_hist(Expr value, std::string out) {
    return {AggOp::Hist, std::move(value), std::move(out)};
}
AggExprSpec agg_sumsq(Expr value, std::string out) {
    return {AggOp::SumSq, std::move(value), std::move(out)};
}
AggExprSpec agg_argmax(Expr value, Expr by, std::string out) {
    AggExprSpec s;
    s.op = AggOp::ArgMax;
    s.value = std::move(value);
    s.out = std::move(out);
    s.by = std::move(by);
    return s;
}
AggExprSpec agg_set_union(Expr value, std::string out) {
    return {AggOp::SetUnion, std::move(value), std::move(out)};
}

DataFrame group_agg_expr(const std::vector<Expr>& keys,
                         const std::vector<AggExprSpec>& specs,
                         const std::vector<const Series*>& inputs,
                         const std::vector<std::string>& key_names) {
    // The value expressions (all numeric) compile into one program so a shared
    // subexpression is computed once (CSE); deduping identical value nodes maps
    // them to one evaluated column, folded into a single shared FieldStat.
    std::vector<Expr> value_roots;
    std::unordered_map<const ExprNode*, std::int32_t> value_col;
    auto dedup = [&](const Expr& e) -> std::int32_t {
        const ExprNode* np = e.node().get();
        auto it = value_col.find(np);
        if (it != value_col.end()) return it->second;
        const std::int32_t idx = static_cast<std::int32_t>(value_roots.size());
        value_roots.push_back(e);
        value_col.emplace(np, idx);
        return idx;
    };
    // The ops agg_uses_raw_value() names read their value as a String repr, so
    // it is String-typed in the common case; the numeric expr evaluator cannot
    // produce a String column, so a bare column reference bypasses it and
    // shares the input column directly (mirroring the key's dodge below). Only
    // a bare ref is supported for them - a computed expression is not.
    std::vector<std::int32_t> raw_cols;  // input column indices
    auto raw_col = [&](const Expr& e) -> std::int32_t {
        const std::int32_t ci = expr_col_index(e);
        if (ci < 0)
            throw std::invalid_argument(
                "group_agg_expr: this aggregate's value must be a bare column "
                "reference");
        raw_cols.push_back(ci);
        return static_cast<std::int32_t>(raw_cols.size() - 1);
    };
    std::vector<AggSpec> col_specs;
    col_specs.reserve(specs.size());
    for (const AggExprSpec& sp : specs) {
        AggSpec cs;
        cs.op = sp.op;
        cs.out = sp.out;
        cs.param = sp.param;
        if (sp.op == AggOp::Count || !sp.value.valid()) {
            cs.value_col = -1;
        } else if (agg_uses_raw_value(sp.op)) {
            cs.value_col = raw_col(sp.value);
        } else {
            cs.value_col = dedup(sp.value);
        }
        // The agg_uses_by_col() ops read a second column via by_col; leaving
        // it -1 would index values[SIZE_MAX] at accumulate.
        if (agg_uses_by_col(sp.op)) {
            if (!sp.by.valid())
                throw std::invalid_argument(
                    "group_agg_expr: this aggregate requires a `by` column");
            cs.by_col = dedup(sp.by);
        }
        col_specs.push_back(std::move(cs));
    }

    // A bare column reference is taken directly, any type (a String counter arg
    // aggregates like the eager group_by: numeric reducers skip it, first/last/
    // count still work). Only genuinely computed values run through the numeric
    // evaluator, which has no String kernel and would yield an invalid column.
    std::vector<Series> value_cols(value_roots.size());
    std::vector<Expr> computed;
    std::vector<std::size_t> computed_pos;
    for (std::size_t i = 0; i < value_roots.size(); ++i) {
        const std::int32_t ci = expr_col_index(value_roots[i]);
        if (ci >= 0) {
            value_cols[i] = inputs[static_cast<std::size_t>(ci)]->share();
        } else {
            computed.push_back(value_roots[i]);
            computed_pos.push_back(i);
        }
    }
    if (!computed.empty()) {
        std::vector<Series> ev = eval_many(computed, inputs);
        for (std::size_t j = 0; j < ev.size(); ++j)
            value_cols[computed_pos[j]] = std::move(ev[j]);
    }
    // raw_col indices are 0-based within raw_cols; offset them past the
    // value_cols outputs once both column counts are known.
    const std::int32_t raw_base = static_cast<std::int32_t>(value_cols.size());
    for (AggSpec& cs : col_specs)
        if (agg_uses_raw_value(cs.op)) cs.value_col += raw_base;

    std::vector<const Series*> values;
    values.reserve(value_cols.size() + raw_cols.size());
    for (Series& c : value_cols) {
        if (!c.valid())
            throw std::invalid_argument(
                "group_agg: an aggregate value expression is not a numeric "
                "column (no arithmetic kernel for its type)");
        values.push_back(&c);
    }
    for (std::int32_t ci : raw_cols)
        values.push_back(inputs[static_cast<std::size_t>(ci)]);

    // Each key may be any type (e.g. a string category): a bare column
    // reference shares that input column directly; only a computed key routes
    // through the evaluator.
    std::vector<Series> key_cols;
    key_cols.reserve(keys.size());
    for (const Expr& key : keys) {
        const std::int32_t ki = expr_col_index(key);
        key_cols.push_back(ki >= 0
                               ? inputs[static_cast<std::size_t>(ki)]->share()
                               : eval(key, inputs));
    }
    std::vector<const Series*> key_ptrs;
    key_ptrs.reserve(key_cols.size());
    for (const Series& c : key_cols) key_ptrs.push_back(&c);
    return group_agg(key_ptrs, values, std::move(col_specs), key_names);
}

DataFrame group_agg_expr(const Expr& key, const std::vector<AggExprSpec>& specs,
                         const std::vector<const Series*>& inputs,
                         const std::string& key_name) {
    return group_agg_expr(std::vector<Expr>{key}, specs, inputs,
                          std::vector<std::string>{key_name});
}

}  // namespace dftracer::utils::dataframe

namespace {
dftu_agg_spec make_spec(int32_t op, const dftu_expr* value, const char* out,
                        double param = 0.0, const dftu_expr* by = nullptr) {
    return {op, value, out, param, by};
}
}  // namespace

extern "C" {

dftu_agg_spec dftu_agg_count(const char* out) {
    return make_spec(DFTU_AGG_COUNT, nullptr, out);
}
dftu_agg_spec dftu_agg_sum(const dftu_expr* value, const char* out) {
    return make_spec(DFTU_AGG_SUM, value, out);
}
dftu_agg_spec dftu_agg_min(const dftu_expr* value, const char* out) {
    return make_spec(DFTU_AGG_MIN, value, out);
}
dftu_agg_spec dftu_agg_max(const dftu_expr* value, const char* out) {
    return make_spec(DFTU_AGG_MAX, value, out);
}
dftu_agg_spec dftu_agg_mean(const dftu_expr* value, const char* out) {
    return make_spec(DFTU_AGG_MEAN, value, out);
}
dftu_agg_spec dftu_agg_var(const dftu_expr* value, const char* out) {
    return make_spec(DFTU_AGG_VAR, value, out);
}
dftu_agg_spec dftu_agg_std(const dftu_expr* value, const char* out) {
    return make_spec(DFTU_AGG_STD, value, out);
}
dftu_agg_spec dftu_agg_skew(const dftu_expr* value, const char* out) {
    return make_spec(DFTU_AGG_SKEW, value, out);
}
dftu_agg_spec dftu_agg_kurt(const dftu_expr* value, const char* out) {
    return make_spec(DFTU_AGG_KURT, value, out);
}
dftu_agg_spec dftu_agg_first(const dftu_expr* value, const char* out) {
    return make_spec(DFTU_AGG_FIRST, value, out);
}
dftu_agg_spec dftu_agg_last(const dftu_expr* value, const char* out) {
    return make_spec(DFTU_AGG_LAST, value, out);
}
dftu_agg_spec dftu_agg_pct(const dftu_expr* value, double q, const char* out) {
    return make_spec(DFTU_AGG_PCT, value, out, q);
}
dftu_agg_spec dftu_agg_hist(const dftu_expr* value, const char* out) {
    return make_spec(DFTU_AGG_HIST, value, out);
}
dftu_agg_spec dftu_agg_sumsq(const dftu_expr* value, const char* out) {
    return make_spec(DFTU_AGG_SUMSQ, value, out);
}
dftu_agg_spec dftu_agg_argmax(const dftu_expr* value, const dftu_expr* by,
                              const char* out) {
    return make_spec(DFTU_AGG_ARGMAX, value, out, 0.0, by);
}
dftu_agg_spec dftu_agg_set_union(const dftu_expr* value, const char* out) {
    return make_spec(DFTU_AGG_SET_UNION, value, out);
}

int32_t dftu_dataframe_group_agg_expr(const dftu_expr* key,
                                      const dftu_agg_spec* specs,
                                      int32_t n_specs,
                                      const dftu_series* const* inputs,
                                      int32_t n_inputs, dftu_series** out_key,
                                      dftu_series** out_values) {
    if (!key || !specs || n_specs <= 0) return -1;
    std::vector<dataframe::AggExprSpec> cxx;
    cxx.reserve(static_cast<std::size_t>(n_specs));
    for (int32_t i = 0; i < n_specs; ++i) {
        dataframe::AggExprSpec s;
        s.op = static_cast<dataframe::AggOp>(specs[i].op);
        if (specs[i].value)
            s.value = dataframe::expr_handle_unwrap(specs[i].value);
        s.out = specs[i].out ? specs[i].out : "";
        s.param = specs[i].param;
        if (specs[i].by) s.by = dataframe::expr_handle_unwrap(specs[i].by);
        cxx.push_back(std::move(s));
    }
    std::vector<dataframe::Series> owned;
    std::vector<const dataframe::Series*> cols;
    owned.reserve(static_cast<std::size_t>(n_inputs));
    cols.reserve(static_cast<std::size_t>(n_inputs));
    for (int32_t i = 0; i < n_inputs; ++i) {
        owned.emplace_back(const_cast<dftu_series*>(inputs[i]));
        cols.push_back(&owned.back());
    }
    int32_t written = -1;
    try {
        dataframe::DataFrame out = dataframe::group_agg_expr(
            dataframe::expr_handle_unwrap(key), cxx, cols, "key");
        *out_key = out.columns[0].release();
        for (std::size_t i = 1; i < out.columns.size(); ++i)
            out_values[i - 1] = out.columns[i].release();
        written = n_specs;
    } catch (const std::exception&) {
        written = -1;
    }
    for (dataframe::Series& c : owned)
        c.release();  // borrowed inputs, do not free
    return written;
}
}
