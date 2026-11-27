// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dataframe/internal/column_read.h>
#include <dftracer/utils/dataframe/internal/field_stat_simd.h>
#include <dftracer/utils/dataframe/kernels/field_stat.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/dstd/bits.h>
#include <dftracer/utils/dstd/int128.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "dftracer/utils/dataframe/kernels/field_stat.cpp"
#include <hwy/foreach_target.h>  // must precede highway.h
#include <hwy/highway.h>

HWY_BEFORE_NAMESPACE();
namespace dftracer::utils::dataframe {
namespace HWY_NAMESPACE {

// One step of the first pass: sum, min and max lanes. Vectors travel by value
// or reference, never in a struct, so a sizeless SVE vector compiles.
template <class V>
void SumMinMaxAdd(V& sum, V& min, V& max, V v) {
    sum = hwy::HWY_NAMESPACE::Add(sum, v);
    min = hwy::HWY_NAMESPACE::Min(min, v);
    max = hwy::HWY_NAMESPACE::Max(max, v);
}

template <class D, class V>
void SumMinMaxReduce(D d, FsRaw& r, V sum, V min, V max) {
    r.sum = hwy::HWY_NAMESPACE::ReduceSum(d, sum);
    r.min = hwy::HWY_NAMESPACE::ReduceMin(d, min);
    r.max = hwy::HWY_NAMESPACE::ReduceMax(d, max);
}

inline void AddScalar(FsRaw& r, double v) {
    r.sum += v;
    if (v < r.min) r.min = v;
    if (v > r.max) r.max = v;
}

// One vector of the second pass: the deviations dv from the mean, summed as
// powers one to four.
template <class V>
void MomentsAdd(V& v1, V& v2, V& v3, V& v4, V v, V vm) {
    const V dv = hwy::HWY_NAMESPACE::Sub(v, vm);
    const V dd = hwy::HWY_NAMESPACE::Mul(dv, dv);
    v1 = hwy::HWY_NAMESPACE::Add(v1, dv);
    v2 = hwy::HWY_NAMESPACE::Add(v2, dd);
    v3 = hwy::HWY_NAMESPACE::Add(v3, hwy::HWY_NAMESPACE::Mul(dd, dv));
    v4 = hwy::HWY_NAMESPACE::Add(v4, hwy::HWY_NAMESPACE::Mul(dd, dd));
}

// The power sums of the deviations, powers one to four.
struct MomentSums {
    double s1, s2, s3, s4;

    void add(double dv) {
        const double dd = dv * dv;
        s1 += dv;
        s2 += dd;
        s3 += dd * dv;
        s4 += dd * dd;
    }
};

// The second pass is about the mean of the first. It also sums the deviations,
// so cm2 gets the corrected two-pass term (sum d)^2 / n that removes the
// rounding error of the mean itself. A raw power-sum pass cancels when the mean
// is large next to the spread. The first pass's mean is off by e = (sum d) / n:
// take the moments about the true mean, shift + e, with the exact corrections
// for that offset.
inline auto LoadAsF64(const hwy::HWY_NAMESPACE::ScalableTag<double> df,
                      const double* p) {
    return hwy::HWY_NAMESPACE::LoadU(df, p);
}

inline auto LoadAsF64(const hwy::HWY_NAMESPACE::ScalableTag<double> df,
                      const std::int64_t* p) {
    const hwy::HWY_NAMESPACE::ScalableTag<std::int64_t> di;
    return hwy::HWY_NAMESPACE::ConvertTo(df, hwy::HWY_NAMESPACE::LoadU(di, p));
}

template <class D, class T>
void CentralMoments(D d, FsRaw& r, const T* x, std::size_t n) {
    const std::size_t L = hwy::HWY_NAMESPACE::Lanes(d);
    const double mean = r.sum / static_cast<double>(n);
    const auto vm = hwy::HWY_NAMESPACE::Set(d, mean);
    auto v1 = hwy::HWY_NAMESPACE::Zero(d), v2 = hwy::HWY_NAMESPACE::Zero(d),
         v3 = hwy::HWY_NAMESPACE::Zero(d), v4 = hwy::HWY_NAMESPACE::Zero(d);
    std::size_t i = 0;
    for (; i + L <= n; i += L)
        MomentsAdd(v1, v2, v3, v4, LoadAsF64(d, x + i), vm);
    MomentSums m{hwy::HWY_NAMESPACE::ReduceSum(d, v1),
                 hwy::HWY_NAMESPACE::ReduceSum(d, v2),
                 hwy::HWY_NAMESPACE::ReduceSum(d, v3),
                 hwy::HWY_NAMESPACE::ReduceSum(d, v4)};
    for (; i < n; ++i) m.add(static_cast<double>(x[i]) - mean);
    const double s1 = m.s1, s2 = m.s2, s3 = m.s3, s4 = m.s4;
    const double dn = static_cast<double>(n);
    const double e = s1 / dn;
    r.shift = mean;
    r.cmean = e;
    r.cm2 = s2 - s1 * e;
    r.cm3 = s3 - 3.0 * e * s2 + 2.0 * dn * e * e * e;
    r.cm4 = s4 - 4.0 * e * s3 + 6.0 * e * e * s2 - 3.0 * dn * e * e * e * e;
}

// Two passes over the chunk: sum, min and max first, then the central moments
// about the mean just computed.
FsRaw MomentsF64Impl(const double* x, std::size_t n) {
    const hwy::HWY_NAMESPACE::ScalableTag<double> d;
    const std::size_t L = hwy::HWY_NAMESPACE::Lanes(d);
    auto sum = hwy::HWY_NAMESPACE::Zero(d);
    auto min = hwy::HWY_NAMESPACE::Set(d, x[0]);
    auto max = hwy::HWY_NAMESPACE::Set(d, x[0]);
    std::size_t i = 0;
    for (; i + L <= n; i += L)
        SumMinMaxAdd(sum, min, max, hwy::HWY_NAMESPACE::LoadU(d, x + i));
    FsRaw r{};
    SumMinMaxReduce(d, r, sum, min, max);
    for (; i < n; ++i) AddScalar(r, x[i]);

    CentralMoments(d, r, x, n);
    return r;
}

FsRaw MomentsI64Impl(const std::int64_t* x, std::size_t n) {
    const hwy::HWY_NAMESPACE::ScalableTag<double> df;
    const hwy::HWY_NAMESPACE::ScalableTag<std::int64_t> di;
    const std::size_t L =
        hwy::HWY_NAMESPACE::Lanes(df);  // == Lanes(di); both 64-bit lanes
    auto sum = hwy::HWY_NAMESPACE::Zero(df);
    auto min = hwy::HWY_NAMESPACE::Set(df, static_cast<double>(x[0]));
    auto max = hwy::HWY_NAMESPACE::Set(df, static_cast<double>(x[0]));
    auto es = hwy::HWY_NAMESPACE::Zero(di);
    auto eh = hwy::HWY_NAMESPACE::Zero(di);
    auto emin = hwy::HWY_NAMESPACE::Set(di, x[0]),
         emax = hwy::HWY_NAMESPACE::Set(di, x[0]);
    std::size_t i = 0;
    for (; i + L <= n; i += L) {
        const auto vi = hwy::HWY_NAMESPACE::LoadU(di, x + i);
        es = hwy::HWY_NAMESPACE::Add(es, vi);
        eh =
            hwy::HWY_NAMESPACE::Add(eh, hwy::HWY_NAMESPACE::ShiftRight<32>(vi));
        emin = hwy::HWY_NAMESPACE::Min(emin, vi);
        emax = hwy::HWY_NAMESPACE::Max(emax, vi);
        SumMinMaxAdd(sum, min, max, hwy::HWY_NAMESPACE::ConvertTo(df, vi));
    }
    FsRaw r{};
    SumMinMaxReduce(df, r, sum, min, max);
    r.esum = hwy::HWY_NAMESPACE::ReduceSum(di, es);
    r.emin = hwy::HWY_NAMESPACE::ReduceMin(di, emin);
    r.emax = hwy::HWY_NAMESPACE::ReduceMax(di, emax);
    r.ehigh = hwy::HWY_NAMESPACE::ReduceSum(di, eh);
    for (; i < n; ++i) {
        const std::int64_t xi = x[i];
        AddScalar(r, static_cast<double>(xi));
        static_cast<void>(dstd::bits::add_overflow(r.esum, xi, &r.esum));
        r.ehigh += xi >> 32;
        if (xi < r.emin) r.emin = xi;
        if (xi > r.emax) r.emax = xi;
    }

    CentralMoments(df, r, x, n);
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

void scalar_reduce(FieldStat& fs, const Series& c, std::int64_t b,
                   std::int64_t e) {
    const TypeId t = c.type();
    if (!is_arithmetic_type(t) && !is_temporal_type(t)) {
        DFTRACER_UTILS_LOG_ERROR("field_stat: no numeric value for type '{}'",
                                 type_name(t));
        return;
    }
    const bool is_double = is_double_decoded(t);
    const bool is_u = is_uint(t);
    const dftu_series* h = c.handle();
    for (std::int64_t i = b; i < e; ++i) {
        const dftu_series* leaf = h;
        std::int64_t j = i;
        if (!find_leaf(leaf, j)) continue;
        if (is_double)
            fs.add(cell_f64(leaf, j));
        else if (is_u)
            fs.add(cell_u64(leaf, j));
        else
            fs.add(cell_i64(leaf, j));
    }
}

// The SIMD pass over `n` > 0 contiguous values, none null.
FieldStat dense_stat(const FsRaw& r, std::size_t n, FieldStatDomain domain) {
    FieldStat fs;
    fs.n = n;
    fs.sum = r.sum;
    fs.shift = r.shift;
    fs.cmean = r.cmean;
    fs.cm2 = r.cm2;
    fs.cm3 = r.cm3;
    fs.cm4 = r.cm4;
    fs.min = r.min;
    fs.max = r.max;
    fs.domain = domain;
    return fs;
}

FieldStat dense_stat(const double* x, std::size_t n) {
    return dense_stat(HWY_DYNAMIC_DISPATCH(MomentsF64Impl)(x, n), n,
                      FieldStatDomain::F64);
}

FieldStat dense_stat(const std::int64_t* x, std::size_t n) {
    const FsRaw r = HWY_DYNAMIC_DISPATCH(MomentsI64Impl)(x, n);
    FieldStat fs = dense_stat(r, n, FieldStatDomain::I64);
    fs.esum = r.esum;
    fs.emin = r.emin;
    fs.emax = r.emax;
    // The exact sum is ehigh * 2^32 plus the low halves, which stay below 2^64
    // for n < 2^32, so the wraps of esum follow from the two.
    const dstd::int128_t one_wrap = dstd::int128_t{1} << 64;
    const dstd::int128_t high =
        static_cast<dstd::int128_t>(r.ehigh) * (dstd::int128_t{1} << 32);
    fs.ecarry = static_cast<std::int64_t>((high - r.esum + one_wrap - 1) >> 64);
    return fs;
}

// The values of the non-null rows [begin, end) of the view `v`, whose values
// are T, packed into `out`. Returns how many there are.
template <class T>
std::int64_t pack_view(const dftu_series& v, std::int64_t begin,
                       std::int64_t end, T* out) {
    std::int64_t n = 0;
    const dftu_series* base = v.child().get();
    // The usual view, a filter's SELECTION over a FLAT column, is read through
    // its two buffers.
    if (v.encoding == Encoding::Selection && !v.validity.block() &&
        base->encoding == Encoding::Flat) {
        const auto* sel = reinterpret_cast<const std::int64_t*>(v.data.data());
        const T* x = reinterpret_cast<const T*>(base->data.data());
        const std::uint8_t* valid =
            base->validity.block() ? base->validity.data() : nullptr;
        for (std::int64_t i = begin; i < end; ++i) {
            const std::int64_t j = sel[i];
            if (j >= 0 && (!valid || ((valid[j >> 3] >> (j & 7)) & 1)))
                out[n++] = x[j];
        }
        return n;
    }
    for (std::int64_t i = begin; i < end; ++i) {
        T x{};
        if constexpr (std::is_same_v<T, double>) {
            if (try_cell_f64(&v, i, x)) out[n++] = x;
        } else {
            if (try_cell_i64(&v, i, x)) out[n++] = x;
        }
    }
    return n;
}

// Reduces a view in blocks, so the SIMD pass runs with no copy of the column. A
// block holding a null row adds value by value, as a FLAT column with nulls.
template <class T>
FieldStat view_stat(const dftu_series& v, std::int64_t begin,
                    std::int64_t end) {
    constexpr std::int64_t BLOCK = 1 << 11;
    T block[BLOCK];
    FieldStat fs;
    for (std::int64_t at = begin; at < end; at += BLOCK) {
        const std::int64_t rows = std::min(BLOCK, end - at);
        const std::int64_t n = pack_view(v, at, at + rows, block);
        if (n == rows) {
            fs.merge(dense_stat(block, static_cast<std::size_t>(n)));
        } else {
            for (std::int64_t k = 0; k < n; ++k) fs.add(block[k]);
        }
    }
    return fs;
}

}  // namespace

static FieldStat field_stat_reduce_serial(const Series& col, std::int64_t begin,
                                          std::int64_t end) {
    FieldStat fs;
    if (begin >= end) return fs;
    if (col.handle()->is_chunked()) {
        // Each chunk takes its own SIMD or scalar pass over the rows in range.
        for (std::int64_t at = begin; at < end;) {
            std::int64_t local = 0;
            const dftu_series& chunk = col.handle()->chunk_at(at, local);
            const std::int64_t rows = std::min(end - at, chunk.length - local);
            Series piece{const_cast<dftu_series*>(&chunk)};
            fs.merge(field_stat_reduce_serial(piece, local, local + rows));
            piece.release();
            at += rows;
        }
        return fs;
    }
    const TypeId t = col.type();
    const bool view = col.encoding() != Encoding::Flat;
    const bool dense = !view && col.null_count() == 0;
    const std::size_t n = static_cast<std::size_t>(end - begin);
    if (t == TypeId::Float64) {
        if (view) return view_stat<double>(*col.handle(), begin, end);
        if (dense) return dense_stat(col.data<double>() + begin, n);
    }
    if (t == TypeId::Int64) {
        if (view) return view_stat<std::int64_t>(*col.handle(), begin, end);
        if (dense) return dense_stat(col.data<std::int64_t>() + begin, n);
    }
    scalar_reduce(fs, col, begin, end);
    return fs;
}

FieldStat field_stat_reduce(const Series& col, std::int64_t begin,
                            std::int64_t end) {
    if (end < 0) end = col.length();
    const std::int64_t n = end - begin;
    // The per-range reduction is SIMD; for a large full-column reduction with a
    // backend, split into chunks and merge the mergeable FieldStats (central
    // moments, combined by the Chan/Pebay merge). Small n / no backend runs
    // serial. A view also gathers each row, so it splits finer.
    const Encoding encoding = col.encoding();
    const bool view =
        encoding != Encoding::Flat && encoding != Encoding::Chunked;
    const std::int64_t grain = std::int64_t{1} << (view ? 16 : 18);
    if (n <= grain || !parallel_backend_installed())
        return field_stat_reduce_serial(col, begin, end);
    return parallel_reduce<FieldStat>(
        n, grain, FieldStat{},
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
