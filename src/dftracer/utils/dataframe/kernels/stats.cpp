// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/c_api.h>                 // dftu_series_data
#include <dftracer/utils/dataframe/field_stat.h>            // FieldStat
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/column_read.h>  // read_f64
#include <dftracer/utils/dataframe/internal/quantile_lerp.h>
#include <dftracer/utils/dataframe/internal/string_reader.h>  // is_string_column
#include <dftracer/utils/dataframe/internal/value_ids.h>
#include <dftracer/utils/dataframe/kernels/cast.h>            // cast_simd
#include <dftracer/utils/dataframe/kernels/field_stat.h>  // field_stat_reduce
#include <dftracer/utils/dataframe/kernels/filter.h>      // take
#include <dftracer/utils/dataframe/kernels/sort.h>  // argsort, parallel_packed_sort
#include <dftracer/utils/dataframe/kernels/stats.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/dstd/deque.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/hash/splitmix64.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

namespace dftracer::utils::dataframe {

namespace {

// Every cell of `c`, a column with no null, as a double into `dst`.
void fill_dense(const dftu_series& c, double* dst) {
    const std::int64_t len = c.length;
    if (c.encoding == Encoding::Flat && c.data.block()) {
        if (c.type == TypeId::Float64) {
            std::memcpy(dst, c.data.data(),
                        static_cast<std::size_t>(len) * sizeof(double));
            return;
        }
        if (cast_simd(static_cast<std::int32_t>(c.type),
                      static_cast<std::int32_t>(TypeId::Float64), c.data.data(),
                      dst, static_cast<std::size_t>(len)))
            return;
    }
    for (std::int64_t i = 0; i < len; ++i) dst[i] = cell_f64(&c, i);
}

// Every cell of `h`, a non-FLAT column with no null, as a double into `dst`:
// chunks fill their own slices in parallel, other layers in row ranges.
void fill_dense_parallel(const dftu_series& h, double* dst) {
    if (h.is_chunked()) {
        const auto* starts =
            reinterpret_cast<const std::int64_t*>(h.data.data());
        parallel_for(static_cast<std::int64_t>(h.nested.size()), 1,
                     [&](std::int64_t b, std::int64_t e) {
                         for (std::int64_t k = b; k < e; ++k)
                             fill_dense(
                                 *h.nested[static_cast<std::size_t>(k)].series,
                                 dst + starts[k]);
                     });
        return;
    }
    parallel_for(h.length, std::int64_t{1} << 16,
                 [&](std::int64_t b, std::int64_t e) {
                     for (std::int64_t i = b; i < e; ++i)
                         dst[i] = cell_f64(&h, i);
                 });
}

// Non-null values as doubles (moment/quantile stats are numeric). The dense
// (no-null, FLAT) case is a straight widen-to-double over a contiguous buffer,
// so it goes through the SIMD cast fast path (memcpy for Float64, one
// Convert/Promote for Float32/Int32/Int64); only the null-skipping compaction
// path stays scalar.
dstd::Vector<double> nonnull_values(const Series& v) {
    const std::int64_t n = v.length();
    const dftu_series& h = *v.handle();
    if (n > 0 && v.encoding() == Encoding::Flat && v.null_count() == 0) {
        const void* src = dftu_series_data(v.handle());
        if (src) {
            dstd::Vector<double> out(static_cast<std::size_t>(n));
            if (v.type() == TypeId::Float64) {
                std::memcpy(out.data(), src,
                            static_cast<std::size_t>(n) * sizeof(double));
            } else if (!cast_simd(static_cast<std::int32_t>(v.type()),
                                  static_cast<std::int32_t>(TypeId::Float64),
                                  src, out.data(),
                                  static_cast<std::size_t>(n))) {
                for (std::int64_t i = 0; i < n; ++i) out[i] = read_f64(v, i);
            }
            return out;
        }
    }
    if (v.encoding() == Encoding::Chunked) {
        // Each chunk takes the fast path of its own encoding; a per-cell walk
        // would search the chunk starts for every row.
        const std::int64_t nulls = cell_null_count(h);
        if (nulls == 0) {
            dstd::Vector<double> out(static_cast<std::size_t>(n));
            fill_dense_parallel(h, out.data());
            return out;
        }
        dstd::Vector<double> out;
        out.reserve(static_cast<std::size_t>(n - nulls));
        for (const auto& piece : h.nested) {
            Series chunk{piece.series.get()};
            const dstd::Vector<double> part = nonnull_values(chunk);
            chunk.release();
            out.insert(out.end(), part.begin(), part.end());
        }
        return out;
    }
    if (v.encoding() == Encoding::Constant) {
        const std::int64_t present = n - cell_null_count(h);
        if (present == 0) return {};
        std::int64_t first = 0;
        while (cell_is_null(&h, first)) ++first;
        return dstd::Vector<double>(static_cast<std::size_t>(present),
                                    cell_f64(&h, first));
    }
    // Nulls, or a view: compact per chunk in parallel, then stitch. A flat
    // Float64 / Int64 column reads its buffers directly; the rest goes
    // through the generic cell readers.
    const bool layered = v.encoding() != Encoding::Flat;
    const bool has_nulls = !layered && v.null_count() > 0;
    constexpr std::int64_t GRAIN = std::int64_t{1} << 16;
    const std::int64_t chunks = (n + GRAIN - 1) / GRAIN;
    dstd::Vector<dstd::Vector<double>> parts(static_cast<std::size_t>(chunks));
    const std::uint8_t* valid =
        h.encoding == Encoding::Flat && h.validity.block() ? h.validity.data()
                                                           : nullptr;
    const bool flat64 =
        h.encoding == Encoding::Flat && h.data.block() &&
        (v.type() == TypeId::Float64 || v.type() == TypeId::Int64);
    parallel_for(n, GRAIN, [&](std::int64_t b, std::int64_t e) {
        dstd::Vector<double>& out = parts[static_cast<std::size_t>(b / GRAIN)];
        out.reserve(static_cast<std::size_t>(e - b));
        if (flat64) {
            const auto* d =
                reinterpret_cast<const std::uint8_t*>(h.data.data());
            const bool is_f = v.type() == TypeId::Float64;
            for (std::int64_t i = b; i < e; ++i) {
                if (valid && !((valid[i >> 3] >> (i & 7)) & 1)) continue;
                std::uint64_t bits;
                std::memcpy(&bits, d + static_cast<std::size_t>(i) * 8, 8);
                out.push_back(is_f ? std::bit_cast<double>(bits)
                                   : static_cast<double>(
                                         std::bit_cast<std::int64_t>(bits)));
            }
            return;
        }
        for (std::int64_t i = b; i < e; ++i) {
            if (layered) {
                double x = 0.0;
                if (try_cell_f64(&h, i, x)) out.push_back(x);
            } else if (!has_nulls || !v.is_null(i)) {
                out.push_back(read_f64(v, i));
            }
        }
    });
    dstd::Vector<std::size_t> at(parts.size() + 1, 0);
    for (std::size_t c = 0; c < parts.size(); ++c)
        at[c + 1] = at[c] + parts[c].size();
    dstd::Vector<double> out(at.back());
    parallel_for(
        static_cast<std::int64_t>(parts.size()), 1,
        [&](std::int64_t b, std::int64_t e) {
            for (std::int64_t c = b; c < e; ++c)
                std::copy(parts[static_cast<std::size_t>(c)].begin(),
                          parts[static_cast<std::size_t>(c)].end(),
                          out.begin() + static_cast<std::ptrdiff_t>(
                                            at[static_cast<std::size_t>(c)]));
        });
    return out;
}

// A reader of `v` for a serial scan: row `i` into `x`, false for a null row. A
// FLAT column tests its bitmap only when it holds a null, and any other layout
// is walked once per row.
auto present_rows(const Series& v) {
    const bool flat = v.encoding() == Encoding::Flat;
    const bool has_nulls = flat && v.null_count() > 0;
    return [&v, h = v.handle(), flat, has_nulls](std::int64_t i, double& x) {
        if (!flat) return try_cell_f64(h, i, x);
        if (has_nulls && v.is_null(i)) return false;
        x = cell_f64(h, i);
        return true;
    };
}

bool equal_at(const Series& v, std::int64_t a, std::int64_t b) {
    const dftu_series* h = v.handle();
    switch (value_domain(v.type())) {
        case ValueDomain::Bytes:
            return cell_bytes(h, a) == cell_bytes(h, b);
        case ValueDomain::Numeric:
            return cell_f64(h, a) == cell_f64(h, b);
        case ValueDomain::None:
            dstd::fatal(
                dstd::fmt<"column type '{}' has no comparable per-row value "
                          "(nested type)">(type_name(v.type()))
                    .c_str());
    }
    return false;
}

// Decimal128/256 are ValueDomain::Bytes (exact compare needs the scaled
// integer) but read_f64 decodes them to a real number, so they window like
// Float16; every other Bytes/None type has no numeric magnitude to widen.
bool refuse_non_numeric(dstd::StringView op, TypeId t) {
    if (value_domain(t) == ValueDomain::Numeric || t == TypeId::Decimal128 ||
        t == TypeId::Decimal256)
        return false;
    DFTRACER_UTILS_LOG_ERROR("{}: no numeric value for type '{}'", op,
                             type_name(t));
    return true;
}

// Whether `v`, reduced to `fs`, has too few valid rows for a variance. The
// rows are counted only below two values, as a count walks a view again.
bool too_few_for_spread(const Series& v, const FieldStat& fs, bool sample) {
    if (fs.n >= 2) return false;
    const std::int64_t n = v.length() - cell_null_count(*v.handle());
    return n == 0 || (sample && n < 2);
}

}  // namespace

double variance(const Series& v, bool sample) {
    if (refuse_non_numeric("variance", v.type())) return 0.0;
    const FieldStat fs = field_stat_reduce(v, 0, v.length());
    if (too_few_for_spread(v, fs, sample)) return std::nan("");
    return fs.variance(sample);
}

double stddev(const Series& v, bool sample) {
    if (refuse_non_numeric("stddev", v.type())) return 0.0;
    const FieldStat fs = field_stat_reduce(v, 0, v.length());
    if (too_few_for_spread(v, fs, sample)) return std::nan("");
    return fs.stddev(sample);
}

double skewness(const Series& v) {
    if (refuse_non_numeric("skewness", v.type())) return 0.0;
    return field_stat_reduce(v, 0, v.length()).skewness();
}

double kurtosis(const Series& v) {
    if (refuse_non_numeric("kurtosis", v.type())) return 0.0;
    return field_stat_reduce(v, 0, v.length()).kurtosis();
}

double skewness_unbiased(const Series& v) {
    if (refuse_non_numeric("skewness", v.type())) return 0.0;
    return field_stat_reduce(v, 0, v.length()).skewness_unbiased();
}

double kurtosis_unbiased(const Series& v) {
    if (refuse_non_numeric("kurtosis", v.type())) return 0.0;
    return field_stat_reduce(v, 0, v.length()).kurtosis_unbiased();
}

namespace {

// The order-preserving bit pattern of a double (the sort kernels' key); every
// NaN, whatever its sign bit, takes the largest key so it ranks above every
// number.
// FloatFlip by Michael Herf, Radix Tricks (2001), MIT-style license,
// https://stereopsis.com/radix.html
std::uint64_t sortable_key(double f) {
    if (std::isnan(f)) return ~0ULL;
    std::uint64_t u = std::bit_cast<std::uint64_t>(f);
    return u ^ ((u >> 63) ? ~0ULL : 0x8000000000000000ULL);
}

// The values at sorted positions `lo` and `lo + 1` of the non-null cells
// of `x` (`validity` the bitmap, null for none; `lo` counted over non-null
// cells), without sorting: sampled splitters cut the key range into
// buckets, one parallel histogram places the two positions, and only the
// bucket (or two) that holds them is collected and sorted. Falls back to
// a full sort (NaN) when the keys crowd one bucket, or when either position
// ranks among the NaN cells of a float column.
template <class T, class KeyFn>
dstd::Pair<double, double> two_order_statistics(const T* x, std::size_t n,
                                                const std::uint8_t* validity,
                                                std::size_t lo, KeyFn key) {
    constexpr std::size_t BUCKETS = 256, OVERSAMPLE = 16;
    constexpr std::int64_t GRAIN = std::int64_t{1} << 16;
    constexpr std::uint16_t NONE = 0xFFFF;
    auto is_null = [&](std::size_t i) {
        return validity && !((validity[i >> 3] >> (i & 7)) & 1);
    };
    dstd::Vector<std::uint64_t> sample;
    sample.reserve(BUCKETS * OVERSAMPLE);
    for (std::size_t i = 0; sample.size() < BUCKETS * OVERSAMPLE; ++i) {
        if (i >= BUCKETS * OVERSAMPLE * 4) return {std::nan(""), std::nan("")};
        const std::size_t at = dftracer::utils::dstd::hash::splitmix64(i) % n;
        if (!is_null(at)) sample.push_back(key(x[at]));
    }
    std::sort(sample.begin(), sample.end());
    dstd::Vector<std::uint64_t> splitters;
    splitters.reserve(BUCKETS - 1);
    for (std::size_t k = 1; k < BUCKETS; ++k)
        splitters.push_back(sample[k * OVERSAMPLE]);
    auto bucket_of = [&](T v) {
        return static_cast<std::size_t>(
            std::upper_bound(splitters.begin(), splitters.end(), key(v)) -
            splitters.begin());
    };
    const std::size_t chunks = (n + static_cast<std::size_t>(GRAIN) - 1) /
                               static_cast<std::size_t>(GRAIN);
    dstd::Vector<std::size_t> hist(chunks * BUCKETS, 0);
    Scratch<std::uint16_t> which(n);  // each value's bucket, found once
    parallel_for(
        static_cast<std::int64_t>(n), GRAIN,
        [&](std::int64_t b, std::int64_t e) {
            std::size_t* h =
                hist.data() + (static_cast<std::size_t>(b) / GRAIN) * BUCKETS;
            for (std::int64_t i = b; i < e; ++i) {
                const auto si = static_cast<std::size_t>(i);
                if (is_null(si)) {
                    which[si] = NONE;
                    continue;
                }
                const auto w = static_cast<std::uint16_t>(bucket_of(x[si]));
                which[si] = w;
                ++h[w];
            }
        });
    dstd::Vector<std::size_t> start(BUCKETS + 1, 0);
    for (std::size_t k = 0; k < BUCKETS; ++k) {
        std::size_t total = 0;
        for (std::size_t c = 0; c < chunks; ++c) total += hist[c * BUCKETS + k];
        start[k + 1] = start[k] + total;
    }
    const std::size_t present = start[BUCKETS];
    if (lo >= present) return {std::nan(""), std::nan("")};
    // The bucket holding `lo` and the one holding `lo + 1`, which may lie
    // past empty buckets.
    std::size_t b0 = 0;
    while (start[b0 + 1] <= lo) ++b0;
    std::size_t b1 = b0;
    if (lo + 1 < present)
        while (start[b1 + 1] <= lo + 1) ++b1;
    if (start[b1 + 1] - start[b0] > n / 4) return {std::nan(""), std::nan("")};
    dstd::Vector<dstd::Vector<T>> parts(chunks);
    parallel_for(
        static_cast<std::int64_t>(n), GRAIN,
        [&](std::int64_t b, std::int64_t e) {
            dstd::Vector<T>& out = parts[static_cast<std::size_t>(b) / GRAIN];
            for (std::int64_t i = b; i < e; ++i) {
                const std::size_t k = which[static_cast<std::size_t>(i)];
                if (k < b0 || k > b1) continue;
                const T v = x[static_cast<std::size_t>(i)];
                if constexpr (std::is_floating_point_v<T>) {
                    if (std::isnan(v)) continue;
                }
                out.push_back(v);
            }
        });
    dstd::Vector<T> held;
    held.reserve(start[b1 + 1] - start[b0]);
    for (const auto& p : parts) held.insert(held.end(), p.begin(), p.end());
    std::sort(held.begin(), held.end());
    const std::size_t at = lo - start[b0];
    const std::size_t in_range = start[b1 + 1] - start[b0];
    if (at >= held.size() || (at + 1 < in_range && at + 1 >= held.size()))
        return {std::nan(""), std::nan("")};
    return {
        static_cast<double>(held[at]),
        static_cast<double>(at + 1 < held.size() ? held[at + 1] : held[at])};
}

std::uint64_t sortable_i64(std::int64_t v) {
    return static_cast<std::uint64_t>(v) ^ 0x8000000000000000ULL;
}

}  // namespace

double quantile(const Series& v, double q) {
    return quantile(v, q, Interpolation::Linear);
}

namespace {

// -0.0 and 0.0 rank equal, so which one a rank gives depends on the order the
// values arrived in; every quantile reports the zero as 0.0.
double positive_zero(double d) { return d == 0.0 ? 0.0 : d; }

double between_ranks(double a, double b, double frac, Interpolation interp,
                     QuantileForm form) {
    const bool midpoint = interp == Interpolation::Midpoint;
    if (form == QuantileForm::PandasGroup)
        return group_quantile_lerp(a, b, frac, midpoint);
    // Even at a whole rank, as numpy: [1, inf] at 0 is 1 + inf * 0.
    return quantile_lerp(a, b, midpoint ? (frac == 0.0 ? 0.0 : 0.5) : frac);
}

}  // namespace

namespace {

double quantile_by_rank(const Series& v, double q, Interpolation interp,
                        QuantileForm form) {
    if (refuse_non_numeric("quantile", v.type()) || std::isnan(q))
        return std::nan("");
    const std::int64_t nulls = cell_null_count(*v.handle());
    const auto n = static_cast<std::size_t>(v.length() - nulls);
    if (n == 0) return std::nan("");
    double pos = std::clamp(q, 0.0, 1.0) * static_cast<double>(n - 1);
    std::size_t lo = static_cast<std::size_t>(pos);
    double frac = pos - static_cast<double>(lo);
    if (lo + 1 >= n) frac = 0.0;
    const bool two_ranks =
        interp == Interpolation::Linear || interp == Interpolation::Midpoint;
    // A flat Float64 / Int64 column is read in place, nulls skipped by the
    // bitmap; the bucket pass copies only the bucket the position falls in.
    if (two_ranks && n > (std::size_t{1} << 18) &&
        parallel_backend_installed() && v.encoding() == Encoding::Flat &&
        (v.type() == TypeId::Float64 || v.type() == TypeId::Int64)) {
        const auto total = static_cast<std::size_t>(v.length());
        const std::uint8_t* validity = v.handle()->validity.block()
                                           ? v.handle()->validity.data()
                                           : nullptr;
        auto [a, b] = v.type() == TypeId::Float64
                          ? two_order_statistics(v.data<double>(), total,
                                                 validity, lo, sortable_key)
                          : two_order_statistics(v.data<std::int64_t>(), total,
                                                 validity, lo, sortable_i64);
        if (!std::isnan(a))
            return between_ranks(a, lo + 1 < n ? b : a, frac, interp, form);
    }
    // The bucket pass and the select need one flat working array, so a column
    // read in place still gets one buffer of its non-null values; with no
    // nulls it is filled in parallel with no zero fill first.
    if (two_ranks && n > (std::size_t{1} << 18) &&
        parallel_backend_installed() && nulls == 0 &&
        v.encoding() != Encoding::Flat) {
        Scratch<double> dense(n);
        fill_dense_parallel(*v.handle(), dense.data());
        auto [a, b] =
            two_order_statistics(dense.data(), n, nullptr, lo, sortable_key);
        if (!std::isnan(a))
            return between_ranks(a, lo + 1 < n ? b : a, frac, interp, form);
    }
    dstd::Vector<double> x = nonnull_values(v);
    const bool is_float = v.type() == TypeId::Float64 ||
                          v.type() == TypeId::Float32 ||
                          v.type() == TypeId::Float16;
    return quantile_of(x, q, interp, is_float, form);
}

}  // namespace

double quantile(const Series& v, double q, Interpolation interp,
                QuantileForm form) {
    return positive_zero(quantile_by_rank(v, q, interp, form));
}

namespace {

double quantile_ranked(dstd::Vector<double>& x, double q, Interpolation interp,
                       bool may_have_nan, QuantileForm form) {
    const std::size_t n = x.size();
    if (n == 0 || std::isnan(q)) return std::nan("");
    const double pos = std::clamp(q, 0.0, 1.0) * static_cast<double>(n - 1);
    const auto lo = static_cast<std::size_t>(pos);
    double frac = pos - static_cast<double>(lo);
    if (lo + 1 >= n) frac = 0.0;
    const bool two_ranks =
        interp == Interpolation::Linear || interp == Interpolation::Midpoint;
    // The selection keys a NaN above every number and gives up when a rank
    // falls among the NaN cells, so it needs no scan for one.
    if (two_ranks && n > (std::size_t{1} << 18) &&
        parallel_backend_installed()) {
        auto [a, b] =
            two_order_statistics(x.data(), n, nullptr, lo, sortable_key);
        if (!std::isnan(a))
            return between_ranks(a, lo + 1 < n ? b : a, frac, interp, form);
    }
    // NaN ranks above every number: sort the numbers and let the ranks past
    // them read NaN, so the sort never compares a NaN.
    const auto is_nan = [](double d) { return d != d; };
    const std::size_t numbers =
        may_have_nan && std::any_of(x.begin(), x.end(), is_nan)
            ? static_cast<std::size_t>(
                  std::partition(x.begin(), x.end(),
                                 [&](double d) { return !is_nan(d); }) -
                  x.begin())
            : n;
    const std::size_t hi = std::min(lo + 1, n - 1);
    parallel_packed_sort(x.data(), numbers,
                         [](double a, double b) { return a < b; });
    const auto rank = [&](std::size_t i) {
        return i < numbers ? x[i] : std::nan("");
    };
    switch (interp) {
        case Interpolation::Linear:
        case Interpolation::Midpoint:
            return between_ranks(rank(lo), rank(hi), frac, interp, form);
        case Interpolation::Nearest:
            return rank(
                std::min(static_cast<std::size_t>(std::round(pos)), n - 1));
        case Interpolation::Lower:
            return rank(lo);
        case Interpolation::Higher:
            return rank(frac == 0.0 ? lo : hi);
    }
    return std::nan("");
}

}  // namespace

double quantile_of(dstd::Vector<double>& x, double q, Interpolation interp,
                   bool may_have_nan, QuantileForm form) {
    return positive_zero(quantile_ranked(x, q, interp, may_have_nan, form));
}

double median(const Series& v) {
    return quantile(v, 0.5, Interpolation::Midpoint, QuantileForm::PandasGroup);
}

std::int64_t nunique(const Series& v) {
    if (v.length() == 0) return 0;
    if (refuse_nested_value("nunique", v.type())) return 0;
    // One null counts once: a null dictionary entry is a null row too.
    return unique(v).length() - (cell_null_count(*v.handle()) > 0 ? 1 : 0);
}

// The first row of every distinct value, ascending: one null row stands for
// all the nulls. Every type compares as value_ids does, so a float's -0.0 is
// 0.0 and every NaN is one value (as pandas and Polars key a float).
static dstd::Vector<std::int64_t> distinct_first_rows(const Series& f) {
    dstd::Vector<std::int64_t> firsts;
    ValueIds ids;
    value_ids(*f.handle(), ids);
    firsts.assign(ids.first.begin(), ids.first.end());
    return firsts;
}

Series unique(const Series& v, bool maintain_order) {
    if (refuse_nested_value("unique", v.type())) return Series{};
    const Series f =
        v.encoding() == Encoding::Flat ? v.share() : v.materialize();
    const dstd::Vector<std::int64_t> firsts = distinct_first_rows(f);
    Series d = f.take(firsts);
    if (d.encoding() != Encoding::Flat) d = d.materialize();
    if (maintain_order) return d;
    return d.sort(false, false);
}

Series rank(const Series& v, RankMethod method, bool descending, bool pct) {
    const std::int64_t n = v.length();
    if (refuse_nested_value("rank", v.type())) return Series{};
    const Encoding encoding = v.encoding();
    if (encoding != Encoding::Flat &&
        (encoding == Encoding::Chunked || !is_string_column(*v.handle()) ||
         has_constant_layer(*v.handle()))) {
        // One copy: argsort sorts a flat buffer, so the ties read it too.
        const Series flat = v.materialize();
        return flat.valid() ? rank(flat, method, descending, pct) : Series{};
    }
    const dftu_series* h = v.handle();
    const std::int64_t nulls = cell_null_count(*h);
    const bool has_nulls = nulls > 0;
    // A float NaN is missing like a null: rank the present rows alone (so the
    // pct divisor counts neither) and give the missing rows a null rank.
    if (v.type() == TypeId::Float64 || v.type() == TypeId::Float32 ||
        v.type() == TypeId::Float16) {
        dstd::Vector<std::int64_t> keep;
        keep.reserve(static_cast<std::size_t>(n));
        bool has_nan = false;
        for (std::int64_t i = 0; i < n; ++i) {
            if (has_nulls && cell_is_null(h, i)) continue;
            if (std::isnan(cell_f64(h, i))) {
                has_nan = true;
                continue;
            }
            keep.push_back(i);
        }
        if (has_nan) {
            const Series ranked = rank(take(v, keep), method, descending, pct);
            const double* r = ranked.data<double>();
            dstd::Vector<double> out(static_cast<std::size_t>(n), std::nan(""));
            dstd::Vector<std::uint8_t> ok(static_cast<std::size_t>((n + 7) / 8),
                                          0);
            for (std::size_t t = 0; t < keep.size(); ++t) {
                const std::int64_t i = keep[t];
                out[static_cast<std::size_t>(i)] = r[t];
                ok[static_cast<std::size_t>(i >> 3)] |=
                    static_cast<std::uint8_t>(1u << (i & 7));
            }
            return Series::flat(TypeId::Float64, out.data(), n, ok.data());
        }
    }
    dstd::Vector<double> ranks(static_cast<std::size_t>(n), std::nan(""));
    const Series order = argsort(v, descending);  // nulls sort last
    const std::int64_t* idx = order.data<std::int64_t>();
    std::int64_t k = 0, dense = 0;
    while (k < n) {
        std::int64_t i = idx[k];
        if (has_nulls && cell_is_null(h, i)) break;  // nulls keep a NaN rank
        std::int64_t j = k + 1;
        while (j < n) {
            std::int64_t ij = idx[j];
            if (has_nulls && cell_is_null(h, ij)) break;
            if (!equal_at(v, i, ij)) break;
            ++j;
        }
        ++dense;
        for (std::int64_t t = k; t < j; ++t) {
            double r = 0.0;
            switch (method) {
                case RankMethod::Ordinal:
                    r = static_cast<double>(t + 1);
                    break;
                case RankMethod::Min:
                    r = static_cast<double>(k + 1);
                    break;
                case RankMethod::Max:
                    r = static_cast<double>(j);
                    break;
                case RankMethod::Dense:
                    r = static_cast<double>(dense);
                    break;
                case RankMethod::Average:
                    r = static_cast<double>(k + 1 + j) / 2.0;
                    break;
            }
            ranks[static_cast<std::size_t>(idx[t])] = r;
        }
        k = j;
    }
    if (pct) {
        // pandas divides a dense rank by the number of distinct values (its
        // largest rank), every other method by the non-null count; either way
        // the largest value ranks 1.0.
        const double divisor = method == RankMethod::Dense
                                   ? static_cast<double>(dense)
                                   : static_cast<double>(n - nulls);
        for (std::int64_t i = 0; i < n; ++i)
            if (!has_nulls || !cell_is_null(h, i))
                ranks[static_cast<std::size_t>(i)] /= divisor;
    }
    // A null value has a null rank (pandas and polars both), not a NaN.
    if (!has_nulls) return Series::flat(TypeId::Float64, ranks.data(), n);
    dstd::Vector<std::uint8_t> valid(static_cast<std::size_t>((n + 7) / 8), 0);
    for (std::int64_t i = 0; i < n; ++i)
        if (!cell_is_null(h, i))
            valid[static_cast<std::size_t>(i >> 3)] |=
                static_cast<std::uint8_t>(1u << (i & 7));
    return Series::flat(TypeId::Float64, ranks.data(), n, valid.data());
}

namespace {

// pandas min_periods = window: a window holding a null gives a null. Clears
// the valid bit of every row within `window` of a null input.
void null_windows(const Series& v, std::int64_t nulls, std::int64_t window,
                  dstd::Vector<std::uint8_t>& valid) {
    if (nulls == 0) return;
    const std::int64_t n = v.length();
    std::int64_t last_null = -window;
    for (std::int64_t i = 0; i < n; ++i) {
        if (cell_is_null(v.handle(), i)) last_null = i;
        if (i - last_null < window)
            valid[static_cast<std::size_t>(i >> 3)] &=
                static_cast<std::uint8_t>(~(1u << (i & 7)));
    }
}

// Every row of `v` as a double, with `nulls` set to its null rows: one copy,
// as the windows index a plain array. A null row is 0, since a NaN or an inf
// under it would stay in a running sum past its window.
Scratch<double> window_values(const Series& v, std::int64_t& nulls) {
    const dftu_series& h = *v.handle();
    Scratch<double> out(static_cast<std::size_t>(h.length));
    const bool bulk = h.encoding == Encoding::Flat || h.is_chunked();
    if (bulk) {
        nulls = cell_null_count(h);
        if (nulls == 0) {
            if (h.is_chunked())
                fill_dense_parallel(h, out.data());
            else
                fill_dense(h, out.data());
            return out;
        }
    }
    nulls = parallel_reduce<std::int64_t>(
        h.length, std::int64_t{1} << 16, 0,
        [&](std::int64_t b, std::int64_t e) {
            std::int64_t missing = 0;
            for (std::int64_t i = b; i < e; ++i) {
                double x = 0.0;
                missing += !try_cell_f64(&h, i, x);
                out[static_cast<std::size_t>(i)] = x;
            }
            return missing;
        },
        [](std::int64_t a, std::int64_t b) { return a + b; });
    return out;
}

}  // namespace

Series rolling(const Series& v, std::int64_t window, RollingOp op) {
    if (refuse_non_numeric("rolling", v.type())) return Series{};
    const std::int64_t n = v.length();
    if (window < 1) window = 1;
    dstd::Vector<std::uint8_t> valid(static_cast<std::size_t>((n + 7) / 8), 0);
    // Every full window (rows [window-1, n)) is valid; set that contiguous run
    // in bulk (byte memset for the interior) instead of one bit per row.
    {
        std::int64_t a = window - 1;
        if (a < 0) a = 0;
        std::int64_t i = a;
        for (; i < n && (i & 7); ++i)
            valid[static_cast<std::size_t>(i >> 3)] |=
                static_cast<std::uint8_t>(1u << (i & 7));
        const std::int64_t full_end = n & ~std::int64_t{7};
        if (i < full_end) {
            std::memset(&valid[static_cast<std::size_t>(i >> 3)], 0xFF,
                        static_cast<std::size_t>((full_end - i) / 8));
            i = full_end;
        }
        for (; i < n; ++i)
            valid[static_cast<std::size_t>(i >> 3)] |=
                static_cast<std::uint8_t>(1u << (i & 7));
    }

    // A dense FLAT Float64 column is read in place.
    std::int64_t nulls = 0;
    Scratch<double> vals;
    const double* p = nullptr;
    if (n > 0 && v.encoding() == Encoding::Flat &&
        v.type() == TypeId::Float64 && v.null_count() == 0) {
        p = v.data<double>();
    } else {
        vals = window_values(v, nulls);
        p = vals.data();
    }

    // The output written in place; a chunk warms its window up from the
    // `window - 1` rows before it, so the chunks run in parallel.
    auto* res = new dftu_series();
    res->type = TypeId::Float64;
    res->encoding = Encoding::Flat;
    res->length = n;
    res->data = new_buffer(static_cast<std::size_t>(n) * sizeof(double));
    double* out = reinterpret_cast<double*>(writable(res->data));
    constexpr std::int64_t GRAIN_ROWS = std::int64_t{1} << 16;
    const std::int64_t grain = std::max(GRAIN_ROWS, window * 4);
    if (op == RollingOp::Sum || op == RollingOp::Mean) {
        parallel_for(n, grain, [&](std::int64_t b, std::int64_t e) {
            // Kahan-compensated adds and removes, as pandas' rolling sum:
            // a plain running sum drifts by the window's magnitude on a
            // long series and no longer rounds like a fresh sum would.
            double run = 0.0, add_c = 0.0, sub_c = 0.0;
            const auto kahan = [&run](double x, double& c) {
                const double y = x - c;
                const double t = run + y;
                c = (t - run) - y;
                run = t;
            };
            const std::int64_t i0 = std::max<std::int64_t>(0, b - window + 1);
            for (std::int64_t i = i0; i < e; ++i) {
                kahan(p[i], add_c);
                if (i - window >= i0) kahan(-p[i - window], sub_c);
                if (i >= b) {
                    if (i >= window - 1)
                        out[i] = op == RollingOp::Mean
                                     ? run / static_cast<double>(window)
                                     : run;
                    else
                        out[i] = 0.0;
                }
            }
        });
    } else {
        // Monotonic deque of indices: front holds the window extreme.
        const bool is_max = op == RollingOp::Max;
        parallel_for(n, grain, [&](std::int64_t b, std::int64_t e) {
            dstd::Deque<std::int64_t> dq;
            for (std::int64_t i = std::max<std::int64_t>(0, b - window + 1);
                 i < e; ++i) {
                const double x = p[i];
                while (!dq.empty()) {
                    const double back = p[dq.back()];
                    if (is_max ? (back <= x) : (back >= x))
                        dq.pop_back();
                    else
                        break;
                }
                dq.push_back(i);
                if (dq.front() <= i - window) dq.pop_front();
                if (i >= b) out[i] = i >= window - 1 ? p[dq.front()] : 0.0;
            }
        });
    }
    null_windows(v, nulls, window, valid);
    res->null_count = 0;
    for (std::int64_t i = 0; i < n; ++i)
        if (!((valid[static_cast<std::size_t>(i >> 3)] >> (i & 7)) & 1))
            ++res->null_count;
    if (res->null_count > 0) {
        res->validity = new_buffer(valid.size());
        std::memcpy(writable(res->validity), valid.data(), valid.size());
    }
    return Series{res};
}

namespace {

// Quantile (linear interpolation between the two nearest ranks) of a scratch
// vector; the caller may leave it unsorted (this sorts in place). NaN if empty.
double quantile_sorted(dstd::Vector<double>& x, double q) {
    const std::size_t n = x.size();
    if (n == 0) return std::nan("");
    std::sort(x.begin(), x.end());
    if (q <= 0.0) return x.front();
    if (q >= 1.0) return x.back();
    double pos = q * static_cast<double>(n - 1);
    std::size_t lo = static_cast<std::size_t>(pos);
    double frac = pos - static_cast<double>(lo);
    if (lo + 1 >= n) return x[lo];
    return quantile_lerp(x[lo], x[lo + 1], frac);
}

// Sample variance of a scratch window (divide by n-1); 0 for fewer than 2.
double window_sample_var(dstd::Span<const double> x) {
    const std::size_t n = x.size();
    if (n < 2) return 0.0;
    double mean = 0.0;
    for (double v : x) mean += v;
    mean /= static_cast<double>(n);
    double s2 = 0.0;
    for (double v : x) {
        double d = v - mean;
        s2 += d * d;
    }
    return s2 / static_cast<double>(n - 1);
}

// Materialize each trailing `window` as a scratch vector and reduce it with
// `agg`; the first window-1 rows and every window holding a null are null. Rows
// are independent (each reads only its own trailing window), so above a size
// threshold and with a backend installed the row range fans out through
// parallel_for; the valid mask is filled in bulk first so worker threads only
// ever touch their own disjoint `out` slots.
template <class Agg>
Series rolling_window(const Series& v, std::int64_t window, Agg agg,
                      dstd::StringView op_name) {
    if (refuse_non_numeric(op_name, v.type())) return Series{};
    const std::int64_t n = v.length();
    if (window < 1) window = 1;
    dstd::Vector<double> out(static_cast<std::size_t>(n), 0.0);
    dstd::Vector<std::uint8_t> valid(static_cast<std::size_t>((n + 7) / 8), 0);
    {
        std::int64_t a = std::max<std::int64_t>(window - 1, 0);
        std::int64_t i = a;
        for (; i < n && (i & 7); ++i)
            valid[static_cast<std::size_t>(i >> 3)] |=
                static_cast<std::uint8_t>(1u << (i & 7));
        const std::int64_t full_end = n & ~std::int64_t{7};
        if (i < full_end) {
            std::memset(&valid[static_cast<std::size_t>(i >> 3)], 0xFF,
                        static_cast<std::size_t>((full_end - i) / 8));
            i = full_end;
        }
        for (; i < n; ++i)
            valid[static_cast<std::size_t>(i >> 3)] |=
                static_cast<std::uint8_t>(1u << (i & 7));
    }

    std::int64_t nulls = 0;
    const Scratch<double> vals = window_values(v, nulls);
    const double* p = vals.data();

    const std::int64_t begin = std::min<std::int64_t>(window - 1, n);
    const std::int64_t rows = n - begin;
    constexpr std::int64_t GRAIN = 1 << 12;
    if (parallel_backend_installed() && rows > GRAIN) {
        parallel_for(rows, GRAIN, [&](std::int64_t b, std::int64_t e) {
            dstd::Vector<double> buf;
            buf.reserve(static_cast<std::size_t>(window));
            for (std::int64_t i = begin + b; i < begin + e; ++i) {
                buf.clear();
                for (std::int64_t j = i - window + 1; j <= i; ++j)
                    buf.push_back(p[j]);
                out[static_cast<std::size_t>(i)] = agg(buf);
            }
        });
    } else {
        dstd::Vector<double> buf;
        buf.reserve(static_cast<std::size_t>(window));
        for (std::int64_t i = begin; i < n; ++i) {
            buf.clear();
            for (std::int64_t j = i - window + 1; j <= i; ++j)
                buf.push_back(p[j]);
            out[static_cast<std::size_t>(i)] = agg(buf);
        }
    }
    null_windows(v, nulls, window, valid);
    return Series::flat(TypeId::Float64, out.data(), n, valid.data());
}

}  // namespace

Series rolling_var(const Series& v, std::int64_t window) {
    return rolling_window(
        v, window,
        [](dstd::Span<const double> w) { return window_sample_var(w); },
        "rolling_var");
}

Series rolling_std(const Series& v, std::int64_t window) {
    return rolling_window(
        v, window,
        [](dstd::Span<const double> w) {
            return std::sqrt(window_sample_var(w));
        },
        "rolling_std");
}

Series rolling_median(const Series& v, std::int64_t window) {
    return rolling_window(
        v, window,
        [](dstd::Vector<double>& w) {
            return quantile_of(w, 0.5, Interpolation::Midpoint, true,
                               QuantileForm::PandasGroup);
        },
        "rolling_median");
}

Series rolling_quantile(const Series& v, std::int64_t window, double q) {
    return rolling_window(
        v, window,
        [q](dstd::Vector<double>& w) {
            return positive_zero(quantile_sorted(w, q));
        },
        "rolling_quantile");
}

// Two chunk-transform composers shared by the EWM family: `y -> a*y + c`
// applied first, then `y -> a*y + c` applied second, composes to
// `y -> (a1*a2)*y + (a2*c1 + c2)`.
using AffineT = dstd::Pair<double, double>;

static AffineT affine_compose(AffineT lhs, AffineT rhs) {
    return AffineT{lhs.first * rhs.first, rhs.first * lhs.second + rhs.second};
}

// Parallel prefix scan for out[i] = a*out[i-1] + c(i), out[0] = c(0), where
// the coefficient `a` is the same at every step (ewm_mean's recurrence, and
// ewm_std's variance recurrence). Chunks scan locally from seed 0, then a
// single a^k correction folds in the true entering value - cheap because `a`
// never changes within a chunk. A chunk containing index 0 needs no
// correction: c(0) fixes the true value outright, independent of any seed.
template <class C>
void affine_scan_const_a(std::int64_t n, std::int64_t grain, double a, C&& c,
                         dstd::Vector<double>& out) {
    if (n == 0) return;
    if (!parallel_backend_installed() || n <= grain) {
        double y = c(0);
        out[0] = y;
        for (std::int64_t i = 1; i < n; ++i) {
            y = a * y + c(i);
            out[static_cast<std::size_t>(i)] = y;
        }
        return;
    }
    parallel_prefix_scan<AffineT>(
        n, grain, AffineT{1.0, 0.0},
        [&](std::int64_t b, std::int64_t e) -> AffineT {
            double y = (b == 0) ? c(0) : 0.0;
            if (b == 0) out[0] = y;
            std::int64_t i0 = (b == 0) ? 1 : b;
            for (std::int64_t i = i0; i < e; ++i) {
                y = a * y + c(i);
                out[static_cast<std::size_t>(i)] = y;
            }
            if (b == 0) return AffineT{0.0, y};
            return AffineT{std::pow(a, e - b), y};
        },
        [&](std::int64_t b, std::int64_t e, AffineT offset) {
            if (b == 0) return;
            const double v_pre = offset.second;
            double p = a;
            for (std::int64_t i = b; i < e; ++i) {
                out[static_cast<std::size_t>(i)] += p * v_pre;
                p *= a;
            }
        },
        affine_compose);
}

// Parallel prefix scan for out[i] = a(i)*out[i-1] + c(i), out[0] = c(0),
// where the coefficient may vary by index but is known independent of any
// other row's value (ewm_std's running mean, whose coefficient depends only
// on the precomputed cumulative weight). Unlike the constant-a case, a
// single a^k correction does not exist, so each chunk records its per-index
// cumulative product of a(i) in `aprod` for the later correction pass to
// multiply against the true entering value.
template <class A, class C>
void affine_scan_var_a(std::int64_t n, std::int64_t grain, A&& a, C&& c,
                       dstd::Vector<double>& out, dstd::Vector<double>& aprod) {
    if (n == 0) return;
    if (!parallel_backend_installed() || n <= grain) {
        double y = c(0);
        out[0] = y;
        for (std::int64_t i = 1; i < n; ++i) {
            y = a(i) * y + c(i);
            out[static_cast<std::size_t>(i)] = y;
        }
        return;
    }
    parallel_prefix_scan<AffineT>(
        n, grain, AffineT{1.0, 0.0},
        [&](std::int64_t b, std::int64_t e) -> AffineT {
            double y = (b == 0) ? c(0) : 0.0;
            if (b == 0) out[0] = y;
            std::int64_t i0 = (b == 0) ? 1 : b;
            double aacc = 1.0;
            for (std::int64_t i = i0; i < e; ++i) {
                const double ai = a(i);
                y = ai * y + c(i);
                out[static_cast<std::size_t>(i)] = y;
                aacc *= ai;
                aprod[static_cast<std::size_t>(i)] = aacc;
            }
            if (b == 0) return AffineT{0.0, y};
            return AffineT{aacc, y};
        },
        [&](std::int64_t b, std::int64_t e, AffineT offset) {
            if (b == 0) return;
            const double v_pre = offset.second;
            for (std::int64_t i = b; i < e; ++i)
                out[static_cast<std::size_t>(i)] +=
                    aprod[static_cast<std::size_t>(i)] * v_pre;
        },
        affine_compose);
}

// pandas ewm(adjust=True, ignore_na=False): at row i observation j carries
// weight decay^(i-j) whether or not the rows between were null; a null row
// adds no observation and repeats the previous value; a Float64 column.
Series ewm_mean(const Series& v, double alpha) {
    if (refuse_non_numeric("ewm_mean", v.type())) return Series{};
    const std::int64_t n = v.length();
    dstd::Vector<double> out(static_cast<std::size_t>(n), 0.0);
    if (n == 0) return Series::flat(TypeId::Float64, out.data(), n);
    const double decay = 1.0 - alpha;
    constexpr std::int64_t GRAIN = 1 << 14;
    const dftu_series* h = v.handle();
    if (parallel_backend_installed() && n > GRAIN && alpha > 0.0 &&
        cell_null_count(*h) == 0) {
        // w_sum[i] = sum_{k=0}^{i} decay^k, a closed form per index, so
        // mean[i] = mean[i-1] * (1 - 1 / w_sum[i]) + x[i] / w_sum[i] is an
        // affine scan with an index-only coefficient.
        dstd::Vector<double> w_sum(static_cast<std::size_t>(n), 1.0);
        parallel_for(n, GRAIN, [&](std::int64_t b, std::int64_t e) {
            for (std::int64_t i = std::max<std::int64_t>(b, 1); i < e; ++i)
                w_sum[static_cast<std::size_t>(i)] =
                    (1.0 - std::pow(decay, i + 1)) / (1.0 - decay);
        });
        dstd::Vector<double> aprod(static_cast<std::size_t>(n), 0.0);
        affine_scan_var_a(
            n, GRAIN,
            [&](std::int64_t i) {
                return 1.0 - 1.0 / w_sum[static_cast<std::size_t>(i)];
            },
            [&](std::int64_t i) {
                return i == 0 ? cell_f64(h, 0)
                              : cell_f64(h, i) /
                                    w_sum[static_cast<std::size_t>(i)];
            },
            out, aprod);
        return Series::flat(TypeId::Float64, out.data(), n);
    }
    dstd::Vector<std::uint8_t> valid(static_cast<std::size_t>((n + 7) / 8), 0);
    const auto read = present_rows(v);
    double w_sum = 0.0, mean = 0.0;
    for (std::int64_t i = 0; i < n; ++i) {
        w_sum *= decay;
        double x = 0.0;
        if (read(i, x)) {
            w_sum += 1.0;
            mean = w_sum == 1.0 ? x : mean + (x - mean) / w_sum;
        }
        if (w_sum > 0.0) {
            out[static_cast<std::size_t>(i)] = mean;
            valid[static_cast<std::size_t>(i >> 3)] |=
                static_cast<std::uint8_t>(1u << (i & 7));
        }
    }
    return Series::flat(TypeId::Float64, out.data(), n, valid.data());
}

Series ewm_std(const Series& v, double alpha) {
    if (refuse_non_numeric("ewm_std", v.type())) return Series{};
    const std::int64_t n = v.length();
    dstd::Vector<double> out(static_cast<std::size_t>(n), 0.0);
    dstd::Vector<std::uint8_t> valid(static_cast<std::size_t>((n + 7) / 8), 0);
    if (n == 0) return Series::flat(TypeId::Float64, out.data(), n);
    // The variance is unchanged by a shift, so run the recurrences on the
    // values minus the first valid one: a running mean of values near 1e9 would
    // lose the digits of a spread of 1 in a single double.
    const dftu_series* h = v.handle();
    const auto read = present_rows(v);
    double x0 = 0.0;
    for (std::int64_t i = 0; i < n; ++i)
        if (read(i, x0)) break;
    // Incremental reliability-weight EW variance: at step i observation j
    // carries weight decay^(i-j). w_sum tracks the total weight, w2_sum the
    // sum of squared weights, and s the weighted sum of squared deviations;
    // the debiased sample variance is s / (w_sum - w2_sum / w_sum).
    const double decay = 1.0 - alpha;
    constexpr std::int64_t GRAIN = 1 << 14;
    // decay == 1 (alpha == 0) breaks the closed-form geometric sum below
    // (division by 1 - decay); that case is rare enough to just run serial.
    if (parallel_backend_installed() && n > GRAIN && alpha > 0.0 &&
        cell_null_count(*h) == 0) {
        // w_sum[i] and w2_sum[i] solve the same two recurrences as below but
        // depend only on i, not on any row's value, so each index is a
        // closed-form geometric sum computable independently in parallel:
        // w_sum[i] = sum_{k=0}^{i} decay^k, w2_sum[i] = sum_{k=0}^{i} decay^2k.
        dstd::Vector<double> w_sum(static_cast<std::size_t>(n), 1.0);
        dstd::Vector<double> w2_sum(static_cast<std::size_t>(n), 1.0);
        const double decay2 = decay * decay;
        parallel_for(n, GRAIN, [&](std::int64_t b, std::int64_t e) {
            for (std::int64_t i = std::max<std::int64_t>(b, 1); i < e; ++i) {
                w_sum[static_cast<std::size_t>(i)] =
                    (1.0 - std::pow(decay, i + 1)) / (1.0 - decay);
                w2_sum[static_cast<std::size_t>(i)] =
                    (1.0 - std::pow(decay2, i + 1)) / (1.0 - decay2);
            }
        });

        // mean[i] = mean[i-1]*(1 - 1/w_sum[i]) + x[i]/w_sum[i]: an affine
        // scan whose coefficient depends only on i (via w_sum), so it runs
        // through the variable-coefficient scan above.
        dstd::Vector<double> mean(static_cast<std::size_t>(n), 0.0);
        dstd::Vector<double> aprod(static_cast<std::size_t>(n), 0.0);
        affine_scan_var_a(
            n, GRAIN,
            [&](std::int64_t i) {
                return 1.0 - 1.0 / w_sum[static_cast<std::size_t>(i)];
            },
            [&](std::int64_t i) {
                return i == 0 ? (cell_f64(h, 0) - x0)
                              : (cell_f64(h, i) - x0) /
                                    w_sum[static_cast<std::size_t>(i)];
            },
            mean, aprod);

        // s[i] = decay*s[i-1] + delta*(x[i]-mean[i]), delta = x[i]-mean[i-1]:
        // constant coefficient `decay`, so it reuses the ewm_mean-style scan.
        affine_scan_const_a(
            n, GRAIN, decay,
            [&](std::int64_t i) {
                if (i == 0) return 0.0;
                const double x = (cell_f64(h, i) - x0);
                const double delta = x - mean[static_cast<std::size_t>(i - 1)];
                return delta * (x - mean[static_cast<std::size_t>(i)]);
            },
            out);

        parallel_for(n, GRAIN, [&](std::int64_t b, std::int64_t e) {
            for (std::int64_t i = std::max<std::int64_t>(b, 1); i < e; ++i) {
                const std::size_t ui = static_cast<std::size_t>(i);
                const double denom = w_sum[ui] - w2_sum[ui] / w_sum[ui];
                if (denom > 0.0) {
                    out[ui] = std::sqrt(out[ui] / denom);
                    valid[static_cast<std::size_t>(i >> 3)] |=
                        static_cast<std::uint8_t>(1u << (i & 7));
                } else {
                    out[ui] = 0.0;
                }
            }
        });
        return Series::flat(TypeId::Float64, out.data(), n, valid.data());
    }
    // The serial scan, null-aware: a null row decays the weights, adds no
    // observation and repeats the previous std.
    double w_sum = 0.0, w2_sum = 0.0, mean = 0.0, s = 0.0;
    bool have = false;
    for (std::int64_t i = 0; i < n; ++i) {
        w_sum *= decay;
        w2_sum *= decay * decay;
        double x = 0.0;
        if (read(i, x)) {
            x -= x0;
            w_sum += 1.0;
            w2_sum += 1.0;
            if (w_sum == 1.0) {
                mean = x;
            } else {
                const double delta = x - mean;
                mean += delta / w_sum;
                s = decay * s + delta * (x - mean);
            }
        }
        const double denom = w_sum - (w_sum > 0.0 ? w2_sum / w_sum : 0.0);
        if (denom > 0.0) {
            out[static_cast<std::size_t>(i)] = std::sqrt(s / denom);
            have = true;
        }
        if (have)
            valid[static_cast<std::size_t>(i >> 3)] |=
                static_cast<std::uint8_t>(1u << (i & 7));
        if (have && denom <= 0.0)
            out[static_cast<std::size_t>(i)] =
                out[static_cast<std::size_t>(i - 1)];
    }
    return Series::flat(TypeId::Float64, out.data(), n, valid.data());
}

namespace {

// True when the column reads exactly as int64 (signed integers, Bool and the
// Int64-backed temporal types), so a cut can compare without a double.
bool exact_int_column(const Series& c) {
    switch (physical_type(c.type())) {
        case TypeId::Bool:
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
            return true;
        default:
            return false;
    }
}

// The bin of each value: the count of breaks below it (right-closed) or at or
// below it. Values that arrive in ascending order walk one pointer along the
// sorted edges; a value below its predecessor falls back to a binary search.
template <class T, class Read>
Series cut_with(const Series& v, dstd::Span<const T> edges, bool right,
                bool inner, Read read) {
    const std::int64_t n = v.length();
    const std::int64_t nb = static_cast<std::int64_t>(edges.size());
    dstd::Vector<std::int32_t> out(static_cast<std::size_t>(n), -1);
    dstd::Vector<std::uint8_t> valid(static_cast<std::size_t>((n + 7) / 8), 0);
    std::int64_t at = 0;
    bool have_prev = false;
    T prev{};
    std::uint8_t acc = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        T x{};
        if (read(i, x)) {
            if (have_prev && x >= prev) {
                if (right)
                    while (at < nb && edges[static_cast<std::size_t>(at)] < x)
                        ++at;
                else
                    while (at < nb && edges[static_cast<std::size_t>(at)] <= x)
                        ++at;
            } else {
                auto it = right
                              ? std::lower_bound(edges.begin(), edges.end(), x)
                              : std::upper_bound(edges.begin(), edges.end(), x);
                at = it - edges.begin();
            }
            prev = x;
            have_prev = true;
            std::int32_t bin = static_cast<std::int32_t>(at);
            bool in_range = true;
            if (inner) {
                in_range = bin >= 1 && bin < static_cast<std::int32_t>(nb);
                --bin;
            }
            if (in_range) {
                out[static_cast<std::size_t>(i)] = bin;
                acc |= static_cast<std::uint8_t>(1u << (i & 7));
            }
        }
        if ((i & 7) == 7) {
            valid[static_cast<std::size_t>(i >> 3)] = acc;
            acc = 0;
        }
    }
    if ((n & 7) != 0) valid[static_cast<std::size_t>(n >> 3)] = acc;
    return Series::flat(TypeId::Int32, out.data(), n, valid.data());
}

}  // namespace

Series cut(const Series& v, const Series& breaks, std::int32_t flags) {
    if (refuse_non_numeric("cut", v.type())) return Series{};
    if ((flags & ~(DFTU_CUT_RIGHT | DFTU_CUT_INNER)) != 0) return Series{};
    const bool right = (flags & DFTU_CUT_RIGHT) != 0;
    const bool inner = (flags & DFTU_CUT_INNER) != 0;
    const std::int64_t nb = breaks.length();
    const dftu_series* h = v.handle();
    const dftu_series* bh = breaks.handle();
    // pandas takes only increasing edges, so a null edge is refused.
    if (cell_null_count(*bh) > 0) {
        DFTRACER_UTILS_LOG_ERROR("cut: a break is null");
        return Series{};
    }
    if (exact_int_column(v) && exact_int_column(breaks)) {
        // A copy of the breaks: the binary search needs them sorted.
        dstd::Vector<std::int64_t> edges(static_cast<std::size_t>(nb));
        for (std::int64_t k = 0; k < nb; ++k)
            edges[static_cast<std::size_t>(k)] = cell_i64(bh, k);
        std::sort(edges.begin(), edges.end());
        const dstd::Span<const std::int64_t> span(edges.data(), edges.size());
        if (physical_type(v.type()) == TypeId::Int64 &&
            v.encoding() == Encoding::Flat) {
            const std::int64_t* raw = v.data<std::int64_t>();
            const std::uint8_t* valid =
                h->validity.block() ? h->validity.data() : nullptr;
            return cut_with(v, span, right, inner,
                            [raw, valid](std::int64_t i, std::int64_t& x) {
                                x = raw[i];
                                return !valid ||
                                       ((valid[i >> 3] >> (i & 7)) & 1);
                            });
        }
        return cut_with(v, span, right, inner,
                        [h](std::int64_t i, std::int64_t& x) {
                            return try_cell_i64(h, i, x);
                        });
    }
    // A copy of the breaks: the binary search needs them sorted.
    dstd::Vector<double> edges(static_cast<std::size_t>(nb));
    for (std::int64_t k = 0; k < nb; ++k)
        edges[static_cast<std::size_t>(k)] = cell_f64(bh, k);
    std::sort(edges.begin(), edges.end());
    const dstd::Span<const double> span(edges.data(), edges.size());
    return cut_with(v, span, right, inner, [h](std::int64_t i, double& x) {
        return try_cell_f64(h, i, x);
    });
}

Series qcut(const Series& v, std::int32_t q) {
    if (refuse_non_numeric("qcut", v.type())) return Series{};
    if (q < 1) q = 1;
    dstd::Vector<double> vals = nonnull_values(v);
    dstd::Vector<double> edges;
    edges.reserve(static_cast<std::size_t>(q - 1));
    for (std::int32_t k = 1; k < q; ++k)
        edges.push_back(quantile_sorted(
            vals, static_cast<double>(k) / static_cast<double>(q)));
    // nonnull_values order does not matter to quantile_sorted (it sorts a
    // copy's contents in place, so successive edges stay consistent).
    std::sort(edges.begin(), edges.end());
    const std::int64_t n = v.length();
    dstd::Vector<std::int32_t> out(static_cast<std::size_t>(n), -1);
    dstd::Vector<std::uint8_t> valid(static_cast<std::size_t>((n + 7) / 8), 0);
    const dftu_series* h = v.handle();
    for (std::int64_t i = 0; i < n; ++i) {
        double x = 0.0;
        if (!try_cell_f64(h, i, x)) continue;
        auto it = std::upper_bound(edges.begin(), edges.end(), x);
        out[static_cast<std::size_t>(i)] =
            static_cast<std::int32_t>(it - edges.begin());
        valid[static_cast<std::size_t>(i >> 3)] |=
            static_cast<std::uint8_t>(1u << (i & 7));
    }
    return Series::flat(TypeId::Int32, out.data(), n, valid.data());
}

Series search_sorted(const Series& v, const Series& values, bool right) {
    if (refuse_non_numeric("search_sorted", v.type())) return Series{};
    if (refuse_non_numeric("search_sorted", values.type())) return Series{};
    // A null sorts after every value, as NumPy sorts NaN; the value under it
    // is never read.
    using Key = dstd::Pair<bool, double>;
    const auto key = [](const Series& s, std::int64_t i) {
        double x = 0.0;
        const bool present = try_cell_f64(s.handle(), i, x);
        return Key{!present, x};
    };
    const std::int64_t n = v.length();
    // A copy of `v` as keys: the binary searches index one plain array.
    dstd::Vector<Key> self(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i)
        self[static_cast<std::size_t>(i)] = key(v, i);
    const std::int64_t m = values.length();
    dstd::Vector<std::int64_t> out(static_cast<std::size_t>(m), 0);
    for (std::int64_t k = 0; k < m; ++k) {
        const Key qv = key(values, k);
        auto it = right ? std::upper_bound(self.begin(), self.end(), qv)
                        : std::lower_bound(self.begin(), self.end(), qv);
        out[static_cast<std::size_t>(k)] =
            static_cast<std::int64_t>(it - self.begin());
    }
    return Series::flat(TypeId::Int64, out.data(), m);
}

Series interpolate(const Series& v) {
    if (refuse_non_numeric("interpolate", v.type())) return Series{};
    const std::int64_t n = v.length();
    const dftu_series* h = v.handle();
    // Fast path: no nulls -> a plain widening copy.
    if (cell_null_count(*h) == 0) {
        const dstd::Vector<double> widened = nonnull_values(v);
        return Series::flat(TypeId::Float64, widened.data(), n);
    }
    dstd::Vector<double> out(static_cast<std::size_t>(n), 0.0);
    dstd::Vector<std::uint8_t> valid(static_cast<std::size_t>((n + 7) / 8), 0);
    auto mark = [&](std::int64_t i) {
        valid[static_cast<std::size_t>(i >> 3)] |=
            static_cast<std::uint8_t>(1u << (i & 7));
    };
    // Collect anchors (non-null rows) first; the null runs between
    // consecutive anchors are then independent of each other, so they can be
    // filled in parallel once the anchor list (and its values) is known.
    dstd::Vector<std::int64_t> anchors;
    for (std::int64_t i = 0; i < n; ++i) {
        double x = 0.0;
        if (!try_cell_f64(h, i, x)) continue;
        out[static_cast<std::size_t>(i)] = x;
        mark(i);
        anchors.push_back(i);
    }
    constexpr std::int64_t INTERPOLATE_GRAIN = 1 << 16;
    const std::int64_t npairs = static_cast<std::int64_t>(anchors.size()) - 1;
    if (parallel_backend_installed() && npairs > INTERPOLATE_GRAIN) {
        // Two threads filling adjacent runs can land bits in the same
        // validity byte, so bit-sets here must be atomic (a plain `|=` would
        // race); fold into the plain bitmap once every run is filled.
        dstd::Vector<std::atomic<std::uint8_t>> avalid(valid.size());
        for (std::size_t i = 0; i < valid.size(); ++i) avalid[i] = valid[i];
        auto amark = [&](std::int64_t i) {
            avalid[static_cast<std::size_t>(i >> 3)].fetch_or(
                static_cast<std::uint8_t>(1u << (i & 7)),
                std::memory_order_relaxed);
        };
        parallel_for(
            npairs, INTERPOLATE_GRAIN, [&](std::int64_t b, std::int64_t e) {
                for (std::int64_t k = b; k < e; ++k) {
                    const std::int64_t i0 =
                        anchors[static_cast<std::size_t>(k)];
                    const std::int64_t i1 =
                        anchors[static_cast<std::size_t>(k) + 1];
                    if (i1 - i0 <= 1) continue;
                    const double a = out[static_cast<std::size_t>(i0)];
                    const double cur = out[static_cast<std::size_t>(i1)];
                    const double span = static_cast<double>(i1 - i0);
                    for (std::int64_t j = i0 + 1; j < i1; ++j) {
                        const double t = static_cast<double>(j - i0) / span;
                        out[static_cast<std::size_t>(j)] = a + (cur - a) * t;
                        amark(j);
                    }
                }
            });
        for (std::size_t i = 0; i < valid.size(); ++i)
            valid[i] = avalid[i].load(std::memory_order_relaxed);
    } else {
        for (std::int64_t k = 0; k < npairs; ++k) {
            const std::int64_t i0 = anchors[static_cast<std::size_t>(k)];
            const std::int64_t i1 = anchors[static_cast<std::size_t>(k) + 1];
            if (i1 - i0 <= 1) continue;
            const double a = out[static_cast<std::size_t>(i0)];
            const double cur = out[static_cast<std::size_t>(i1)];
            const double span = static_cast<double>(i1 - i0);
            for (std::int64_t j = i0 + 1; j < i1; ++j) {
                const double t = static_cast<double>(j - i0) / span;
                out[static_cast<std::size_t>(j)] = a + (cur - a) * t;
                mark(j);
            }
        }
    }
    return Series::flat(TypeId::Float64, out.data(), n, valid.data());
}

}  // namespace dftracer::utils::dataframe

double dftu_series_variance(const dftu_series* v, std::int32_t sample) {
    if (!v) return 0.0;
    dftracer::utils::dataframe::Series c{const_cast<dftu_series*>(v)};
    double r = dftracer::utils::dataframe::variance(c, sample != 0);
    c.release();
    return r;
}
double dftu_series_stddev(const dftu_series* v, std::int32_t sample) {
    if (!v) return 0.0;
    dftracer::utils::dataframe::Series c{const_cast<dftu_series*>(v)};
    double r = dftracer::utils::dataframe::stddev(c, sample != 0);
    c.release();
    return r;
}
double dftu_series_skewness(const dftu_series* v) {
    if (!v) return 0.0;
    dftracer::utils::dataframe::Series c{const_cast<dftu_series*>(v)};
    double r = dftracer::utils::dataframe::skewness(c);
    c.release();
    return r;
}
double dftu_series_kurtosis(const dftu_series* v) {
    if (!v) return 0.0;
    dftracer::utils::dataframe::Series c{const_cast<dftu_series*>(v)};
    double r = dftracer::utils::dataframe::kurtosis(c);
    c.release();
    return r;
}
double dftu_series_skewness_unbiased(const dftu_series* v) {
    if (!v) return 0.0;
    dftracer::utils::dataframe::Series c{const_cast<dftu_series*>(v)};
    double r = dftracer::utils::dataframe::skewness_unbiased(c);
    c.release();
    return r;
}
double dftu_series_kurtosis_unbiased(const dftu_series* v) {
    if (!v) return 0.0;
    dftracer::utils::dataframe::Series c{const_cast<dftu_series*>(v)};
    double r = dftracer::utils::dataframe::kurtosis_unbiased(c);
    c.release();
    return r;
}
double dftu_series_quantile(const dftu_series* v, double q) {
    if (!v) return 0.0;
    dftracer::utils::dataframe::Series c{const_cast<dftu_series*>(v)};
    double r = dftracer::utils::dataframe::quantile(c, q);
    c.release();
    return r;
}
double dftu_series_quantile_interp(const dftu_series* v, double q,
                                   dftu_interpolation interp) {
    if (!v) return 0.0;
    if (interp < DFTU_INTERP_LINEAR || interp > DFTU_INTERP_MIDPOINT)
        return std::nan("");
    dftracer::utils::dataframe::Series c{const_cast<dftu_series*>(v)};
    double r = dftracer::utils::dataframe::quantile(
        c, q, static_cast<dftracer::utils::dataframe::Interpolation>(interp));
    c.release();
    return r;
}
std::int64_t dftu_series_nunique(const dftu_series* v) {
    if (!v) return 0;
    dftracer::utils::dataframe::Series c{const_cast<dftu_series*>(v)};
    std::int64_t r = dftracer::utils::dataframe::nunique(c);
    c.release();
    return r;
}
dftu_series* dftu_series_unique(const dftu_series* v) {
    if (!v) return nullptr;
    dftracer::utils::dataframe::Series c{const_cast<dftu_series*>(v)};
    dftracer::utils::dataframe::Series r =
        dftracer::utils::dataframe::unique(c.drop_nulls(), false);
    c.release();
    return r.release();
}
dftu_series* dftu_series_unique_nulls(const dftu_series* v,
                                      std::int32_t maintain_order) {
    if (!v) return nullptr;
    dftracer::utils::dataframe::Series c{const_cast<dftu_series*>(v)};
    dftracer::utils::dataframe::Series r =
        dftracer::utils::dataframe::unique(c, maintain_order != 0);
    c.release();
    return r.release();
}
dftu_series* dftu_series_rank(const dftu_series* v, dftu_rank_method method,
                              std::int32_t flags) {
    if (!v) return nullptr;
    dftracer::utils::dataframe::Series c{const_cast<dftu_series*>(v)};
    dftracer::utils::dataframe::Series r = dftracer::utils::dataframe::rank(
        c, static_cast<dftracer::utils::dataframe::RankMethod>(method),
        (flags & DFTU_RANK_FLAG_DESCENDING) != 0,
        (flags & DFTU_RANK_FLAG_PCT) != 0);
    c.release();
    return r.release();
}
dftu_series* dftu_series_rolling(const dftu_series* v, std::int64_t window,
                                 dftu_rolling_op op) {
    if (!v) return nullptr;
    dftracer::utils::dataframe::Series c{const_cast<dftu_series*>(v)};
    dftracer::utils::dataframe::Series r = dftracer::utils::dataframe::rolling(
        c, window, static_cast<dftracer::utils::dataframe::RollingOp>(op));
    c.release();
    return r.release();
}
dftu_series* dftu_series_rolling_var(const dftu_series* v,
                                     std::int64_t window) {
    if (!v) return nullptr;
    dftracer::utils::dataframe::Series c{const_cast<dftu_series*>(v)};
    dftracer::utils::dataframe::Series r =
        dftracer::utils::dataframe::rolling_var(c, window);
    c.release();
    return r.release();
}
dftu_series* dftu_series_rolling_std(const dftu_series* v,
                                     std::int64_t window) {
    if (!v) return nullptr;
    dftracer::utils::dataframe::Series c{const_cast<dftu_series*>(v)};
    dftracer::utils::dataframe::Series r =
        dftracer::utils::dataframe::rolling_std(c, window);
    c.release();
    return r.release();
}
dftu_series* dftu_series_rolling_median(const dftu_series* v,
                                        std::int64_t window) {
    if (!v) return nullptr;
    dftracer::utils::dataframe::Series c{const_cast<dftu_series*>(v)};
    dftracer::utils::dataframe::Series r =
        dftracer::utils::dataframe::rolling_median(c, window);
    c.release();
    return r.release();
}
dftu_series* dftu_series_rolling_quantile(const dftu_series* v,
                                          std::int64_t window, double q) {
    if (!v) return nullptr;
    dftracer::utils::dataframe::Series c{const_cast<dftu_series*>(v)};
    dftracer::utils::dataframe::Series r =
        dftracer::utils::dataframe::rolling_quantile(c, window, q);
    c.release();
    return r.release();
}
dftu_series* dftu_series_ewm_mean(const dftu_series* v, double alpha) {
    if (!v) return nullptr;
    dftracer::utils::dataframe::Series c{const_cast<dftu_series*>(v)};
    dftracer::utils::dataframe::Series r =
        dftracer::utils::dataframe::ewm_mean(c, alpha);
    c.release();
    return r.release();
}
dftu_series* dftu_series_ewm_std(const dftu_series* v, double alpha) {
    if (!v) return nullptr;
    dftracer::utils::dataframe::Series c{const_cast<dftu_series*>(v)};
    dftracer::utils::dataframe::Series r =
        dftracer::utils::dataframe::ewm_std(c, alpha);
    c.release();
    return r.release();
}
dftu_series* dftu_series_cut(const dftu_series* v, const dftu_series* breaks,
                             std::int32_t flags) {
    if (!v || !breaks) return nullptr;
    dftracer::utils::dataframe::Series c{const_cast<dftu_series*>(v)};
    dftracer::utils::dataframe::Series b{const_cast<dftu_series*>(breaks)};
    dftracer::utils::dataframe::Series r =
        dftracer::utils::dataframe::cut(c, b, flags);
    c.release();
    b.release();
    return r.release();
}
dftu_series* dftu_series_qcut(const dftu_series* v, std::int32_t q) {
    if (!v) return nullptr;
    dftracer::utils::dataframe::Series c{const_cast<dftu_series*>(v)};
    dftracer::utils::dataframe::Series r =
        dftracer::utils::dataframe::qcut(c, q);
    c.release();
    return r.release();
}
dftu_series* dftu_series_search_sorted(const dftu_series* v,
                                       const dftu_series* values) {
    return dftu_series_search_sorted_side(v, values, 0);
}
dftu_series* dftu_series_search_sorted_side(const dftu_series* v,
                                            const dftu_series* values,
                                            std::int32_t right) {
    if (!v || !values) return nullptr;
    dftracer::utils::dataframe::Series c{const_cast<dftu_series*>(v)};
    dftracer::utils::dataframe::Series q{const_cast<dftu_series*>(values)};
    dftracer::utils::dataframe::Series r =
        dftracer::utils::dataframe::search_sorted(c, q, right != 0);
    c.release();
    q.release();
    return r.release();
}
dftu_series* dftu_series_interpolate(const dftu_series* v) {
    if (!v) return nullptr;
    dftracer::utils::dataframe::Series c{const_cast<dftu_series*>(v)};
    dftracer::utils::dataframe::Series r =
        dftracer::utils::dataframe::interpolate(c);
    c.release();
    return r.release();
}
