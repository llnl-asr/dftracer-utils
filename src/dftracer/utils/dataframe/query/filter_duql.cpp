// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/query/filter_duql.h>
#include <dftracer/utils/dstd/ascii.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/duql/detail/string_literal.h>

#include <cmath>
#include <limits>

namespace dftracer::utils::dataframe::query {

namespace {

constexpr dstd::StringView RESERVED[] = {
    "and", "or",     "not",  "in",   "like",  "ilike", "between",
    "is",  "escape", "null", "true", "false", "from"};

bool plain_word(dstd::StringView name) {
    if (name.empty()) return false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        const bool letter =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        const bool digit = c >= '0' && c <= '9';
        if (!(letter || (digit && i > 0))) return false;
    }
    for (const dstd::StringView word : RESERVED)
        if (dstd::ascii_iequals(name, word)) return false;
    return true;
}

dstd::Optional<dstd::String> column_text(std::int32_t col,
                                         dstd::Span<const dstd::String> names) {
    if (col < 0 || static_cast<std::size_t>(col) >= names.size())
        return dstd::nullopt;
    const dstd::String& name = names[static_cast<std::size_t>(col)];
    if (plain_word(name)) return name;
    for (const char c : name) {
        const auto b = static_cast<unsigned char>(c);
        if (c == '`' || b < 0x20 || b == 0x7f) return dstd::nullopt;
    }
    if (name.empty()) return dstd::nullopt;
    return dstd::fmt<"`{}`">(name);
}

dstd::Optional<dstd::String> double_literal(double v) {
    if (!std::isfinite(v)) return dstd::nullopt;
    dstd::String text = dstd::double_text(v);
    if (text.empty()) return dstd::nullopt;
    if (text.find('.') == dstd::String::npos) {
        const std::size_t exponent = text.find('e');
        if (exponent == dstd::String::npos)
            text += ".0";
        else
            text.insert(exponent, ".0");
    }
    return text;
}

const char* cmp_text(CmpOp op) {
    switch (op) {
        case CmpOp::Gt:
            return ">";
        case CmpOp::Ge:
            return ">=";
        case CmpOp::Lt:
            return "<";
        case CmpOp::Le:
            return "<=";
        case CmpOp::Eq:
            return "==";
        case CmpOp::Ne:
            return "!=";
    }
    return nullptr;
}

dstd::Optional<dstd::String> scalar_literal(const Scalar& s) {
    switch (s.tag()) {
        case DFTU_SCALAR_TAG_I64:
            // duql reads `-9223372036854775808` as no number.
            if (s.i64() == std::numeric_limits<std::int64_t>::min())
                return dstd::nullopt;
            return dstd::int_text(s.i64());
        case DFTU_SCALAR_TAG_U64:
            // duql reads a literal above int64 as no number.
            if (s.u64() > static_cast<std::uint64_t>(
                              std::numeric_limits<std::int64_t>::max()))
                return dstd::nullopt;
            return dstd::int_text(s.u64());
        case DFTU_SCALAR_TAG_F64:
            return double_literal(s.f64());
        case DFTU_SCALAR_TAG_STR:
            return duql::detail::quote_string(s.str());
        default:
            return dstd::nullopt;
    }
}

dstd::Optional<dstd::String> list_literal(const Series& column) {
    const Series values =
        column.is_flat() ? column.share() : column.materialize();
    if (!values.valid() || values.null_count() != 0) return dstd::nullopt;
    dstd::String out = "[";
    const auto add = [&](dstd::StringView item) {
        if (out.size() > 1) out += ", ";
        out.append(item.data(), item.size());
    };
    switch (values.type()) {
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32: {
            const Series as64 = values.type() == TypeId::Int64
                                    ? values.share()
                                    : values.cast(TypeId::Int64);
            if (!as64.valid() || as64.null_count() != 0) return dstd::nullopt;
            for (std::int64_t i = 0; i < as64.length(); ++i) {
                const std::int64_t v = as64.data<std::int64_t>()[i];
                if (v == std::numeric_limits<std::int64_t>::min())
                    return dstd::nullopt;
                add(dstd::int_text(v));
            }
            break;
        }
        case TypeId::Float64: {
            for (std::int64_t i = 0; i < values.length(); ++i) {
                auto text = double_literal(values.data<double>()[i]);
                if (!text) return dstd::nullopt;
                add(*text);
            }
            break;
        }
        case TypeId::String:
            for (std::int64_t i = 0; i < values.length(); ++i)
                add(duql::detail::quote_string(values.string_at(i)));
            break;
        default:
            return dstd::nullopt;
    }
    out += ']';
    return out;
}

// `e` as a condition. `negated` is whether an odd number of `not` is above it:
// `in` is two-valued in the engine (a null cell is not in the list, so `not in`
// keeps it) and unknown in duql (so `not in` drops it), hence an `in` under a
// `not` is refused. Every other leaf is unknown on a null cell in both.
dstd::Optional<dstd::String> render(const Expr& e,
                                    dstd::Span<const dstd::String> names,
                                    bool negated) {
    std::int32_t col = -1;
    CmpOp cmp{};
    Scalar rhs;
    if (expr_as_col_cmp(e, &col, &cmp, &rhs)) {
        auto name = column_text(col, names);
        auto lit = scalar_literal(rhs);
        const char* op = cmp_text(cmp);
        if (!name || !lit || !op) return dstd::nullopt;
        return dstd::fmt<"{} {} {}">(*name, op, *lit);
    }
    LogicalOp lop{};
    Expr a, b;
    if (expr_as_logical(e, &lop, &a, &b)) {
        auto left = render(a, names, negated);
        auto right = render(b, names, negated);
        if (!left || !right) return dstd::nullopt;
        return dstd::fmt<"({} {} {})">(
            *left, lop == LogicalOp::And ? "and" : "or", *right);
    }
    if (expr_as_not(e, &a)) {
        auto inner = render(a, names, !negated);
        if (!inner) return dstd::nullopt;
        return dstd::fmt<"not ({})">(*inner);
    }
    Series values;
    if (expr_as_col_is_in(e, &col, &values)) {
        if (negated) return dstd::nullopt;
        auto name = column_text(col, names);
        auto list = list_literal(values);
        if (!name || !list) return dstd::nullopt;
        return dstd::fmt<"{} in {}">(*name, *list);
    }
    bool null = false;
    if (expr_as_col_is_null(e, &col, &null)) {
        auto name = column_text(col, names);
        if (!name) return dstd::nullopt;
        return dstd::fmt<"{} is {}null">(*name, null ? "" : "not ");
    }
    StrPredOp str{};
    dstd::StringView pattern;
    if (expr_as_col_str_pred(e, &col, &str, &pattern)) {
        auto name = column_text(col, names);
        if (!name) return dstd::nullopt;
        const dstd::String text = duql::detail::quote_string(pattern);
        switch (str) {
            case StrPredOp::Contains:
                return dstd::fmt<"contains({}, {})">(*name, text);
            case StrPredOp::StartsWith:
                return dstd::fmt<"starts_with({}, {})">(*name, text);
            case StrPredOp::EndsWith:
                return dstd::fmt<"ends_with({}, {})">(*name, text);
            case StrPredOp::Like:
                // The engine escapes with a backslash; duql has no escape
                // character unless the text names one.
                if (pattern.find('\\') != dstd::StringView::npos)
                    return dstd::fmt<"{} like {} escape \"\\\\\"">(*name, text);
                return dstd::fmt<"{} like {}">(*name, text);
            case StrPredOp::Matches:
            case StrPredOp::Search:
                return dstd::nullopt;
        }
    }
    return dstd::nullopt;
}

}  // namespace

dstd::Optional<dstd::String> expr_to_duql(
    const Expr& e, dstd::Span<const dstd::String> names) {
    return render(e, names, false);
}

DuqlFilters filters_to_duql(dstd::Span<const Expr> filters,
                            dstd::Span<const dstd::String> names) {
    DuqlFilters out;
    for (std::size_t i = 0; i < filters.size(); ++i) {
        auto text = render(filters[i], names, false);
        if (!text) continue;
        if (!out.text.empty()) out.text += " and ";
        out.text += *text;
        out.index.push_back(static_cast<std::int32_t>(i));
    }
    return out;
}

}  // namespace dftracer::utils::dataframe::query
