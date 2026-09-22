#ifndef DFTRACER_UTILS_DATAFRAME_FIELD_STAT_H
#define DFTRACER_UTILS_DATAFRAME_FIELD_STAT_H

#include <bit>
#include <cmath>
#include <cstdint>

namespace dftracer::utils::dataframe {

/// Numeric domain a field's values live in. Narrow integer/float types widen
/// losslessly into these three families; a field carrying both integer and
/// float values (or mixed signedness) demotes to F64.
enum class FieldStatDomain : std::uint8_t { I64, U64, F64 };

/// A field value carrying its source domain, so the accumulator keeps integers
/// exact instead of coercing everything to double at read time.
struct FieldNum {
    FieldStatDomain domain = FieldStatDomain::F64;
    std::int64_t i = 0;
    std::uint64_t u = 0;
    double d = 0;

    static FieldNum of(std::int64_t v) {
        return {FieldStatDomain::I64, v, 0, static_cast<double>(v)};
    }
    static FieldNum of(std::uint64_t v) {
        return {FieldStatDomain::U64, 0, v, static_cast<double>(v)};
    }
    static FieldNum of(double v) { return {FieldStatDomain::F64, 0, 0, v}; }

    double as_double() const {
        switch (domain) {
            case FieldStatDomain::I64:
                return static_cast<double>(i);
            case FieldStatDomain::U64:
                return static_cast<double>(u);
            default:
                return d;
        }
    }

    /// Exact integer view, for deriving an integer field (te = ts + dur)
    /// without rounding a large timestamp through a double.
    std::uint64_t as_u64() const {
        switch (domain) {
            case FieldStatDomain::I64:
                return static_cast<std::uint64_t>(i);
            case FieldStatDomain::U64:
                return u;
            default:
                return static_cast<std::uint64_t>(d);
        }
    }
};

/// One field's running sufficient statistic as raw power sums (sumsq = sum x^2,
/// m3 = sum x^3, m4 = sum x^4) plus n/min/max: a complete mergeable summary for
/// count/sum/min/max/mean/var/std/skew/kurt. The shared aggregation atom for
/// the dataframe engine, the View, and the aggregation tier.
///
/// The double sum/min/max/moments are always maintained (variance/skew/kurtosis
/// are inherently double). In addition, when every value has been a same-domain
/// integer, exact int64/uint64 sum/min/max are tracked so a Sum/Min/Max of an
/// integer field (durations, timestamps) is reported exactly rather than
/// rounded through a double (which loses precision past 2^53). A float value,
/// or mixed signedness, demotes `domain` to F64 and the exact accumulators are
/// ignored.
struct FieldStat {
    // The fields a count / sum / mean / min / max accumulation touches sit
    // in the first 64 bytes, one cache line per add; the higher moments
    // follow.
    std::uint64_t n = 0;  ///< events where the field was present
    double sum = 0;
    double min = 0;
    double max = 0;

    FieldStatDomain domain = FieldStatDomain::F64;
    /// Exact integer sum/min/max, valid only when domain != F64. Stored as
    /// int64 bit patterns; reinterpreted as uint64 when domain == U64.
    std::int64_t esum = 0;
    std::int64_t emin = 0;
    std::int64_t emax = 0;

    double sumsq = 0;
    double m3 = 0;
    double m4 = 0;

    void add(const FieldNum& v) {
        switch (v.domain) {
            case FieldStatDomain::I64:
                add(v.i);
                break;
            case FieldStatDomain::U64:
                add(v.u);
                break;
            default:
                add(v.d);
        }
    }

    void add(double x) {
        add_double(x);
        domain =
            FieldStatDomain::F64;  // a float value makes the field non-integral
    }

    /// The `add` overloads without the higher moments (sumsq, m3, m4): for
    /// an accumulation whose reducers are count, sum, mean, min and max only,
    /// which is most group-bys. The moments then stay zero, so a state built
    /// this way must not be read for a variance or a skew; the caller that
    /// chose this path knows its reducers.
    template <bool Moments = true>
    void add_with(double x) {
        if constexpr (Moments) {
            add_double(x);
        } else {
            add_light(x);
        }
        domain = FieldStatDomain::F64;
    }
    template <bool Moments = true>
    void add_with(std::int64_t x) {
        const bool first = n == 0;
        if constexpr (Moments) {
            add_double(static_cast<double>(x));
        } else {
            add_light(static_cast<double>(x));
        }
        if (first) {
            domain = FieldStatDomain::I64;
            esum = emin = emax = x;
        } else if (domain == FieldStatDomain::I64) {
            esum += x;
            if (x < emin) emin = x;
            if (x > emax) emax = x;
        } else {
            domain = FieldStatDomain::F64;
        }
    }
    template <bool Moments = true>
    void add_with(std::uint64_t x) {
        const bool first = n == 0;
        if constexpr (Moments) {
            add_double(static_cast<double>(x));
        } else {
            add_light(static_cast<double>(x));
        }
        if (first) {
            domain = FieldStatDomain::U64;
            esum = emin = emax = std::bit_cast<std::int64_t>(x);
        } else if (domain == FieldStatDomain::U64) {
            esum = std::bit_cast<std::int64_t>(
                std::bit_cast<std::uint64_t>(esum) + x);
            if (x < std::bit_cast<std::uint64_t>(emin))
                emin = std::bit_cast<std::int64_t>(x);
            if (x > std::bit_cast<std::uint64_t>(emax))
                emax = std::bit_cast<std::int64_t>(x);
        } else {
            domain = FieldStatDomain::F64;
        }
    }

    void add(std::int64_t x) {
        const bool first = n == 0;
        add_double(static_cast<double>(x));
        if (first) {
            domain = FieldStatDomain::I64;
            esum = emin = emax = x;
        } else if (domain == FieldStatDomain::I64) {
            esum += x;
            if (x < emin) emin = x;
            if (x > emax) emax = x;
        } else {
            domain = FieldStatDomain::F64;  // mixed with U64/F64
        }
    }

    void add(std::uint64_t x) {
        const bool first = n == 0;
        add_double(static_cast<double>(x));
        if (first) {
            domain = FieldStatDomain::U64;
            esum = emin = emax = std::bit_cast<std::int64_t>(x);
        } else if (domain == FieldStatDomain::U64) {
            esum = std::bit_cast<std::int64_t>(
                std::bit_cast<std::uint64_t>(esum) + x);
            if (x < std::bit_cast<std::uint64_t>(emin))
                emin = std::bit_cast<std::int64_t>(x);
            if (x > std::bit_cast<std::uint64_t>(emax))
                emax = std::bit_cast<std::int64_t>(x);
        } else {
            domain = FieldStatDomain::F64;  // mixed with I64/F64
        }
    }

    void merge(const FieldStat& o) {
        if (o.n == 0) return;
        if (n == 0) {
            *this = o;
            return;
        }
        // Double path (variance/skew/kurtosis atoms).
        if (o.min < min) min = o.min;
        if (o.max > max) max = o.max;
        sum += o.sum;
        sumsq += o.sumsq;
        m3 += o.m3;
        m4 += o.m4;
        n += o.n;

        // Exact path: only when both sides share the same integer domain.
        if (domain != o.domain || domain == FieldStatDomain::F64) {
            domain = FieldStatDomain::F64;
        } else if (domain == FieldStatDomain::I64) {
            esum += o.esum;
            if (o.emin < emin) emin = o.emin;
            if (o.emax > emax) emax = o.emax;
        } else {  // U64
            esum = std::bit_cast<std::int64_t>(
                std::bit_cast<std::uint64_t>(esum) +
                std::bit_cast<std::uint64_t>(o.esum));
            if (std::bit_cast<std::uint64_t>(o.emin) <
                std::bit_cast<std::uint64_t>(emin))
                emin = o.emin;
            if (std::bit_cast<std::uint64_t>(o.emax) >
                std::bit_cast<std::uint64_t>(emax))
                emax = o.emax;
        }
    }

    /// Finalizers over n = the field-present count (the dataframe-engine
    /// convention; consumers that denominate by a different N, e.g. the View's
    /// group-row count, finalize from the raw sums directly). Sample
    /// variance/std; population (biased) skewness and excess kurtosis -
    /// matching kernels/stats.cpp so the two surfaces agree.
    double mean() const { return n ? sum / static_cast<double>(n) : 0.0; }
    double variance(bool sample = true) const {
        if (n < 1) return 0.0;
        const double dn = static_cast<double>(n);
        double c2 = sumsq - sum * sum / dn;
        if (c2 < 0.0) c2 = 0.0;  // clamp round-off
        if (sample) return n < 2 ? 0.0 : c2 / (dn - 1.0);
        return c2 / dn;
    }
    double stddev(bool sample = true) const {
        return std::sqrt(variance(sample));
    }
    double skewness() const {
        if (n < 1) return 0.0;
        const double dn = static_cast<double>(n);
        const double mu = sum / dn;
        const double c2 = sumsq / dn - mu * mu;
        if (c2 <= 0.0) return 0.0;
        const double c3 =
            m3 / dn - 3.0 * mu * (sumsq / dn) + 2.0 * mu * mu * mu;
        return c3 / std::pow(c2, 1.5);
    }
    double kurtosis() const {
        if (n < 1) return 0.0;
        const double dn = static_cast<double>(n);
        const double mu = sum / dn;
        const double c2 = sumsq / dn - mu * mu;
        if (c2 <= 0.0) return 0.0;
        const double c4 = m4 / dn - 4.0 * mu * (m3 / dn) +
                          6.0 * mu * mu * (sumsq / dn) -
                          3.0 * mu * mu * mu * mu;
        return c4 / (c2 * c2) - 3.0;
    }

   private:
    void add_double(double x) {
        if (n == 0) {
            min = max = x;
        } else {
            if (x < min) min = x;
            if (x > max) max = x;
        }
        const double x2 = x * x;
        sum += x;
        sumsq += x2;
        m3 += x2 * x;
        m4 += x2 * x2;
        ++n;
    }
    void add_light(double x) {
        if (n == 0) {
            min = max = x;
        } else {
            if (x < min) min = x;
            if (x > max) max = x;
        }
        sum += x;
        ++n;
    }
};

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_FIELD_STAT_H
