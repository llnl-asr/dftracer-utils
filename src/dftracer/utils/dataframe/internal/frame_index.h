// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_FRAME_INDEX_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_FRAME_INDEX_H

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::dataframe {

/// What a frame op does to DataFrame::index_names, one row per frame op in
/// frame_op_index_rules.def.
enum class IndexRule : std::uint8_t { Keep, Rebuild, Drop };

namespace index_detail {

/// The rule row of op `name`, or -1.
constexpr int rule_row(dstd::StringView name) {
    int row = 0;
#define DFTU_FRAME_INDEX_RULE(op, rule)            \
    if (name == dstd::StringView(#op)) return row; \
    ++row;
#include <dftracer/utils/dataframe/frame_op_index_rules.def>
    return -1;
}

constexpr IndexRule rule_at(int row) {
    constexpr IndexRule RULES[] = {
#define DFTU_FRAME_INDEX_RULE(op, rule) IndexRule::rule,
#include <dftracer/utils/dataframe/frame_op_index_rules.def>
    };
    return RULES[row];
}

}  // namespace index_detail

/// The rule of the frame op named `op` (the name in exported_frame_ops.def
/// without its prefix); nullopt for a name with no rule.
dstd::Optional<IndexRule> index_rule_of(dstd::StringView op);

/// The rule of op `OP`, checked at compile time to exist.
template <dstd::FixedString OP>
constexpr IndexRule index_rule() {
    constexpr int ROW = index_detail::rule_row(dstd::StringView(OP.v));
    static_assert(ROW >= 0, "frame op has no row in frame_op_index_rules.def");
    return index_detail::rule_at(ROW);
}

/// Whether two index name lists hold the same names in the same order.
template <class A, class B>
bool index_same(dstd::Span<const A> a, dstd::Span<const B> b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (dstd::StringView(a[i]) != dstd::StringView(b[i])) return false;
    return true;
}

/// The join rule: a join keeps an index only when both sides are indexed on
/// exactly their join keys, in order.
template <class L, class R>
bool index_join_keeps(dstd::Span<const L> left_index,
                      dstd::Span<const R> right_index,
                      dstd::Span<const dstd::String> left_on,
                      dstd::Span<const dstd::String> right_on) {
    return !left_index.empty() && index_same(left_index, left_on) &&
           index_same(right_index, right_on);
}

/// The rules apply here and nowhere else. Each function sets `out.row_index`
/// from the operand `in` and the op's arguments, and replaces whatever `out`
/// carried. A frame with no index costs one null check.

/// Set `out`'s index to the names in `names` that are columns of `out`, in
/// order (the result of a plan whose index was tracked by name).
void apply_index_names(dstd::Span<const dstd::String> names, DataFrame& out);
/// Keep: the names of `in`'s index that are columns of `out`, in order.
void apply_index_keep(const DataFrame& in, DataFrame& out);
/// Drop: no index.
void apply_index_drop(DataFrame& out);
/// Drop for an op whose result still holds the columns of `in`: no index, and
/// the columns that were `in`'s index leave `out` with it.
void apply_index_drop(const DataFrame& in, DataFrame& out);
/// `in` without the columns of its index that are not in `keep`, and no
/// index: the operand of an op that drops the index.
DataFrame without_index_columns(const DataFrame& in,
                                dstd::Span<const dstd::String> keep);
/// `in` without the hidden default label columns (`__index_level_<i>__`) of
/// its index, and no index: the operand of an op that drops the index but
/// keeps the columns a user named, as `reset_index` does.
DataFrame without_hidden_index_columns(const DataFrame& in);
/// Rebuild by keys (group_by, group_by_dynamic, pivot): when `in`
/// is indexed, `keys` become the index of `out`.
void apply_index_keys(const DataFrame& in, dstd::Span<const dstd::String> keys,
                      DataFrame& out);
/// Rebuild for rename: the index columns of `in` under their new names, by
/// position.
void apply_index_renamed(const DataFrame& in, DataFrame& out);
/// Rebuild for join: `left_on` when both frames are indexed on exactly their
/// join keys, in order, else none.
void apply_index_joined(const DataFrame& left, const DataFrame& right,
                        dstd::Span<const dstd::String> left_on,
                        dstd::Span<const dstd::String> right_on,
                        DataFrame& out);
/// Rebuild for concat and union: the shared index names when every part has
/// the same ones and `out` holds them, else none.
void apply_index_concat(dstd::Span<const DataFrame* const> parts,
                        DataFrame& out);

template <dstd::FixedString OP>
void index_keep(const DataFrame& in, DataFrame& out) {
    static_assert(index_rule<OP>() == IndexRule::Keep);
    apply_index_keep(in, out);
}

template <dstd::FixedString OP>
void index_drop(DataFrame& out) {
    static_assert(index_rule<OP>() == IndexRule::Drop);
    apply_index_drop(out);
}

template <dstd::FixedString OP>
void index_drop(const DataFrame& in, DataFrame& out) {
    static_assert(index_rule<OP>() == IndexRule::Drop);
    apply_index_drop(in, out);
}

template <dstd::FixedString OP>
void index_keys(const DataFrame& in, dstd::Span<const dstd::String> keys,
                DataFrame& out) {
    static_assert(index_rule<OP>() == IndexRule::Rebuild);
    apply_index_keys(in, keys, out);
}

template <dstd::FixedString OP>
void index_renamed(const DataFrame& in, DataFrame& out) {
    static_assert(index_rule<OP>() == IndexRule::Rebuild);
    apply_index_renamed(in, out);
}

template <dstd::FixedString OP>
void index_joined(const DataFrame& left, const DataFrame& right,
                  dstd::Span<const dstd::String> left_on,
                  dstd::Span<const dstd::String> right_on, DataFrame& out) {
    static_assert(index_rule<OP>() == IndexRule::Rebuild);
    apply_index_joined(left, right, left_on, right_on, out);
}

template <dstd::FixedString OP>
void index_concat(dstd::Span<const DataFrame* const> parts, DataFrame& out) {
    static_assert(index_rule<OP>() == IndexRule::Rebuild);
    apply_index_concat(parts, out);
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_FRAME_INDEX_H
