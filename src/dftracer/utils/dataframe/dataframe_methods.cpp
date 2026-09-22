#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The dfops alias qualifies the batch_ops free-function calls below so they are
// not shadowed by the same-named DataFrame members.
namespace dfops = dftracer::utils::dataframe;

namespace {
// Mirrors LazyFrame's file-local default in lazyframe.cpp.
constexpr std::int64_t DEFAULT_MORSEL_ROWS = 65536;
}  // namespace

namespace dftracer::utils::dataframe {

std::int64_t DataFrame::column_index(std::string_view name) const {
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name) return static_cast<std::int64_t>(i);
    }
    return -1;
}

Series DataFrame::column(std::string_view name) const {
    const std::int64_t i = column_index(name);
    return i < 0 ? Series{} : columns[static_cast<std::size_t>(i)].share();
}

DataFrame DataFrame::take(const std::vector<std::int64_t>& indices) const {
    return dfops::take(*this, indices);
}
DataFrame DataFrame::filter(const Series& mask) const {
    return dfops::filter(*this, mask);
}
DataFrame DataFrame::slice(std::int64_t offset, std::int64_t len) const {
    return dfops::slice(*this, offset, len);
}
DataFrame DataFrame::head(std::int64_t n) const {
    return dfops::head(*this, n);
}
DataFrame DataFrame::tail(std::int64_t n) const {
    return dfops::tail(*this, n);
}

coro::AsyncGenerator<DataFrame> DataFrame::stream(
    std::int64_t morsel_rows) const {
    const std::int64_t eff_rows =
        morsel_rows > 0 ? morsel_rows : DEFAULT_MORSEL_ROWS;
    const std::int64_t nrows = num_rows();
    for (std::int64_t offset = 0; offset < nrows; offset += eff_rows) {
        DataFrame chunk = dfops::slice(*this, offset, eff_rows);
        co_yield std::move(chunk);
    }
}
DataFrame DataFrame::reverse() const { return dfops::reverse(*this); }
DataFrame DataFrame::sort_by(const std::string& name, bool descending) const {
    return dfops::sort_by(*this, name, descending);
}
DataFrame DataFrame::sort_by_multi(const std::vector<std::string>& by,
                                   bool descending) const {
    return dfops::sort_by_multi(*this, by, descending);
}
DataFrame DataFrame::sort_by_multi(const std::vector<std::string>& by,
                                   const std::vector<bool>& descending) const {
    return dfops::sort_by_multi(*this, by, descending);
}
DataFrame DataFrame::drop_nulls() const { return dfops::drop_nulls(*this); }
DataFrame DataFrame::fill_null(Scalar value) const {
    return dfops::fill_null(*this, value);
}
DataFrame DataFrame::unique(const std::vector<std::string>& subset) const {
    return dfops::unique(*this, subset);
}
DataFrame DataFrame::drop_duplicates(
    const std::vector<std::string>& subset) const {
    return dfops::unique(*this, subset);
}
DataFrame DataFrame::sample(std::int64_t n, std::uint64_t seed) const {
    return dfops::sample(*this, n, seed);
}
DataFrame DataFrame::with_row_index(const std::string& name) const {
    return dfops::with_row_index(*this, name);
}
DataFrame DataFrame::describe() const { return dfops::describe(*this); }
DataFrame DataFrame::null_count() const { return dfops::null_count(*this); }
DataFrame DataFrame::reduce(const std::vector<GroupAgg>& aggs) const {
    return dfops::reduce(*this, aggs);
}
DataFrame DataFrame::reduce(Agg agg) const { return dfops::reduce(*this, agg); }
Series DataFrame::is_duplicated() const { return dfops::is_duplicated(*this); }
Series DataFrame::is_unique() const { return dfops::is_unique(*this); }
Series DataFrame::partition_id(const std::vector<std::string>& keys,
                               std::int64_t n_parts) const {
    return dfops::partition_id(*this, keys, n_parts);
}

DataFrame Series::value_counts() const { return dfops::value_counts(*this); }
DataFrame DataFrame::topk(const std::string& name, std::int64_t k,
                          bool largest) const {
    return dfops::topk(*this, name, k, largest);
}

DataFrame DataFrame::select(const std::vector<std::string>& cols) const {
    return dfops::select(*this, cols);
}
DataFrame DataFrame::rename(const std::vector<std::string>& new_names) const {
    return dfops::rename(*this, new_names);
}
DataFrame DataFrame::with_column(const std::string& name,
                                 const Series& col) const {
    return dfops::with_column(*this, name, col);
}

DataFrame DataFrame::group_by(const std::string& key,
                              const std::vector<GroupAgg>& aggs) const {
    return dfops::group_by(*this, key, aggs);
}

DataFrame DataFrame::group_by(const std::vector<std::string>& keys,
                              const std::vector<GroupAgg>& aggs) const {
    return dfops::group_by(*this, keys, aggs);
}

GroupBy DataFrame::group_by(std::vector<std::string> keys) const {
    return GroupBy(*this, std::move(keys));
}

GroupBy::GroupBy(const DataFrame& frame, std::vector<std::string> keys)
    : keys_(std::move(keys)) {
    frame_.names = frame.names;
    frame_.columns.reserve(frame.columns.size());
    for (const Series& c : frame.columns) frame_.columns.push_back(c.share());
    for (const std::string& k : keys_)
        if (frame_.column_index(k) < 0)
            throw std::out_of_range("group_by: no column named " + k);
}

DataFrame GroupBy::agg(const std::vector<GroupAgg>& aggs) const {
    return dfops::group_by(frame_, keys_, aggs);
}

DataFrame GroupBy::reduce(Agg agg) const {
    return dfops::group_by(frame_, keys_,
                           dfops::reduce_specs(frame_, agg, keys_));
}

DataFrame GroupBy::size() const {
    return dfops::group_by(frame_, keys_, {GroupAgg{Agg::Count, "", "size"}});
}

DataFrame DataFrame::group_by(const Expr& key,
                              const std::vector<AggExprSpec>& aggs) const {
    const std::int64_t idx = expr_col_index(key);
    const std::string key_name =
        (idx >= 0 && static_cast<std::size_t>(idx) < names.size())
            ? names[static_cast<std::size_t>(idx)]
            : std::string("key");
    std::vector<const Series*> inputs;
    inputs.reserve(columns.size());
    for (const Series& c : columns) inputs.push_back(&c);
    return group_agg_expr(key, aggs, inputs, key_name);
}

DataFrame DataFrame::group_by(const std::vector<Expr>& keys,
                              const std::vector<AggExprSpec>& aggs) const {
    std::vector<std::string> key_names;
    key_names.reserve(keys.size());
    for (std::size_t k = 0; k < keys.size(); ++k) {
        const std::int64_t idx = expr_col_index(keys[k]);
        key_names.push_back(
            (idx >= 0 && static_cast<std::size_t>(idx) < names.size())
                ? names[static_cast<std::size_t>(idx)]
                : "key" + std::to_string(k));
    }
    std::vector<const Series*> inputs;
    inputs.reserve(columns.size());
    for (const Series& c : columns) inputs.push_back(&c);
    return group_agg_expr(keys, aggs, inputs, key_names);
}

DataFrame DataFrame::join(const DataFrame& other,
                          const std::vector<std::string>& left_on,
                          const std::vector<std::string>& right_on, JoinHow how,
                          const std::string& suffix) const {
    return dfops::join(*this, other, left_on, right_on, how, suffix);
}
DataFrame DataFrame::join(const DataFrame& other,
                          const std::vector<std::string>& on, JoinHow how,
                          const std::string& suffix) const {
    return dfops::join(*this, other, on, on, how, suffix);
}

DataFrame DataFrame::unpivot(const std::vector<std::string>& id_vars,
                             const std::vector<std::string>& value_vars) const {
    return dfops::unpivot(*this, id_vars, value_vars);
}
DataFrame DataFrame::melt(const std::vector<std::string>& id_vars,
                          const std::vector<std::string>& value_vars) const {
    return dfops::unpivot(*this, id_vars, value_vars);
}
DataFrame DataFrame::explode(const std::string& column) const {
    return dfops::explode(*this, column);
}
DataFrame DataFrame::compare_agg(const DataFrame& variant,
                                 std::int64_t n_key) const {
    return dfops::compare_agg(*this, variant, n_key);
}
DataFrame DataFrame::unnest(const std::string& column, bool keep_empty) const {
    return dfops::unnest(*this, column, keep_empty);
}
DataFrame DataFrame::to_dummies(const std::string& column) const {
    return dfops::to_dummies(*this, column);
}
DataFrame DataFrame::pivot(const std::string& index, const std::string& on,
                           const std::string& values,
                           const std::string& agg) const {
    return dfops::pivot(*this, index, on, values, agg);
}
DataFrame DataFrame::pivot(const std::string& index, const std::string& on,
                           const std::string& values, Agg agg) const {
    return dfops::pivot(*this, index, on, values, to_string(agg));
}
DataFrame DataFrame::group_by_dynamic(const std::string& time_col,
                                      std::int64_t every, std::int64_t period,
                                      const std::vector<GroupAgg>& aggs,
                                      std::int64_t origin,
                                      bool origin_min) const {
    return dfops::group_by_dynamic(*this, time_col, every, period, aggs, origin,
                                   origin_min);
}

}  // namespace dftracer::utils::dataframe
