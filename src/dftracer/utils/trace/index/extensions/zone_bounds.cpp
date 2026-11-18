// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/trace/index/extensions/zone_bounds.h>

#include <cmath>
#include <cstring>
#include <limits>

namespace dftracer::utils::trace::index::extensions {

namespace {

constexpr auto INT64_MAX_U =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

int compare_bytes(dstd::StringView a, dstd::StringView b) {
    const std::size_t n = a.size() < b.size() ? a.size() : b.size();
    const int c = n == 0 ? 0 : std::memcmp(a.data(), b.data(), n);
    if (c != 0) return c < 0 ? -1 : 1;
    return a.size() < b.size() ? -1 : (a.size() > b.size() ? 1 : 0);
}

// Whether the integer reads back from the double unchanged.
template <class I>
bool holds_exactly(I v, double d) {
    return duql::compare_numbers(v, d).value_or(1) == 0;
}

}  // namespace

ZoneLiteral ZoneLiteral::from_number(duql::Number n) {
    ZoneLiteral out;
    out.number = n;
    return out;
}

ZoneLiteral ZoneLiteral::from_text(dstd::String s) {
    ZoneLiteral out;
    out.number = duql::parse_number(s);
    out.text = std::move(s);
    return out;
}

void ZoneBounds::make_mixed() {
    type_ = BoundType::Mixed;
    min_str_.clear();
    max_str_.clear();
    min_exact_ = true;
    max_exact_ = true;
}

void ZoneBounds::to_double() {
    if (type_ == BoundType::Int64) {
        const auto lo = min_.i;
        const auto hi = max_.i;
        min_.d = static_cast<double>(lo);
        max_.d = static_cast<double>(hi);
        min_exact_ = min_exact_ && holds_exactly(lo, min_.d);
        max_exact_ = max_exact_ && holds_exactly(hi, max_.d);
    } else if (type_ == BoundType::UInt64) {
        const auto lo = min_.u;
        const auto hi = max_.u;
        min_.d = static_cast<double>(lo);
        max_.d = static_cast<double>(hi);
        min_exact_ = min_exact_ && holds_exactly(lo, min_.d);
        max_exact_ = max_exact_ && holds_exactly(hi, max_.d);
    }
    type_ = BoundType::Double;
}

void ZoneBounds::add_double(double value, bool lossy) {
    if (value < min_.d) {
        min_.d = value;
        min_exact_ = !lossy;
    } else if (value == min_.d && lossy) {
        min_exact_ = false;
    }
    if (value > max_.d) {
        max_.d = value;
        max_exact_ = !lossy;
    } else if (value == max_.d && lossy) {
        max_exact_ = false;
    }
}

void ZoneBounds::add(std::int64_t value) {
    ++values_;
    switch (type_) {
        case BoundType::None:
            type_ = BoundType::Int64;
            min_.i = max_.i = value;
            return;
        case BoundType::Int64:
            if (value < min_.i) min_.i = value;
            if (value > max_.i) max_.i = value;
            return;
        case BoundType::UInt64:
            if (value >= 0) {
                const auto u = static_cast<std::uint64_t>(value);
                if (u < min_.u) min_.u = u;
                if (u > max_.u) max_.u = u;
                return;
            }
            if (max_.u <= INT64_MAX_U) {
                min_.i = static_cast<std::int64_t>(min_.u);
                max_.i = static_cast<std::int64_t>(max_.u);
                type_ = BoundType::Int64;
                if (value < min_.i) min_.i = value;
                return;
            }
            to_double();
            [[fallthrough]];
        case BoundType::Double: {
            const auto d = static_cast<double>(value);
            add_double(d, !holds_exactly(value, d));
            return;
        }
        case BoundType::String:
            make_mixed();
            return;
        case BoundType::Mixed:
            return;
    }
}

void ZoneBounds::add(std::uint64_t value) {
    ++values_;
    switch (type_) {
        case BoundType::None:
            type_ = BoundType::UInt64;
            min_.u = max_.u = value;
            return;
        case BoundType::UInt64:
            if (value < min_.u) min_.u = value;
            if (value > max_.u) max_.u = value;
            return;
        case BoundType::Int64:
            if (value <= INT64_MAX_U) {
                const auto i = static_cast<std::int64_t>(value);
                if (i < min_.i) min_.i = i;
                if (i > max_.i) max_.i = i;
                return;
            }
            to_double();
            [[fallthrough]];
        case BoundType::Double: {
            const auto d = static_cast<double>(value);
            add_double(d, !holds_exactly(value, d));
            return;
        }
        case BoundType::String:
            make_mixed();
            return;
        case BoundType::Mixed:
            return;
    }
}

void ZoneBounds::add(double value) {
    if (std::isnan(value)) {
        ++nans_;
        return;
    }
    ++values_;
    switch (type_) {
        case BoundType::None:
            type_ = BoundType::Double;
            min_.d = max_.d = value;
            return;
        case BoundType::Int64:
        case BoundType::UInt64:
            to_double();
            [[fallthrough]];
        case BoundType::Double:
            add_double(value, false);
            return;
        case BoundType::String:
            make_mixed();
            return;
        case BoundType::Mixed:
            return;
    }
}

void ZoneBounds::add(dstd::StringView value) {
    ++values_;
    switch (type_) {
        case BoundType::None:
            type_ = BoundType::String;
            min_str_.assign(value.data(), value.size());
            max_str_ = min_str_;
            return;
        case BoundType::String:
            if (compare_bytes(value, min_str_) < 0)
                min_str_.assign(value.data(), value.size());
            if (compare_bytes(value, max_str_) > 0)
                max_str_.assign(value.data(), value.size());
            return;
        case BoundType::Int64:
        case BoundType::UInt64:
        case BoundType::Double:
            make_mixed();
            return;
        case BoundType::Mixed:
            return;
    }
}

void ZoneBounds::merge_from(const ZoneBounds& other) {
    const std::uint64_t values = values_ + other.values_;
    nulls_ += other.nulls_;
    nans_ += other.nans_;
    switch (other.type_) {
        case BoundType::None:
            break;
        case BoundType::Mixed:
            make_mixed();
            break;
        case BoundType::Int64:
            add(other.min_.i);
            add(other.max_.i);
            break;
        case BoundType::UInt64:
            add(other.min_.u);
            add(other.max_.u);
            break;
        case BoundType::Double:
            add(other.min_.d);
            add(other.max_.d);
            break;
        case BoundType::String:
            add(dstd::StringView(other.min_str_));
            add(dstd::StringView(other.max_str_));
            break;
    }
    values_ = values;
    if (has_bounds()) {
        min_exact_ = min_exact_ && other.min_exact_;
        max_exact_ = max_exact_ && other.max_exact_;
    }
}

duql::Number ZoneBounds::min_number() const {
    switch (type_) {
        case BoundType::Int64:
            return duql::Number{min_.i};
        case BoundType::UInt64:
            return duql::Number{min_.u};
        default:
            return duql::Number{min_.d};
    }
}

duql::Number ZoneBounds::max_number() const {
    switch (type_) {
        case BoundType::Int64:
            return duql::Number{max_.i};
        case BoundType::UInt64:
            return duql::Number{max_.u};
        default:
            return duql::Number{max_.d};
    }
}

namespace {

dstd::Optional<std::uint64_t> whole_u64(const duql::Number& n) {
    constexpr double TWO_POW_64 = 18446744073709551616.0;
    if (const auto* i = std::get_if<std::int64_t>(&n))
        return *i >= 0 ? dstd::Optional<std::uint64_t>(
                             static_cast<std::uint64_t>(*i))
                       : dstd::nullopt;
    if (const auto* u = std::get_if<std::uint64_t>(&n)) return *u;
    const double d = std::get<double>(n);
    if (std::isfinite(d) && d >= 0 && d < TWO_POW_64 && std::trunc(d) == d)
        return static_cast<std::uint64_t>(d);
    return dstd::nullopt;
}

dstd::Optional<double> as_double(const duql::Number& n) {
    if (const auto* i = std::get_if<std::int64_t>(&n))
        return static_cast<double>(*i);
    if (const auto* u = std::get_if<std::uint64_t>(&n))
        return static_cast<double>(*u);
    return std::get<double>(n);
}

dstd::String number_text(const duql::Number& n) {
    if (const auto* i = std::get_if<std::int64_t>(&n))
        return dstd::to_string(*i);
    if (const auto* u = std::get_if<std::uint64_t>(&n))
        return dstd::to_string(*u);
    return dstd::double_text(std::get<double>(n));
}

}  // namespace

dstd::Optional<std::uint64_t> ZoneBounds::min_u64() const {
    if (!is_numeric()) return dstd::nullopt;
    return whole_u64(min_number());
}

dstd::Optional<std::uint64_t> ZoneBounds::max_u64() const {
    if (!is_numeric()) return dstd::nullopt;
    return whole_u64(max_number());
}

dstd::Optional<double> ZoneBounds::min_double() const {
    if (!is_numeric()) return dstd::nullopt;
    return as_double(min_number());
}

dstd::Optional<double> ZoneBounds::max_double() const {
    if (!is_numeric()) return dstd::nullopt;
    return as_double(max_number());
}

dstd::String ZoneBounds::min_text() const {
    if (type_ == BoundType::String) return min_str_;
    return is_numeric() ? number_text(min_number()) : dstd::String();
}

dstd::String ZoneBounds::max_text() const {
    if (type_ == BoundType::String) return max_str_;
    return is_numeric() ? number_text(max_number()) : dstd::String();
}

bool ZoneBounds::min_is_negative() const {
    switch (type_) {
        case BoundType::Int64:
            return min_.i < 0;
        case BoundType::Double:
            return min_.d < 0;
        case BoundType::String:
            return !min_str_.empty() && min_str_.front() == '-';
        default:
            return false;
    }
}

bool ZoneBounds::same_bounds(const ZoneBounds& other) const {
    if (type_ != other.type_) return false;
    switch (type_) {
        case BoundType::Int64:
            return min_.i == other.min_.i && max_.i == other.max_.i;
        case BoundType::UInt64:
            return min_.u == other.min_.u && max_.u == other.max_.u;
        case BoundType::Double:
            return min_.d == other.min_.d && max_.d == other.max_.d;
        case BoundType::String:
            return min_str_ == other.min_str_ && max_str_ == other.max_str_;
        default:
            return true;
    }
}

std::size_t ZoneBounds::bounds_bytes() const {
    if (is_numeric()) return 16;
    if (type_ == BoundType::String)
        return 8 + min_str_.size() + max_str_.size();
    return 0;
}

dstd::Optional<int> ZoneBounds::compare(bool max,
                                        const ZoneLiteral& literal) const {
    if (is_numeric()) {
        if (!literal.number) return dstd::nullopt;
        return duql::compare_numbers(max ? max_number() : min_number(),
                                     *literal.number);
    }
    if (type_ == BoundType::String && literal.text)
        return compare_bytes(max ? max_str_ : min_str_, *literal.text);
    return dstd::nullopt;
}

bool ZoneBounds::may_match(ZoneOp op, const ZoneLiteral& literal) const {
    if (!has_bounds()) return true;
    const bool upper = op == ZoneOp::Gt || op == ZoneOp::Ge;
    if (!(upper ? max_exact_ : min_exact_)) return true;
    const auto c = compare(upper, literal);
    if (!c) return true;
    switch (op) {
        case ZoneOp::Gt:
            return *c > 0;
        case ZoneOp::Ge:
            return *c >= 0;
        case ZoneOp::Lt:
            return *c < 0;
        case ZoneOp::Le:
            return *c <= 0;
    }
    return true;
}

bool ZoneBounds::may_equal(const ZoneLiteral& literal) const {
    if (!has_bounds()) return true;
    if (min_exact_) {
        const auto c = compare(false, literal);
        if (c && *c > 0) return false;
    }
    if (max_exact_) {
        const auto c = compare(true, literal);
        if (c && *c < 0) return false;
    }
    return true;
}

bool ZoneBounds::all_match(ZoneOp op, const ZoneLiteral& literal) const {
    if (!has_bounds() || nulls_ != 0 || nans_ != 0) return false;
    const bool lower = op == ZoneOp::Gt || op == ZoneOp::Ge;
    if (!(lower ? min_exact_ : max_exact_)) return false;
    const auto c = compare(!lower, literal);
    if (!c) return false;
    switch (op) {
        case ZoneOp::Gt:
            return *c > 0;
        case ZoneOp::Ge:
            return *c >= 0;
        case ZoneOp::Lt:
            return *c < 0;
        case ZoneOp::Le:
            return *c <= 0;
    }
    return false;
}

void ZoneBounds::assign(BoundType type, std::uint64_t values,
                        std::uint64_t nulls, std::uint64_t nans, bool min_exact,
                        bool max_exact) {
    type_ = type;
    values_ = values;
    nulls_ = nulls;
    nans_ = nans;
    min_exact_ = min_exact;
    max_exact_ = max_exact;
}

}  // namespace dftracer::utils::trace::index::extensions
