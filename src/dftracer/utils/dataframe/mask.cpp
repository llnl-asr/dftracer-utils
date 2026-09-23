#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/kernels/string_ops.h>
#include <dftracer/utils/dataframe/mask.h>
#include <dftracer/utils/query/errc.h>

#include <cstdint>
#include <string>
#include <type_traits>
#include <variant>

namespace dataframe = dftracer::utils::dataframe;

namespace dftracer::utils::dataframe {

using namespace dftracer::utils::query;

namespace {

[[noreturn]] void unsupported(const std::string& why) {
    throw dftracer::utils::DFTUtilsException(
        dftracer::utils::make_error(QueryErrc::Unsupported, why));
}

std::int64_t column_index(const dataframe::DataFrame& b,
                          const std::string& name) {
    for (std::size_t i = 0; i < b.names.size(); ++i)
        if (b.names[i] == name) return static_cast<std::int64_t>(i);
    return -1;
}

const dataframe::Series& require_column(const dataframe::DataFrame& b,
                                        const std::string& path) {
    std::int64_t i = column_index(b, path);
    if (i < 0) unsupported("field not materialized: " + path);
    return b.columns[static_cast<std::size_t>(i)];
}

bool is_string(const dataframe::Series& c) {
    return dataframe::narrow_varwidth_type(c.type()) ==
           dataframe::TypeId::String;
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
    } else if (const auto* pd = std::get_if<double>(&v)) {
        s.kind = DFTU_SCALAR_TAG_F64;
        s.value.d = *pd;
    } else if (const auto* pb = std::get_if<bool>(&v)) {
        s.kind = DFTU_SCALAR_TAG_I64;
        s.value.i = *pb ? 1 : 0;
    } else {
        unsupported("numeric column compared to a string literal");
    }
    return s;
}

dataframe::Series compare_mask(const dataframe::Series& col, CompareOp op,
                               const LiteralValue& v) {
    if (is_string(col)) {
        const auto* sp = std::get_if<std::string>(&v);
        if (!sp) unsupported("string column compared to a non-string literal");
        if (op == CompareOp::EQ) return dataframe::str_eq(col, *sp);
        if (op == CompareOp::NE)
            return dataframe::Series{
                dftu_series_logical_not(dataframe::str_eq(col, *sp).handle())};
        unsupported("ordered comparison on a string column");
    }
    return dataframe::Series{dftu_series_compare(
        col.handle(), static_cast<dftu_cmp_op>(cmp_code(op)),
        numeric_scalar(v))};
}

dataframe::Series or_reduce_equals(const dataframe::Series& col,
                                   const ArrayNode& arr, bool negate) {
    if (arr.elements.empty()) unsupported("empty in-list");
    dataframe::Series acc =
        compare_mask(col, CompareOp::EQ, arr.elements[0].value);
    for (std::size_t i = 1; i < arr.elements.size(); ++i) {
        dataframe::Series e =
            compare_mask(col, CompareOp::EQ, arr.elements[i].value);
        acc = dataframe::Series{
            dftu_series_logical(acc.handle(), e.handle(), DFTU_LOGICAL_OR)};
    }
    if (negate) acc = dataframe::Series{dftu_series_logical_not(acc.handle())};
    return acc;
}

dataframe::Series lower(const QueryNode& node, const dataframe::DataFrame& b) {
    return std::visit(
        [&](const auto& n) -> dataframe::Series {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, CompareNode>) {
                return compare_mask(require_column(b, n.field.path), n.op,
                                    n.value.value);
            } else if constexpr (std::is_same_v<T, InNode>) {
                return or_reduce_equals(require_column(b, n.field.path),
                                        n.values, false);
            } else if constexpr (std::is_same_v<T, NotInNode>) {
                return or_reduce_equals(require_column(b, n.field.path),
                                        n.values, true);
            } else if constexpr (std::is_same_v<T, MatchNode>) {
                unsupported("pattern match has no dataframe-mask lowering");
            } else if constexpr (std::is_same_v<T, AndNode>) {
                dataframe::Series a = lower(*n.left, b), c = lower(*n.right, b);
                return dataframe::Series{dftu_series_logical(
                    a.handle(), c.handle(), DFTU_LOGICAL_AND)};
            } else if constexpr (std::is_same_v<T, OrNode>) {
                dataframe::Series a = lower(*n.left, b), c = lower(*n.right, b);
                return dataframe::Series{dftu_series_logical(
                    a.handle(), c.handle(), DFTU_LOGICAL_OR)};
            } else {  // NotNode
                return dataframe::Series{
                    dftu_series_logical_not(lower(*n.operand, b).handle())};
            }
        },
        node.data);
}

}  // namespace

dataframe::Series evaluate_mask(const QueryNode& node,
                                const dataframe::DataFrame& batch) {
    return lower(node, batch);
}

}  // namespace dftracer::utils::dataframe
