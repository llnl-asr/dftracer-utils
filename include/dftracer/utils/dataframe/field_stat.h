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

/// One field's running sufficient statistic: n, sum, min, max and the central
/// moments about the running mean (cm2 = sum (x - mean)^2, cm3 and cm4
/// likewise): a complete mergeable summary for
/// count/sum/min/max/mean/var/std/skew/kurt. The central form is what keeps
/// var/std/skew/kurt accurate when the mean is large next to the spread: raw
/// power sums (sum x^2 ...) cancel catastrophically there (1e9 + N(0, 1) gave a
/// std of 0). Adds use the Welford/Pebay update and merges the Chan/Pebay
/// combination. The shared aggregation atom for the dataframe engine, the View,
/// and the aggregation tier.
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

    /// The running mean as a pair, `shift + cmean`: `shift` is a value near the
    /// mean (the first value, or a chunk's mean) and `cmean` the small
    /// remainder, so the centre keeps digits that one double would lose when
    /// the mean is large next to the spread. The central moments are sums of
    /// (x - mean)^2, ^3, ^4. Kept only by the adds and merges that maintain
    /// moments; a state built with `add_light` leaves them zero and must not be
    /// read for a variance.
    double shift = 0;
    double cmean = 0;
    double cm2 = 0;
    double cm3 = 0;
    double cm4 = 0;
    /// Net wraps of `esum`: the exact sum is esum + ecarry * 2^64, so it fits
    /// its domain only while ecarry is 0, whatever the order of adds and
    /// merges. Touched only when an add wraps.
    std::int64_t ecarry = 0;

    /// The exact integer sum fits its domain (int64 or uint64).
    bool esum_fits() const { return ecarry == 0; }

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

    /// The `add` overloads without the moments (cmean, cm2, cm3, cm4): for
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
            add_i64(x);
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
            add_u64(x);
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
            add_i64(x);
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
            add_u64(x);
            if (x < std::bit_cast<std::uint64_t>(emin))
                emin = std::bit_cast<std::int64_t>(x);
            if (x > std::bit_cast<std::uint64_t>(emax))
                emax = std::bit_cast<std::int64_t>(x);
        } else {
            domain = FieldStatDomain::F64;  // mixed with I64/F64
        }
    }

    void add_i64(std::int64_t x) {
        if (__builtin_add_overflow(esum, x, &esum)) [[unlikely]]
            ecarry += x < 0 ? -1 : 1;
    }
    void add_u64(std::uint64_t x) {
        std::uint64_t s = std::bit_cast<std::uint64_t>(esum);
        if (__builtin_add_overflow(s, x, &s)) [[unlikely]]
            ++ecarry;
        esum = std::bit_cast<std::int64_t>(s);
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
        merge_moments(o);  // reads the pre-merge n and sum of both sides
        sum += o.sum;
        n += o.n;

        // Exact path: only when both sides share the same integer domain.
        if (domain != o.domain || domain == FieldStatDomain::F64) {
            domain = FieldStatDomain::F64;
        } else if (domain == FieldStatDomain::I64) {
            ecarry += o.ecarry;
            add_i64(o.esum);
            if (o.emin < emin) emin = o.emin;
            if (o.emax > emax) emax = o.emax;
        } else {  // U64
            ecarry += o.ecarry;
            add_u64(std::bit_cast<std::uint64_t>(o.esum));
            if (std::bit_cast<std::uint64_t>(o.emin) <
                std::bit_cast<std::uint64_t>(emin))
                emin = o.emin;
            if (std::bit_cast<std::uint64_t>(o.emax) >
                std::bit_cast<std::uint64_t>(emax))
                emax = o.emax;
        }
    }

    /// The centre the moments are taken about, as the pair (hi, lo). A state
    /// with no shift and no moments is totals only (a pre-aggregated row or an
    /// `add_light` state): it is centred on its own mean, sum / n.
    void centre(double& hi, double& lo) const {
        if (n != 0 && shift == 0.0 && cmean == 0.0 && cm2 == 0.0 &&
            cm3 == 0.0 && cm4 == 0.0) {
            hi = sum / static_cast<double>(n);
            lo = 0.0;
        } else {
            hi = shift;
            lo = cmean;
        }
    }
    /// The mean the moments are taken about, rounded to one double.
    double moment_mean() const {
        double hi, lo;
        centre(hi, lo);
        return hi + lo;
    }

    /// Sum of squares, from the central moments: cm2 + n * mean^2.
    double sumsq() const {
        const double mu = moment_mean();
        return n ? cm2 + static_cast<double>(n) * mu * mu : 0.0;
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
        const double c2 = cm2 < 0.0 ? 0.0 : cm2;  // clamp round-off
        if (sample) return n < 2 ? 0.0 : c2 / (dn - 1.0);
        return c2 / dn;
    }
    double stddev(bool sample = true) const {
        return std::sqrt(variance(sample));
    }
    double skewness() const {
        if (n < 1) return 0.0;
        const double dn = static_cast<double>(n);
        const double c2 = cm2 / dn;
        if (c2 <= 0.0) return 0.0;
        return (cm3 / dn) / std::pow(c2, 1.5);
    }
    double kurtosis() const {
        if (n < 1) return 0.0;
        const double dn = static_cast<double>(n);
        const double c2 = cm2 / dn;
        if (c2 <= 0.0) return 0.0;
        return (cm4 / dn) / (c2 * c2) - 3.0;
    }

   private:
    void add_double(double x) {
        if (n == 0) {
            min = max = x;
        } else {
            if (x < min) min = x;
            if (x > max) max = x;
        }
        // Welford/Pebay single-value update of the central moments.
        double hi, lo;
        centre(hi, lo);  // before sum and n change
        if (n == 0) {
            hi = x;      // the first value is the shift
            lo = 0.0;
        }
        sum += x;
        const double n1 = static_cast<double>(n);
        ++n;
        const double nn = static_cast<double>(n);
        const double delta = (x - hi) - lo;  // x - mean, without losing digits
        const double dn = delta / nn;
        const double dn2 = dn * dn;
        const double term1 = delta * dn * n1;
        shift = hi;
        cmean = lo + dn;
        cm4 += term1 * dn2 * (nn * nn - 3.0 * nn + 3.0) + 6.0 * dn2 * cm2 -
               4.0 * dn * cm3;
        cm3 += term1 * dn * (nn - 2.0) - 3.0 * dn * cm2;
        cm2 += term1;
    }
    /// Chan/Pebay combination of the central moments of this state and `o`;
    /// `n` is still the count before the merge.
    void merge_moments(const FieldStat& o) {
        const double na = static_cast<double>(n);
        const double nb = static_cast<double>(o.n);
        const double nt = na + nb;
        double ha, la, hb, lb;
        centre(ha, la);
        o.centre(hb, lb);
        const double delta = (hb - ha) + (lb - la);  // the difference of means
        const double d2 = delta * delta;
        const double nanb = na * nb;
        const double m2 = cm2 + o.cm2 + d2 * nanb / nt;
        const double m3 = cm3 + o.cm3 +
                          d2 * delta * nanb * (na - nb) / (nt * nt) +
                          3.0 * delta * (na * o.cm2 - nb * cm2) / nt;
        const double m4 =
            cm4 + o.cm4 +
            d2 * d2 * nanb * (na * na - nanb + nb * nb) / (nt * nt * nt) +
            6.0 * d2 * (na * na * o.cm2 + nb * nb * cm2) / (nt * nt) +
            4.0 * delta * (na * o.cm3 - nb * cm3) / nt;
        shift = ha;
        cmean = la + delta * nb / nt;
        cm2 = m2;
        cm3 = m3;
        cm4 = m4;
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

/// Mergeable central co-moments of a pair (x, y): the accumulator behind corr,
/// covar and the regressions. Each running mean is a pair `shift + cmean` (see
/// FieldStat), and `cxx`, `cyy`, `cxy` are the sums of (x - mx)^2, (y - my)^2
/// and (x - mx)(y - my). A row updates it with the Welford step; two states
/// merge with the Chan combination, which is associative, so any split of the
/// rows into merged partitions gives the one-pass result.
struct CoStat {
    std::uint64_t n = 0;
    double shx = 0, cmx = 0;
    double shy = 0, cmy = 0;
    double cxx = 0, cyy = 0, cxy = 0;

    double mean_x() const { return shx + cmx; }
    double mean_y() const { return shy + cmy; }

    void add(double x, double y) {
        if (n == 0) {
            shx = x;
            shy = y;
            cmx = cmy = 0.0;
        }
        const double n1 = static_cast<double>(n);
        ++n;
        const double nn = static_cast<double>(n);
        const double dx = (x - shx) - cmx;  // x - mean_x, without losing digits
        const double dy = (y - shy) - cmy;
        const double w = n1 / nn;
        cxx += dx * dx * w;
        cyy += dy * dy * w;
        cxy += dx * dy * w;
        cmx += dx / nn;
        cmy += dy / nn;
    }

    void merge(const CoStat& o) {
        if (o.n == 0) return;
        if (n == 0) {
            *this = o;
            return;
        }
        const double na = static_cast<double>(n);
        const double nb = static_cast<double>(o.n);
        const double nt = na + nb;
        const double dx = (o.shx - shx) + (o.cmx - cmx);  // difference of means
        const double dy = (o.shy - shy) + (o.cmy - cmy);
        const double w = na * nb / nt;
        cxx += o.cxx + dx * dx * w;
        cyy += o.cyy + dy * dy * w;
        cxy += o.cxy + dx * dy * w;
        cmx += dx * nb / nt;
        cmy += dy * nb / nt;
        n += o.n;
    }
};

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_FIELD_STAT_H
