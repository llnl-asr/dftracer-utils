// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_QUERY_FILTER_DUQL_H
#define DFTRACER_UTILS_DATAFRAME_QUERY_FILTER_DUQL_H

#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>

namespace dftracer::utils::dataframe::query {

/// `e` as a duql condition, with `col(i)` named `names[i]` (in backticks when
/// the name is not a plain word). The subset duql says and the filters of a
/// scan use: a column against a number or a string (`> >= < <= == !=`),
/// `in` a list of integers, floats or strings, `is null` and `is not null`,
/// `contains`, `starts_with`, `ends_with` and `like` on a string column,
/// `and`, `or` and `not` over those. Nullopt for anything else, a column past
/// `names`, a NaN or infinite number, an unsigned literal above int64 (duql has
/// no such number), and a name no duql text can quote. An unsigned literal that
/// fits int64 reads back as the signed one: both mean the same value.
dstd::Optional<dstd::String> expr_to_duql(const Expr& e,
                                          dstd::Span<const dstd::String> names);

/// The filters that @ref expr_to_duql can say, as one condition and where each
/// came from.
struct DuqlFilters {
    /// The sayable filters joined by ` and `, every compound one in
    /// parentheses; empty when there is none.
    dstd::String text;
    /// The index in the input of each filter in `text`, in text order.
    dstd::Vector<std::int32_t> index;
};

DuqlFilters filters_to_duql(dstd::Span<const Expr> filters,
                            dstd::Span<const dstd::String> names);

}  // namespace dftracer::utils::dataframe::query

#endif  // DFTRACER_UTILS_DATAFRAME_QUERY_FILTER_DUQL_H
