// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/string_reader.h>
#include <dftracer/utils/dataframe/kernels/string_ops.h>
#include <dftracer/utils/dataframe/mask.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/errc.h>
#include <dftracer/utils/duql/evaluator.h>
#include <dftracer/utils/duql/fields.h>
#include <dftracer/utils/duql/vectorize.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <variant>

namespace dftracer::utils::dataframe {

using namespace dftracer::utils::duql;

namespace {

// A frame has no record schema; its columns are named as a dftracer View
// collects them, with domain fields under `args.`.
constexpr bool FRAME_ARGS_FALLBACK = true;

[[noreturn]] void unsupported(dstd::StringView why) {
    throw dftracer::utils::common::DFTUtilsException(
        dftracer::utils::dstd::Error{DuqlErrc::Unsupported, why});
}

bool is_string(const Series& c) {
    return narrow_varwidth_type(c.type()) == TypeId::String;
}

bool is_bool(const Series& c) { return c.type() == TypeId::Bool; }

bool comparable(const Series& col, const LiteralValue& v) {
    if (is_string(col)) return std::holds_alternative<dstd::String>(v);
    if (is_bool(col)) return std::holds_alternative<bool>(v);
    return !std::holds_alternative<dstd::String>(v) &&
           !std::holds_alternative<bool>(v);
}

Series unknown(std::int64_t rows) { return Series::nulls(TypeId::Bool, rows); }

int cmp_code(CompareOp op) {
    switch (op) {
        case CompareOp::Gt:
            return DFTU_CMP_GT;
        case CompareOp::Ge:
            return DFTU_CMP_GE;
        case CompareOp::Lt:
            return DFTU_CMP_LT;
        case CompareOp::Le:
            return DFTU_CMP_LE;
        case CompareOp::Eq:
            return DFTU_CMP_EQ;
        case CompareOp::Ne:
            return DFTU_CMP_NE;
    }
    return DFTU_CMP_EQ;
}

dftu_scalar numeric_scalar(const LiteralValue& v) {
    dftu_scalar s{};
    if (const auto* pi = std::get_if<std::int64_t>(&v)) {
        s.kind = DFTU_SCALAR_TAG_I64;
        s.value.i = *pi;
    } else if (const auto* pu = std::get_if<std::uint64_t>(&v)) {
        s.kind = DFTU_SCALAR_TAG_U64;
        s.value.u = *pu;
    } else {
        s.kind = DFTU_SCALAR_TAG_F64;
        s.value.d = std::get<double>(v);
    }
    return s;
}

Series negate(Series s) { return Series{dftu_series_logical_not(s.handle())}; }

// A type mismatch is UNKNOWN on every row, as in the row evaluator.
Series compare_mask(const Series& col, CompareOp op, const LiteralValue& v) {
    if (!comparable(col, v)) return unknown(col.length());
    const bool eq = op == CompareOp::Eq || op == CompareOp::Ne;
    if (is_string(col)) {
        const dstd::String& lit = std::get<dstd::String>(v);
        dftu_scalar rhs{};
        rhs.kind = DFTU_SCALAR_TAG_STR;
        rhs.len = static_cast<std::uint32_t>(lit.size());
        rhs.value.s = lit.data();
        Series m{dftu_series_compare(
            col.handle(), static_cast<dftu_cmp_op>(cmp_code(op)), rhs)};
        if (!m.handle())
            unsupported("no comparison kernel for the column type");
        return m;
    }
    if (is_bool(col) && eq) {
        const bool want = (op == CompareOp::Eq) == std::get<bool>(v);
        return want ? col.share() : negate(col.share());
    }
    if (is_bool(col)) {
        return eval(expr_cmp_expr(static_cast<CmpOp>(cmp_code(op)), expr_col(0),
                                  expr_lit_bool(std::get<bool>(v))),
                    {&col});
    }
    Series m{dftu_series_compare(col.handle(),
                                 static_cast<dftu_cmp_op>(cmp_code(op)),
                                 numeric_scalar(v))};
    if (!m.handle()) unsupported("no comparison kernel for the column type");
    return m;
}

// Kleene OR of the equalities with the comparable elements; UNKNOWN when no
// element is comparable, as in the row evaluator.
Series in_mask(const Series& col, const duql::detail::ArrayNode& arr,
               bool negated) {
    if (is_string(col)) {
        dstd::HashSet<dstd::StringView> set;
        for (const LiteralNode& e : arr.elements)
            if (const auto* lit = std::get_if<dstd::String>(&e.value))
                set.insert(*lit);
        if (set.empty()) return unknown(col.length());
        Series m{string_mask(col.handle(), [&set](dstd::StringView s) {
            return set.contains(s);
        })};
        if (!m.handle())
            unsupported("no comparison kernel for the column type");
        return negated ? negate(std::move(m)) : std::move(m);
    }
    dstd::Optional<Series> acc;
    for (const LiteralNode& e : arr.elements) {
        if (!comparable(col, e.value)) continue;
        Series m = compare_mask(col, CompareOp::Eq, e.value);
        acc = acc ? Series{dftu_series_logical(acc->handle(), m.handle(),
                                               DFTU_LOGICAL_OR)}
                  : std::move(m);
    }
    if (!acc) return unknown(col.length());
    return negated ? negate(std::move(*acc)) : std::move(*acc);
}

template <class Leaf>
Series field_mask(const DataFrame& b, const duql::detail::FieldNode& field,
                  Leaf&& leaf) {
    if (field.any) unsupported("any() has no dataframe-mask lowering");
    const std::int64_t i = b.column_index(field.path);
    if (i < 0) return unknown(b.num_rows());
    return leaf(b.columns[static_cast<std::size_t>(i)]);
}

Series leaf_mask(const duql::detail::ExprLeaf& leaf, const DataFrame& b) {
    dstd::Vector<VectorColumn> cols;
    dstd::Vector<const Series*> inputs;
    cols.reserve(b.columns.size());
    inputs.reserve(b.columns.size());
    for (std::size_t i = 0; i < b.columns.size(); ++i) {
        cols.push_back(
            {dstd::String(b.names[i]), b.columns[i].data_type(), false});
        inputs.push_back(&b.columns[i]);
    }
    dstd::Vector<const TQuant*> quants;
    for_each_term(*leaf.term, [&](const Term& t) {
        if (const auto* q = std::get_if<TQuant>(&t.node)) quants.push_back(q);
    });
    dstd::Vector<Series> truths;
    truths.reserve(quants.size());
    for (const TQuant* q : quants) {
        auto t = quantify(*q, inputs, cols, FRAME_ARGS_FALLBACK);
        if (!t) unsupported(t.error().message);
        truths.push_back(std::move(*t));
    }
    for (std::size_t i = 0; i < quants.size(); ++i) {
        cols.push_back({"", scalar(TypeId::Bool), false, false, quants[i]});
        inputs.push_back(&truths[i]);
    }
    auto e = vectorize_condition(*leaf.term, cols, FRAME_ARGS_FALLBACK);
    if (!e) unsupported(e.error().message);
    return eval(*e, inputs);
}

Series lower(const duql::detail::QueryNode& node, const DataFrame& b) {
    return std::visit(
        [&](const auto& n) -> Series {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, duql::detail::CompareNode>) {
                return field_mask(b, n.field, [&](const Series& c) {
                    return compare_mask(c, n.op, n.value.value);
                });
            } else if constexpr (std::is_same_v<T, duql::detail::InNode>) {
                return field_mask(b, n.field, [&](const Series& c) {
                    return in_mask(c, n.values, false);
                });
            } else if constexpr (std::is_same_v<T, duql::detail::NotInNode>) {
                return field_mask(b, n.field, [&](const Series& c) {
                    return in_mask(c, n.values, true);
                });
            } else if constexpr (std::is_same_v<T, duql::detail::MatchNode>) {
                return field_mask(b, n.field, [&](const Series& c) {
                    if (!is_string(c) || !n.compiled)
                        return unknown(c.length());
                    Series m =
                        eval(expr_str_pattern(expr_col(0), n.compiled), {&c});
                    return n.negated ? negate(std::move(m)) : std::move(m);
                });
            } else if constexpr (std::is_same_v<T, duql::detail::AndNode>) {
                Series a = lower(*n.left, b), c = lower(*n.right, b);
                return Series{dftu_series_logical(a.handle(), c.handle(),
                                                  DFTU_LOGICAL_AND)};
            } else if constexpr (std::is_same_v<T, duql::detail::OrNode>) {
                Series a = lower(*n.left, b), c = lower(*n.right, b);
                return Series{dftu_series_logical(a.handle(), c.handle(),
                                                  DFTU_LOGICAL_OR)};
            } else if constexpr (std::is_same_v<T, duql::detail::NotNode>) {
                return negate(lower(*n.operand, b));
            } else {
                return leaf_mask(n, b);
            }
        },
        node.data);
}

}  // namespace

// An UNKNOWN row keeps its null and stores data 0, so a selection over the data
// bits keeps only TRUE rows. The data buffer is replaced, not written, because
// a leaf may share a caller's column.
Series evaluate_mask(const duql::detail::QueryNode& node,
                     const DataFrame& batch) {
    Series m = lower(node, batch);
    if (m.handle() && m.handle()->encoding != Encoding::Flat)
        m = Series{dftu_series_materialize(m.handle())};
    dftu_series* s = m.handle();
    if (!s || !s->validity.block()) return m;
    const std::size_t bytes = buffer_bytes(s->type, s->length);
    auto data = new_buffer(bytes);
    const std::uint8_t* pd = s->data.data();
    const std::uint8_t* pv = s->validity.data();
    std::uint8_t* po = writable(data);
    for (std::size_t i = 0; i < bytes; ++i)
        po[i] = static_cast<std::uint8_t>(pd[i] & pv[i]);
    s->data = std::move(data);
    return m;
}

}  // namespace dftracer::utils::dataframe
