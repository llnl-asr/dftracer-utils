#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/dataframe/internal/column_read.h>
#include <dftracer/utils/dataframe/internal/field_stat_simd.h>
#include <dftracer/utils/dataframe/kernels/field_stat.h>
#include <dftracer/utils/dataframe/parallel.h>

#include <cstddef>
#include <cstdint>

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "dftracer/utils/dataframe/kernels/field_stat.cpp"
#include <hwy/foreach_target.h>  // must precede highway.h
#include <hwy/highway.h>

HWY_BEFORE_NAMESPACE();
namespace dftracer::utils::dataframe {
namespace HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// Two passes over the chunk: sum, min and max first, then the central moments
// about the mean just computed. The second pass also sums the deviations, so
// cm2 gets the corrected two-pass term (sum d)^2 / n that removes the rounding
// error of the mean itself. A raw power-sum pass cancels when the mean is large
// next to the spread.
FsRaw MomentsF64Impl(const double* x, std::size_t n) {
    const hn::ScalableTag<double> d;
    const std::size_t L = hn::Lanes(d);
    auto vs = hn::Zero(d);
    auto vmin = hn::Set(d, x[0]), vmax = hn::Set(d, x[0]);
    std::size_t i = 0;
    for (; i + L <= n; i += L) {
        const auto v = hn::LoadU(d, x + i);
        vs = hn::Add(vs, v);
        vmin = hn::Min(vmin, v);
        vmax = hn::Max(vmax, v);
    }
    FsRaw r{hn::ReduceSum(d, vs),
            hn::ReduceMin(d, vmin),
            hn::ReduceMax(d, vmax),
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0};
    for (; i < n; ++i) {
        const double v = x[i];
        r.sum += v;
        if (v < r.min) r.min = v;
        if (v > r.max) r.max = v;
    }
    const double mean = r.sum / static_cast<double>(n);
    const auto vm = hn::Set(d, mean);
    auto v1 = hn::Zero(d), v2 = hn::Zero(d), v3 = hn::Zero(d), v4 = hn::Zero(d);
    i = 0;
    for (; i + L <= n; i += L) {
        const auto dv = hn::Sub(hn::LoadU(d, x + i), vm);
        const auto dd = hn::Mul(dv, dv);
        v1 = hn::Add(v1, dv);
        v2 = hn::Add(v2, dd);
        v3 = hn::Add(v3, hn::Mul(dd, dv));
        v4 = hn::Add(v4, hn::Mul(dd, dd));
    }
    double s1 = hn::ReduceSum(d, v1), s2 = hn::ReduceSum(d, v2),
           s3 = hn::ReduceSum(d, v3), s4 = hn::ReduceSum(d, v4);
    for (; i < n; ++i) {
        const double dv = x[i] - mean, dd = dv * dv;
        s1 += dv;
        s2 += dd;
        s3 += dd * dv;
        s4 += dd * dd;
    }
    // The first pass's mean is off by e = (sum d) / n. Take the moments about
    // the true mean, shift + e, with the exact corrections for that offset.
    const double dn = static_cast<double>(n);
    const double e = s1 / dn;
    r.shift = mean;
    r.cmean = e;
    r.cm2 = s2 - s1 * e;
    r.cm3 = s3 - 3.0 * e * s2 + 2.0 * dn * e * e * e;
    r.cm4 = s4 - 4.0 * e * s3 + 6.0 * e * e * s2 - 3.0 * dn * e * e * e * e;
    return r;
}

FsRaw MomentsI64Impl(const std::int64_t* x, std::size_t n) {
    const hn::ScalableTag<double> df;
    const hn::ScalableTag<std::int64_t> di;
    const std::size_t L = hn::Lanes(df);  // == Lanes(di); both 64-bit lanes
    auto vs = hn::Zero(df);
    const double x0 = static_cast<double>(x[0]);
    auto vmin = hn::Set(df, x0), vmax = hn::Set(df, x0);
    auto es = hn::Zero(di);
    auto emin = hn::Set(di, x[0]), emax = hn::Set(di, x[0]);
    std::size_t i = 0;
    for (; i + L <= n; i += L) {
        const auto vi = hn::LoadU(di, x + i);
        es = hn::Add(es, vi);
        emin = hn::Min(emin, vi);
        emax = hn::Max(emax, vi);
        const auto v = hn::ConvertTo(df, vi);
        vs = hn::Add(vs, v);
        vmin = hn::Min(vmin, v);
        vmax = hn::Max(vmax, v);
    }
    FsRaw r{hn::ReduceSum(df, vs),
            hn::ReduceMin(df, vmin),
            hn::ReduceMax(df, vmax),
            0,
            0,
            0,
            0,
            0,
            hn::ReduceSum(di, es),
            hn::ReduceMin(di, emin),
            hn::ReduceMax(di, emax)};
    for (; i < n; ++i) {
        const std::int64_t xi = x[i];
        const double v = static_cast<double>(xi);
        r.sum += v;
        if (v < r.min) r.min = v;
        if (v > r.max) r.max = v;
        r.esum += xi;
        if (xi < r.emin) r.emin = xi;
        if (xi > r.emax) r.emax = xi;
    }
    const double mean = r.sum / static_cast<double>(n);
    const auto vm = hn::Set(df, mean);
    auto v1 = hn::Zero(df), v2 = hn::Zero(df), v3 = hn::Zero(df),
         v4 = hn::Zero(df);
    i = 0;
    for (; i + L <= n; i += L) {
        const auto dv = hn::Sub(hn::ConvertTo(df, hn::LoadU(di, x + i)), vm);
        const auto dd = hn::Mul(dv, dv);
        v1 = hn::Add(v1, dv);
        v2 = hn::Add(v2, dd);
        v3 = hn::Add(v3, hn::Mul(dd, dv));
        v4 = hn::Add(v4, hn::Mul(dd, dd));
    }
    double s1 = hn::ReduceSum(df, v1), s2 = hn::ReduceSum(df, v2),
           s3 = hn::ReduceSum(df, v3), s4 = hn::ReduceSum(df, v4);
    for (; i < n; ++i) {
        const double dv = static_cast<double>(x[i]) - mean, dd = dv * dv;
        s1 += dv;
        s2 += dd;
        s3 += dd * dv;
        s4 += dd * dd;
    }
    // The first pass's mean is off by e = (sum d) / n. Take the moments about
    // the true mean, shift + e, with the exact corrections for that offset.
    const double dn = static_cast<double>(n);
    const double e = s1 / dn;
    r.shift = mean;
    r.cmean = e;
    r.cm2 = s2 - s1 * e;
    r.cm3 = s3 - 3.0 * e * s2 + 2.0 * dn * e * e * e;
    r.cm4 = s4 - 4.0 * e * s3 + 6.0 * e * e * s2 - 3.0 * dn * e * e * e * e;
    return r;
}

}  // namespace HWY_NAMESPACE
}  // namespace dftracer::utils::dataframe
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace dftracer::utils::dataframe {

HWY_EXPORT(MomentsF64Impl);
HWY_EXPORT(MomentsI64Impl);

namespace {

bool is_uint(TypeId t) {
    return t == TypeId::Uint8 || t == TypeId::Uint16 || t == TypeId::Uint32 ||
           t == TypeId::Uint64;
}

// Float16/Decimal128/Decimal256 have no integer physical layout, so they must
// go through read_f64 like Float32/Float64 rather than falling into
// read_i64's default (which would silently sum 0 for every row).
bool is_double_decoded(TypeId t) {
    return t == TypeId::Float32 || t == TypeId::Float64 ||
           t == TypeId::Float16 || t == TypeId::Decimal128 ||
           t == TypeId::Decimal256;
}

void scalar_reduce(FieldStat& fs, const Series& c, std::int64_t b,
                   std::int64_t e) {
    const TypeId t = c.type();
    if (!is_arithmetic_type(t) && !is_temporal_type(t)) {
        DFTRACER_UTILS_LOG_ERROR("field_stat: no numeric value for type '%s'",
                                 type_name(t));
        return;
    }
    const bool is_double = is_double_decoded(t);
    const bool is_u = is_uint(t);
    for (std::int64_t i = b; i < e; ++i) {
        if (c.is_null(i)) continue;
        if (is_double)
            fs.add(read_f64(c, i));
        else if (is_u)
            fs.add(read_u64(c, i));
        else
            fs.add(read_i64(c, i));
    }
}

}  // namespace

static FieldStat field_stat_reduce_serial(const Series& col, std::int64_t begin,
                                          std::int64_t end) {
    FieldStat fs;
    if (begin >= end) return fs;
    const TypeId t = col.type();
    const bool dense =
        col.encoding() == Encoding::Flat && col.null_count() == 0;
    const std::size_t n = static_cast<std::size_t>(end - begin);
    if (dense && t == TypeId::Float64) {
        const FsRaw r =
            HWY_DYNAMIC_DISPATCH(MomentsF64Impl)(col.data<double>() + begin, n);
        fs.n = n;
        fs.sum = r.sum;
        fs.shift = r.shift;
        fs.cmean = r.cmean;
        fs.cm2 = r.cm2;
        fs.cm3 = r.cm3;
        fs.cm4 = r.cm4;
        fs.min = r.min;
        fs.max = r.max;
        fs.domain = FieldStatDomain::F64;
        return fs;
    }
    if (dense && t == TypeId::Int64) {
        const FsRaw r = HWY_DYNAMIC_DISPATCH(MomentsI64Impl)(
            col.data<std::int64_t>() + begin, n);
        fs.n = n;
        fs.sum = r.sum;
        fs.shift = r.shift;
        fs.cmean = r.cmean;
        fs.cm2 = r.cm2;
        fs.cm3 = r.cm3;
        fs.cm4 = r.cm4;
        fs.min = r.min;
        fs.max = r.max;
        fs.domain = FieldStatDomain::I64;
        fs.esum = r.esum;
        fs.emin = r.emin;
        fs.emax = r.emax;
        return fs;
    }
    scalar_reduce(fs, col, begin, end);
    return fs;
}

FieldStat field_stat_reduce(const Series& col, std::int64_t begin,
                            std::int64_t end) {
    if (col.handle()->is_chunked()) {
        const Series joined = col.materialize();
        return field_stat_reduce(joined, begin, end);
    }
    if (end < 0) end = col.length();
    const std::int64_t n = end - begin;
    // The per-range reduction is SIMD; for a large full-column reduction with a
    // backend, split into chunks and merge the mergeable FieldStats (central
    // moments, combined by the Chan/Pebay merge). Small n / no backend runs
    // serial.
    constexpr std::int64_t GRAIN = std::int64_t{1} << 18;
    if (n <= GRAIN || !parallel_backend_installed())
        return field_stat_reduce_serial(col, begin, end);
    return parallel_reduce<FieldStat>(
        n, GRAIN, FieldStat{},
        [&](std::int64_t b, std::int64_t e) {
            return field_stat_reduce_serial(col, begin + b, begin + e);
        },
        [](FieldStat a, const FieldStat& b) {
            a.merge(b);
            return a;
        });
}

}  // namespace dftracer::utils::dataframe
#endif  // HWY_ONCE
