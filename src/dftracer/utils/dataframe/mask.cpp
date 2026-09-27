#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/kernels/string_ops.h>
#include <dftracer/utils/dataframe/mask.h>
#include <dftracer/utils/duql/errc.h>
#include <dftracer/utils/duql/evaluator.h>
#include <dftracer/utils/duql/fields.h>
#include <dftracer/utils/duql/vectorize.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace dataframe = dftracer::utils::dataframe;

namespace dftracer::utils::dataframe {

using namespace dftracer::utils::duql;

namespace {

// A frame has no record schema; its columns are named as a dftracer View
// collects them, with domain fields under `args.`.
constexpr bool FRAME_ARGS_FALLBACK = true;

[[noreturn]] void unsupported(const std::string& why) {
    throw dftracer::utils::DFTUtilsException(
        dftracer::utils::make_error(DuqlErrc::Unsupported, why));
}

bool is_string(const dataframe::Series& c) {
    return dataframe::narrow_varwidth_type(c.type()) ==
           dataframe::TypeId::String;
}

bool is_bool(const dataframe::Series& c) {
    return c.type() == dataframe::TypeId::Bool;
}

bool comparable(const dataframe::Series& col, const LiteralValue& v) {
    if (is_string(col)) return std::holds_alternative<std::string>(v);
    if (is_bool(col)) return std::holds_alternative<bool>(v);
    return !std::holds_alternative<std::string>(v) &&
           !std::holds_alternative<bool>(v);
}

dataframe::Series unknown(std::int64_t rows) {
    return dataframe::Series::nulls(dataframe::TypeId::Bool, rows);
}

int cmp_code(CompareOp op) {
    switch (op) {
        case CompareOp::GT:
            return DFTU_CMP_GT;
        case CompareOp::GE:
            return DFTU_CMP_GE;
        case CompareOp::LT:
            return DFTU_CMP_LT;
        case CompareOp::LE:
            return DFTU_CMP_LE;
        case CompareOp::EQ:
            return DFTU_CMP_EQ;
        case CompareOp::NE:
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

dataframe::Series negate(dataframe::Series s) {
    return dataframe::Series{dftu_series_logical_not(s.handle())};
}

// A type mismatch is UNKNOWN on every row, as in the row evaluator.
dataframe::Series compare_mask(const dataframe::Series& col, CompareOp op,
                               const LiteralValue& v) {
    if (!comparable(col, v)) return unknown(col.length());
    const bool eq = op == CompareOp::EQ || op == CompareOp::NE;
    if (is_string(col) && eq) {
        dataframe::Series m = dataframe::str_eq(col, std::get<std::string>(v));
        return op == CompareOp::EQ ? std::move(m) : negate(std::move(m));
    }
    if (is_bool(col) && eq) {
        const bool want = (op == CompareOp::EQ) == std::get<bool>(v);
        return want ? col.share() : negate(col.share());
    }
    if (is_string(col) || is_bool(col)) {
        const Expr rhs = is_string(col) ? expr_lit_str(std::get<std::string>(v))
                                        : expr_lit_bool(std::get<bool>(v));
        return eval(
            expr_cmp_expr(static_cast<CmpOp>(cmp_code(op)), expr_col(0), rhs),
            {&col});
    }
    dataframe::Series m{dftu_series_compare(
        col.handle(), static_cast<dftu_cmp_op>(cmp_code(op)),
        numeric_scalar(v))};
    if (!m.handle()) unsupported("no comparison kernel for the column type");
    return m;
}

// Kleene OR of the equalities with the comparable elements; UNKNOWN when no
// element is comparable, as in the row evaluator.
dataframe::Series in_mask(const dataframe::Series& col, const ArrayNode& arr,
                          bool negated) {
    std::optional<dataframe::Series> acc;
    for (const LiteralNode& e : arr.elements) {
        if (!comparable(col, e.value)) continue;
        dataframe::Series m = compare_mask(col, CompareOp::EQ, e.value);
        acc = acc ? dataframe::Series{dftu_series_logical(
                        acc->handle(), m.handle(), DFTU_LOGICAL_OR)}
                  : std::move(m);
    }
    if (!acc) return unknown(col.length());
    return negated ? negate(std::move(*acc)) : std::move(*acc);
}

template <class Leaf>
dataframe::Series field_mask(const dataframe::DataFrame& b,
                             const FieldNode& field, Leaf&& leaf) {
    if (field.any) unsupported("any() has no dataframe-mask lowering");
    const std::int64_t i = b.column_index(field.path);
    if (i < 0) return unknown(b.num_rows());
    return leaf(b.columns[static_cast<std::size_t>(i)]);
}

dataframe::Series leaf_mask(const ExprLeaf& leaf,
                            const dataframe::DataFrame& b) {
    std::vector<VectorColumn> cols;
    std::vector<const dataframe::Series*> inputs;
    cols.reserve(b.columns.size());
    inputs.reserve(b.columns.size());
    for (std::size_t i = 0; i < b.columns.size(); ++i) {
        cols.push_back({b.names[i], b.columns[i].data_type(), false});
        inputs.push_back(&b.columns[i]);
    }
    std::vector<const TQuant*> quants;
    for_each_term(*leaf.term, [&](const Term& t) {
        if (const auto* q = std::get_if<TQuant>(&t.node)) quants.push_back(q);
    });
    std::vector<dataframe::Series> truths;
    truths.reserve(quants.size());
    for (const TQuant* q : quants) {
        auto t = quantify(*q, inputs, cols, FRAME_ARGS_FALLBACK);
        if (!t) unsupported(t.error().message);
        truths.push_back(std::move(*t));
    }
    for (std::size_t i = 0; i < quants.size(); ++i) {
        cols.push_back({"", dataframe::scalar(dataframe::TypeId::Bool), false,
                        false, quants[i]});
        inputs.push_back(&truths[i]);
    }
    auto e = vectorize_condition(*leaf.term, cols, FRAME_ARGS_FALLBACK);
    if (!e) unsupported(e.error().message);
    return dataframe::eval(*e, inputs);
}

dataframe::Series lower(const QueryNode& node, const dataframe::DataFrame& b) {
    return std::visit(
        [&](const auto& n) -> dataframe::Series {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, CompareNode>) {
                return field_mask(b, n.field, [&](const dataframe::Series& c) {
                    return compare_mask(c, n.op, n.value.value);
                });
            } else if constexpr (std::is_same_v<T, InNode>) {
                return field_mask(b, n.field, [&](const dataframe::Series& c) {
                    return in_mask(c, n.values, false);
                });
            } else if constexpr (std::is_same_v<T, NotInNode>) {
                return field_mask(b, n.field, [&](const dataframe::Series& c) {
                    return in_mask(c, n.values, true);
                });
            } else if constexpr (std::is_same_v<T, MatchNode>) {
                return field_mask(b, n.field, [&](const dataframe::Series& c) {
                    if (!is_string(c) || !n.compiled)
                        return unknown(c.length());
                    dataframe::Series m =
                        eval(expr_str_pattern(expr_col(0), n.compiled), {&c});
                    return n.negated ? negate(std::move(m)) : std::move(m);
                });
            } else if constexpr (std::is_same_v<T, AndNode>) {
                dataframe::Series a = lower(*n.left, b), c = lower(*n.right, b);
                return dataframe::Series{dftu_series_logical(
                    a.handle(), c.handle(), DFTU_LOGICAL_AND)};
            } else if constexpr (std::is_same_v<T, OrNode>) {
                dataframe::Series a = lower(*n.left, b), c = lower(*n.right, b);
                return dataframe::Series{dftu_series_logical(
                    a.handle(), c.handle(), DFTU_LOGICAL_OR)};
            } else if constexpr (std::is_same_v<T, NotNode>) {
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
dataframe::Series evaluate_mask(const QueryNode& node,
                                const dataframe::DataFrame& batch) {
    dataframe::Series m = lower(node, batch);
    if (m.handle() && m.handle()->encoding != dataframe::Encoding::Flat)
        m = dataframe::Series{dftu_series_materialize(m.handle())};
    dftu_series* s = m.handle();
    if (!s || !s->validity) return m;
    const std::size_t bytes = dataframe::buffer_bytes(s->type, s->length);
    auto data = dataframe::Buffer::allocate(bytes);
    const std::uint8_t* pd = s->data->data();
    const std::uint8_t* pv = s->validity->data();
    std::uint8_t* po = data->data();
    for (std::size_t i = 0; i < bytes; ++i)
        po[i] = static_cast<std::uint8_t>(pd[i] & pv[i]);
    s->data = std::move(data);
    return m;
}

}  // namespace dftracer::utils::dataframe
