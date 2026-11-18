// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>
#include <string>

// The dataframe:: prefix qualifies the batch_ops free-function calls below so
// they are not shadowed by the same-named DataFrame members.

namespace {
// Mirrors LazyFrame's file-local default in lazyframe.cpp.
constexpr std::int64_t DEFAULT_BATCH_SIZE = 65536;
}  // namespace

namespace dftracer::utils::dataframe {

std::int64_t DataFrame::column_index(dstd::StringView name) const {
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name) return static_cast<std::int64_t>(i);
    }
    return -1;
}

Series DataFrame::column(dstd::StringView name) const {
    const std::int64_t i = column_index(name);
    return i < 0 ? Series{}
                 : columns[static_cast<std::size_t>(i)].rename(
                       names[static_cast<std::size_t>(i)]);
}

DataFrame DataFrame::take(dstd::Span<const std::int64_t> indices) const {
    return dataframe::take(*this, indices);
}
DataFrame DataFrame::filter(const Series& mask) const {
    return dataframe::filter(*this, mask);
}
DataFrame DataFrame::slice(std::int64_t offset, std::int64_t len) const {
    return dataframe::slice(*this, offset, len);
}
DataFrame DataFrame::head(std::int64_t n) const {
    return dataframe::head(*this, n);
}
DataFrame DataFrame::tail(std::int64_t n) const {
    return dataframe::tail(*this, n);
}

coro::AsyncGenerator<DataFrame> DataFrame::stream(
    std::int64_t batch_size) const {
    const std::int64_t eff_rows =
        batch_size > 0 ? batch_size : DEFAULT_BATCH_SIZE;
    const std::int64_t nrows = num_rows();
    for (std::int64_t offset = 0; offset < nrows; offset += eff_rows) {
        DataFrame chunk = dataframe::slice(*this, offset, eff_rows);
        co_yield std::move(chunk);
    }
}
DataFrame DataFrame::reverse() const { return dataframe::reverse(*this); }
dstd::Result<DataFrame> DataFrame::sort_by(dstd::StringView name,
                                           bool descending,
                                           bool nulls_last) const {
    return dataframe::sort_by(*this, name, descending, nulls_last);
}
dstd::Result<DataFrame> DataFrame::sort_by_multi(
    dstd::Span<const dstd::String> by, bool descending, bool nulls_last) const {
    return dataframe::sort_by_multi(*this, by, descending, nulls_last);
}
dstd::Result<DataFrame> DataFrame::sort_by_multi(
    dstd::Span<const dstd::String> by,
    dstd::Span<const std::uint8_t> descending,
    dstd::Span<const std::uint8_t> nulls_last) const {
    return dataframe::sort_by_multi(*this, by, descending, nulls_last);
}
DataFrame DataFrame::drop_nulls() const { return dataframe::drop_nulls(*this); }
DataFrame DataFrame::fill_null(Scalar value) const {
    return dataframe::fill_null(*this, value);
}
dstd::Result<DataFrame> DataFrame::unique(
    dstd::Span<const dstd::String> subset) const {
    return dataframe::unique(*this, subset);
}
dstd::Result<DataFrame> DataFrame::drop_duplicates(
    dstd::Span<const dstd::String> subset) const {
    return dataframe::unique(*this, subset);
}
DataFrame DataFrame::sample(std::int64_t n, std::uint64_t seed) const {
    return dataframe::sample(*this, n, seed);
}
DataFrame DataFrame::with_row_index(dstd::StringView name) const {
    return dataframe::with_row_index(*this, name);
}
DataFrame DataFrame::describe() const { return dataframe::describe(*this); }
DataFrame DataFrame::null_count() const { return dataframe::null_count(*this); }
dstd::Result<DataFrame> DataFrame::reduce(
    dstd::Span<const GroupAgg> aggs) const {
    return dataframe::reduce(*this, aggs);
}
dstd::Result<DataFrame> DataFrame::reduce(Agg agg) const {
    return dataframe::reduce(*this, agg);
}
dstd::Result<Series> DataFrame::is_duplicated() const {
    return dataframe::is_duplicated(*this);
}
dstd::Result<Series> DataFrame::is_unique() const {
    return dataframe::is_unique(*this);
}
dstd::Result<Series> DataFrame::partition_id(
    dstd::Span<const dstd::String> keys, std::int64_t n_parts) const {
    return dataframe::partition_id(*this, keys, n_parts);
}

dstd::Result<DataFrame> Series::value_counts() const {
    return dataframe::value_counts(*this);
}
dstd::Result<DataFrame> DataFrame::topk(dstd::StringView name, std::int64_t k,
                                        bool largest) const {
    return dataframe::topk(*this, name, k, largest);
}

dstd::Result<DataFrame> DataFrame::select(
    dstd::Span<const dstd::String> cols) const {
    return dataframe::select(*this, cols);
}
dstd::Result<DataFrame> DataFrame::rename(
    dstd::Span<const dstd::String> new_names) const {
    return dataframe::rename(*this, new_names);
}
DataFrame DataFrame::with_column(dstd::StringView name,
                                 const Series& col) const {
    return dataframe::with_column(*this, name, col);
}

dstd::Result<DataFrame> DataFrame::group_by(
    dstd::StringView key, dstd::Span<const GroupAgg> aggs) const {
    return dataframe::group_by(*this, key, aggs);
}

dstd::Result<DataFrame> DataFrame::group_by(
    dstd::Span<const dstd::String> keys,
    dstd::Span<const GroupAgg> aggs) const {
    return dataframe::group_by(*this, keys, aggs);
}

GroupBy DataFrame::group_by(dstd::Vector<dstd::String> keys) const {
    return GroupBy(*this, std::move(keys));
}

GroupBy::GroupBy(const DataFrame& frame, dstd::Vector<dstd::String> keys)
    : keys_(std::move(keys)) {
    frame_.names = frame.names;
    frame_.columns.reserve(frame.columns.size());
    for (const Series& c : frame.columns) frame_.columns.push_back(c.share());
}

dstd::Result<DataFrame> GroupBy::agg(dstd::Span<const GroupAgg> aggs) const {
    return dataframe::group_by(frame_, keys_, aggs);
}

dstd::Result<DataFrame> GroupBy::reduce(Agg agg) const {
    const dstd::Result<dstd::Vector<GroupAgg>> specs =
        dataframe::reduce_specs(frame_, agg, keys_);
    if (!specs) return specs.error();
    return dataframe::group_by(frame_, keys_, *specs);
}

dstd::Result<DataFrame> GroupBy::size() const {
    const GroupAgg size_agg{Agg::Count, "", "size"};
    return dataframe::group_by(frame_, keys_,
                               dstd::Span<const GroupAgg>(&size_agg, 1));
}

dstd::Result<DataFrame> DataFrame::group_by(
    const Expr& key, dstd::Span<const AggExprSpec> aggs) const {
    const std::int64_t idx = expr_col_index(key);
    const dstd::String key_name =
        (idx >= 0 && static_cast<std::size_t>(idx) < names.size())
            ? dstd::String(
                  dstd::StringView{names[static_cast<std::size_t>(idx)]})
            : dstd::String("key");
    dstd::Vector<const Series*> inputs;
    inputs.reserve(columns.size());
    for (const Series& c : columns) inputs.push_back(&c);
    return group_agg_expr(key, aggs, inputs, key_name);
}

dstd::Result<DataFrame> DataFrame::group_by(
    dstd::Span<const Expr> keys, dstd::Span<const AggExprSpec> aggs) const {
    dstd::Vector<dstd::String> key_names;
    key_names.reserve(keys.size());
    for (std::size_t k = 0; k < keys.size(); ++k) {
        const std::int64_t idx = expr_col_index(keys[k]);
        key_names.push_back(
            (idx >= 0 && static_cast<std::size_t>(idx) < names.size())
                ? dstd::String(
                      dstd::StringView{names[static_cast<std::size_t>(idx)]})
                : dstd::fmt<"key{}">(k));
    }
    dstd::Vector<const Series*> inputs;
    inputs.reserve(columns.size());
    for (const Series& c : columns) inputs.push_back(&c);
    return group_agg_expr(keys, aggs, inputs, key_names);
}

dstd::Result<DataFrame> DataFrame::join(const DataFrame& other,
                                        dstd::Span<const dstd::String> left_on,
                                        dstd::Span<const dstd::String> right_on,
                                        JoinHow how, dstd::StringView suffix,
                                        bool nulls_equal) const {
    return dataframe::join(*this, other, left_on, right_on, how, suffix,
                           nulls_equal);
}
dstd::Result<DataFrame> DataFrame::join(const DataFrame& other,
                                        dstd::Span<const dstd::String> on,
                                        JoinHow how, dstd::StringView suffix,
                                        bool nulls_equal) const {
    return dataframe::join(*this, other, on, on, how, suffix, nulls_equal);
}

dstd::Result<DataFrame> DataFrame::unpivot(
    dstd::Span<const dstd::String> id_vars,
    dstd::Span<const dstd::String> value_vars) const {
    return dataframe::unpivot(*this, id_vars, value_vars);
}
dstd::Result<DataFrame> DataFrame::melt(
    dstd::Span<const dstd::String> id_vars,
    dstd::Span<const dstd::String> value_vars) const {
    return dataframe::unpivot(*this, id_vars, value_vars);
}
dstd::Result<DataFrame> DataFrame::explode(dstd::StringView column) const {
    return dataframe::explode(*this, column);
}
dstd::Result<DataFrame> DataFrame::compare_agg(const DataFrame& variant,
                                               std::int64_t n_key) const {
    return dataframe::compare_agg(*this, variant, n_key);
}
dstd::Result<DataFrame> DataFrame::unnest(dstd::StringView column,
                                          bool keep_empty) const {
    return dataframe::unnest(*this, column, keep_empty);
}
dstd::Result<DataFrame> DataFrame::to_dummies(dstd::StringView column) const {
    return dataframe::to_dummies(*this, column);
}
dstd::Result<DataFrame> DataFrame::pivot(dstd::StringView index,
                                         dstd::StringView on,
                                         dstd::StringView values,
                                         dstd::StringView agg) const {
    return dataframe::pivot(*this, index, on, values, agg);
}
dstd::Result<DataFrame> DataFrame::pivot(dstd::StringView index,
                                         dstd::StringView on,
                                         dstd::StringView values,
                                         Agg agg) const {
    return dataframe::pivot(*this, index, on, values, to_string(agg));
}
dstd::Result<DataFrame> DataFrame::group_by_dynamic(
    dstd::StringView time_col, std::int64_t every, std::int64_t period,
    dstd::Span<const GroupAgg> aggs, std::int64_t origin,
    bool origin_min) const {
    return dataframe::group_by_dynamic(*this, time_col, every, period, aggs,
                                       origin, origin_min);
}

}  // namespace dftracer::utils::dataframe
