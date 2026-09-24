#ifndef DFTRACER_UTILS_DATAFRAME_LAZY_OPS_H
#define DFTRACER_UTILS_DATAFRAME_LAZY_OPS_H

#include <dftracer/utils/dataframe/lazyframe.h>

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace dftracer::utils::dataframe {

/// The LazyFrame builder surface for a type that wraps a LazyFrame and wants
/// its chain to keep its own type (a TraceViewer stays a TraceViewer through
/// filter/select/sort). `Derived` provides `const LazyFrame& lazy() const` and
/// `Derived with_lazy(LazyFrame) const`. Builders whose result the wrapped
/// source can still own return `Derived`; the rest return a plain LazyFrame.
template <class Derived>
class LazyOps {
   public:
    Derived select(std::vector<std::string> names) const {
        return wrap(lf().select(std::move(names)));
    }
    Derived filter(Expr predicate) const {
        return wrap(lf().filter(std::move(predicate)));
    }
    Derived with_column(std::string name, Expr expr) const {
        return wrap(lf().with_column(std::move(name), std::move(expr)));
    }
    Derived rename(std::vector<std::string> names) const {
        return wrap(lf().rename(std::move(names)));
    }
    Derived slice(std::int64_t offset, std::int64_t len) const {
        return wrap(lf().slice(offset, len));
    }
    Derived head(std::int64_t n) const { return wrap(lf().head(n)); }
    Derived tail(std::int64_t n) const { return wrap(lf().tail(n)); }
    Derived topk(std::string name, std::int64_t k, bool largest = true) const {
        return wrap(lf().topk(std::move(name), k, largest));
    }
    Derived sort_by(std::string name, bool descending = false) const {
        return wrap(lf().sort_by(std::move(name), descending));
    }
    Derived sort_by_multi(std::vector<std::string> by,
                          bool descending = false) const {
        return wrap(lf().sort_by_multi(std::move(by), descending));
    }
    Derived sort_by_multi(std::vector<std::string> by,
                          std::vector<bool> descending) const {
        return wrap(lf().sort_by_multi(std::move(by), std::move(descending)));
    }
    Derived group_by(std::string key, std::vector<GroupAgg> aggs) const {
        return wrap(lf().group_by(std::move(key), std::move(aggs)));
    }
    Derived group_by(std::vector<std::string> keys, std::vector<GroupAgg> aggs,
                     std::vector<AggDynSpec> dyn = {},
                     std::string dyn_prefix = {}) const {
        return wrap(lf().group_by(std::move(keys), std::move(aggs),
                                  std::move(dyn), std::move(dyn_prefix)));
    }
    Derived group_by(Expr key, std::vector<AggExprSpec> aggs) const {
        return wrap(lf().group_by(std::move(key), std::move(aggs)));
    }
    Derived group_by(std::vector<Expr> keys,
                     std::vector<AggExprSpec> aggs) const {
        return wrap(lf().group_by(std::move(keys), std::move(aggs)));
    }
    Derived reduce(Agg agg) const { return wrap(lf().reduce(agg)); }
    Derived memory_budget(std::uint64_t bytes) const {
        return wrap(lf().memory_budget(bytes));
    }
    Derived auto_spill() const { return wrap(lf().auto_spill()); }

    LazyGroupBy group_by(std::vector<std::string> keys) const {
        return lf().group_by(std::move(keys));
    }
    LazyFrame drop_nulls() const { return lf().drop_nulls(); }
    LazyFrame fill_null(Scalar value) const { return lf().fill_null(value); }
    template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
    LazyFrame fill_null(T value) const {
        return lf().fill_null(value);
    }
    LazyFrame take(std::vector<std::int64_t> indices) const {
        return lf().take(std::move(indices));
    }
    LazyFrame filter_mask(Series mask) const {
        return lf().filter_mask(std::move(mask));
    }
    LazyFrame reverse() const { return lf().reverse(); }
    LazyFrame with_row_index(std::string name) const {
        return lf().with_row_index(std::move(name));
    }
    LazyFrame null_count() const { return lf().null_count(); }
    LazyFrame explode(std::string column) const {
        return lf().explode(std::move(column));
    }
    LazyFrame unnest(std::string column, bool keep_empty = false) const {
        return lf().unnest(std::move(column), keep_empty);
    }
    LazyFrame unpivot(std::vector<std::string> id_vars,
                      std::vector<std::string> value_vars) const {
        return lf().unpivot(std::move(id_vars), std::move(value_vars));
    }
    LazyFrame melt(std::vector<std::string> id_vars,
                   std::vector<std::string> value_vars) const {
        return lf().melt(std::move(id_vars), std::move(value_vars));
    }
    LazyFrame unique(std::vector<std::string> subset = {}) const {
        return lf().unique(std::move(subset));
    }
    LazyFrame drop_duplicates(std::vector<std::string> subset = {}) const {
        return lf().drop_duplicates(std::move(subset));
    }
    LazyFrame sample(std::int64_t n, std::uint64_t seed = 0) const {
        return lf().sample(n, seed);
    }
    LazyFrame is_duplicated() const { return lf().is_duplicated(); }
    LazyFrame is_unique() const { return lf().is_unique(); }
    LazyFrame group_by_dynamic(std::string time_col, std::int64_t every,
                               std::int64_t period, std::vector<GroupAgg> aggs,
                               std::int64_t origin = 0,
                               bool origin_min = false) const {
        return lf().group_by_dynamic(std::move(time_col), every, period,
                                     std::move(aggs), origin, origin_min);
    }
    LazyFrame join(LazyFrame other, std::vector<std::string> left_on,
                   std::vector<std::string> right_on,
                   JoinHow how = JoinHow::Inner,
                   std::string suffix = "_right") const {
        return lf().join(std::move(other), std::move(left_on),
                         std::move(right_on), how, std::move(suffix));
    }
    LazyFrame join(LazyFrame other, std::vector<std::string> on,
                   JoinHow how = JoinHow::Inner,
                   std::string suffix = "_right") const {
        return lf().join(std::move(other), std::move(on), how,
                         std::move(suffix));
    }
    LazyFrame concat(LazyFrame other) const {
        return lf().concat(std::move(other));
    }
    LazyFrame compare_agg(LazyFrame variant, std::int64_t n_key) const {
        return lf().compare_agg(std::move(variant), n_key);
    }
    LazyFrame frame_op(std::string name, OpArgs args,
                       std::vector<LazyFrame> others = {},
                       std::vector<std::string> out_names = {}) const {
        return lf().frame_op(std::move(name), std::move(args),
                             std::move(others), std::move(out_names));
    }
    LazyFrame pivot(std::string index, std::string on, std::string values,
                    std::string agg = "first") const {
        return lf().pivot(std::move(index), std::move(on), std::move(values),
                          std::move(agg));
    }
    LazyFrame to_dummies(std::string column) const {
        return lf().to_dummies(std::move(column));
    }
    LazyFrame describe() const { return lf().describe(); }
    LazyFrame op(std::string name, OpArgs args) const {
        return lf().op(std::move(name), std::move(args));
    }

    std::vector<std::string> schema() const { return lf().schema(); }
    Schema output_schema() const { return lf().output_schema(); }
    std::string explain() const { return lf().explain(); }
    coro::AsyncGenerator<DataFrame> stream(std::int64_t morsel_rows = 0) const {
        return lf().stream(morsel_rows);
    }
    std::unique_ptr<Cursor> open_cursor() const { return lf().open_cursor(); }
    coro::CoroTask<DataFrame> collect(std::int64_t morsel_rows = 0) const {
        return lf().collect(morsel_rows);
    }

    /// The wrapped plan, for passing this chain where a LazyFrame is taken.
    operator LazyFrame() const { return lf(); }

   protected:
    LazyOps() = default;

   private:
    const LazyFrame& lf() const {
        return static_cast<const Derived&>(*this).lazy();
    }
    Derived wrap(LazyFrame next) const {
        return static_cast<const Derived&>(*this).with_lazy(std::move(next));
    }
};

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_LAZY_OPS_H
