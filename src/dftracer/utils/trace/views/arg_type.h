#ifndef DFTRACER_UTILS_TRACE_VIEWS_ARG_TYPE_H
#define DFTRACER_UTILS_TRACE_VIEWS_ARG_TYPE_H

#include <dftracer/utils/trace/views/fold_event.h>

#include <cstdint>
#include <type_traits>
#include <variant>

namespace dftracer::utils::trace::views::detail {

/// The column type a set of arg values needs: int64 while every value is one,
/// uint64 once a value is above int64 and none is negative, float64 once a
/// real (or a negative next to a uint64) appears, string when every value is
/// text, and JSON once text and numbers mix. `see` applies the rule value by
/// value in order.
struct ArgType {
    enum class Kind : std::uint8_t { Int, Uint, Dbl, Str, Json };
    Kind kind = Kind::Int;
    bool negative = false;
    bool number = false;

    void see(const FoldEvent::ArgValue& v) {
        if (kind == Kind::Json) return;
        const bool text = std::holds_alternative<std::uint32_t>(v);
        if (kind == Kind::Str) {
            if (!text) kind = Kind::Json;
            return;
        }
        if (text) {
            kind = number ? Kind::Json : Kind::Str;
            return;
        }
        number = true;
        if (std::holds_alternative<double>(v)) {
            kind = Kind::Dbl;
        } else if (const auto* i = std::get_if<std::int64_t>(&v)) {
            negative = negative || *i < 0;
            if (kind == Kind::Uint && negative) kind = Kind::Dbl;
        } else if (kind == Kind::Int) {
            kind = negative ? Kind::Dbl : Kind::Uint;
        }
    }
};

/// The same kind as ArgType::see gives, from the set of value alternatives
/// seen, whatever their order.
struct ArgKinds {
    static constexpr std::uint32_t DOUBLE_BIT = 1u << 0;
    static constexpr std::uint32_t INT_BIT = 1u << 1;
    static constexpr std::uint32_t TEXT_BIT = 1u << 2;
    static constexpr std::uint32_t UINT_BIT = 1u << 3;

    std::uint32_t mask = 0;
    bool negative = false;

    void add(const FoldEvent::ArgValue& v) {
        mask |= 1u << v.index();
        if (const auto* i = std::get_if<std::int64_t>(&v))
            negative = negative || *i < 0;
    }

    ArgType::Kind kind() const {
        using Kind = ArgType::Kind;
        const bool text = mask & TEXT_BIT;
        if (text) return mask & ~TEXT_BIT ? Kind::Json : Kind::Str;
        if (mask & DOUBLE_BIT) return Kind::Dbl;
        if (mask & UINT_BIT) return negative ? Kind::Dbl : Kind::Uint;
        return Kind::Int;
    }
};

static_assert(std::is_same_v<FoldEvent::ArgValue,
                             std::variant<double, std::int64_t, std::uint32_t,
                                          std::uint64_t>>);

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_ARG_TYPE_H
