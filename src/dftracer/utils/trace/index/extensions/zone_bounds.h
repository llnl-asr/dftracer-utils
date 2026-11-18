// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_EXTENSIONS_ZONE_BOUNDS_H
#define DFTRACER_UTILS_TRACE_INDEX_EXTENSIONS_ZONE_BOUNDS_H

#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/duql/numbers.h>

#include <cstdint>

namespace dftracer::utils::trace::index::extensions {

/// What the bounds of a zone compare as.
enum class BoundType : std::uint8_t {
    /// No value with a bound was seen: the zone is empty, or holds only nulls
    /// and NaNs.
    None = 0,
    Int64,
    UInt64,
    Double,
    /// Compared as unsigned bytes.
    String,
    /// Numbers and strings together: there are no bounds, and a range filter
    /// never prunes on the zone.
    Mixed,
};

/// How a range filter compares with the bounds of a zone.
enum class ZoneOp : std::uint8_t { Gt, Ge, Lt, Le };

/// The other side of a comparison with a zone: a number, or text.
struct ZoneLiteral {
    dstd::Optional<duql::Number> number;
    /// Set for a string literal.
    dstd::Optional<dstd::String> text;

    static ZoneLiteral from_number(duql::Number n);
    static ZoneLiteral from_text(dstd::String s);
};

/// The min and max of the values of one path in one granule, with the counts a
/// filter needs to trust them. A value that is NaN is counted and never a
/// bound. A bound is the real value, never rounded, except that an integer a
/// double cannot hold is rounded when it joins a double zone, which clears the
/// exact flag of that bound. A bound that is not exact never proves that a
/// value is absent, and never proves that every value passes a filter.
class ZoneBounds {
   public:
    /// Observes a value. Types that differ widen: Int64 with UInt64 stays an
    /// integer while the values fit, then becomes Double; an integer with a
    /// Double is Double; a number with a string is Mixed.
    void add(std::int64_t value);
    void add(std::uint64_t value);
    void add(double value);
    void add(dstd::StringView value);
    /// Counts `count` records that carry the path as null.
    void add_nulls(std::uint64_t count) { nulls_ += count; }

    /// Takes in the values another zone observed.
    void merge_from(const ZoneBounds& other);

    BoundType type() const { return type_; }
    bool has_bounds() const {
        return type_ != BoundType::None && type_ != BoundType::Mixed;
    }
    bool is_numeric() const {
        return type_ == BoundType::Int64 || type_ == BoundType::UInt64 ||
               type_ == BoundType::Double;
    }
    /// Values observed that are neither null nor NaN. A zone read from a
    /// payload does not know it: the payload keeps the records of the path in
    /// @ref kinds::Zone::present.
    std::uint64_t value_count() const { return values_; }
    std::uint64_t null_count() const { return nulls_; }
    std::uint64_t nan_count() const { return nans_; }
    bool min_exact() const { return min_exact_; }
    bool max_exact() const { return max_exact_; }
    bool exact() const { return min_exact_ && max_exact_; }

    /// The bounds of a numeric zone, widened to the widest exact number type.
    duql::Number min_number() const;
    duql::Number max_number() const;
    /// The bounds of a String zone.
    dstd::StringView min_string() const { return min_str_; }
    dstd::StringView max_string() const { return max_str_; }
    /// The bound as a whole number from 0 up, when it is one.
    dstd::Optional<std::uint64_t> min_u64() const;
    dstd::Optional<std::uint64_t> max_u64() const;
    /// The bound as a double, for a numeric zone.
    dstd::Optional<double> min_double() const;
    dstd::Optional<double> max_double() const;
    /// The bound as plain text: an integer in decimal, a double by its
    /// shortest text that reads back as the same value, a string as is.
    dstd::String min_text() const;
    dstd::String max_text() const;
    /// Whether the smallest bound is below zero.
    bool min_is_negative() const;

    /// Whether another zone has the same type and bounds.
    bool same_bounds(const ZoneBounds& other) const;
    /// What the bounds take in an encoded payload.
    std::size_t bounds_bytes() const;

    /// Whether some value in the zone may be `op literal`. False only when
    /// the bounds prove none is. A NaN or null never satisfies a comparison.
    bool may_match(ZoneOp op, const ZoneLiteral& literal) const;
    /// Whether the zone may hold a value equal to `literal`.
    bool may_equal(const ZoneLiteral& literal) const;
    /// Whether every value in the zone is `op literal`. False when it cannot
    /// be proven, including when the zone holds nulls or NaNs.
    bool all_match(ZoneOp op, const ZoneLiteral& literal) const;

    /// Sets the zone from decoded parts; for the payload decoder.
    void assign(BoundType type, std::uint64_t values, std::uint64_t nulls,
                std::uint64_t nans, bool min_exact, bool max_exact);
    void set_min(std::int64_t v) { min_.i = v; }
    void set_max(std::int64_t v) { max_.i = v; }
    void set_min(std::uint64_t v) { min_.u = v; }
    void set_max(std::uint64_t v) { max_.u = v; }
    void set_min(double v) { min_.d = v; }
    void set_max(double v) { max_.d = v; }
    void set_min(dstd::String v) { min_str_ = std::move(v); }
    void set_max(dstd::String v) { max_str_ = std::move(v); }
    std::int64_t min_raw_i64() const { return min_.i; }
    std::int64_t max_raw_i64() const { return max_.i; }
    std::uint64_t min_raw_u64() const { return min_.u; }
    std::uint64_t max_raw_u64() const { return max_.u; }
    double min_raw_double() const { return min_.d; }
    double max_raw_double() const { return max_.d; }

   private:
    union Number {
        double d;
        std::int64_t i;
        std::uint64_t u;
    };

    void to_double();
    void add_double(double value, bool lossy);
    void make_mixed();
    /// The sign of the min (or max) minus `literal`; nothing when they do not
    /// compare, such as a string zone against a number.
    dstd::Optional<int> compare(bool max, const ZoneLiteral& literal) const;

    Number min_{};
    Number max_{};
    dstd::String min_str_;
    dstd::String max_str_;
    std::uint64_t values_ = 0;
    std::uint64_t nulls_ = 0;
    std::uint64_t nans_ = 0;
    BoundType type_ = BoundType::None;
    bool min_exact_ = true;
    bool max_exact_ = true;
};

}  // namespace dftracer::utils::trace::index::extensions

#endif  // DFTRACER_UTILS_TRACE_INDEX_EXTENSIONS_ZONE_BOUNDS_H
