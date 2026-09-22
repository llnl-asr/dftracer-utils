#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/column_read.h>
#include <dftracer/utils/dataframe/internal/decimal.h>
#include <dftracer/utils/dataframe/internal/float16.h>
#include <dftracer/utils/dataframe/kernels/sort.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <hwy/contrib/sort/vqsort.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>
#include <string_view>
#include <vector>

namespace dftracer::utils::dataframe {

namespace {

// Scalar fallback for strings, nested types, and columns with nulls.
template <class T>
int cmp_numeric(const Series& v, std::int64_t a, std::int64_t b) {
    const T* d = v.data<T>();
    if (d[a] < d[b]) return -1;
    if (d[a] > d[b]) return 1;
    return 0;
}

int cmp_bool(const Series& v, std::int64_t a, std::int64_t b) {
    const std::uint8_t* bits = v.data<std::uint8_t>();
    int xa = (bits[a >> 3] >> (a & 7)) & 1;
    int xb = (bits[b >> 3] >> (b & 7)) & 1;
    return xa - xb;
}

int cmp_row(const Series& v, std::int64_t a, std::int64_t b) {
    // physical_type() routes Date32/Time32 to the Int32 case and
    // Date64/Time64/Timestamp/Duration to Int64, no case added per type;
    // narrow_varwidth_type() routes LargeString to the String case (its own
    // width is Series::string_at's concern, not this switch's).
    switch (narrow_varwidth_type(physical_type(v.type()))) {
        case TypeId::Bool:
            return cmp_bool(v, a, b);
        case TypeId::Int8:
            return cmp_numeric<std::int8_t>(v, a, b);
        case TypeId::Int16:
            return cmp_numeric<std::int16_t>(v, a, b);
        case TypeId::Int32:
            return cmp_numeric<std::int32_t>(v, a, b);
        case TypeId::Int64:
            return cmp_numeric<std::int64_t>(v, a, b);
        case TypeId::Uint8:
            return cmp_numeric<std::uint8_t>(v, a, b);
        case TypeId::Uint16:
            return cmp_numeric<std::uint16_t>(v, a, b);
        case TypeId::Uint32:
            return cmp_numeric<std::uint32_t>(v, a, b);
        case TypeId::Uint64:
            return cmp_numeric<std::uint64_t>(v, a, b);
        case TypeId::Float16: {
            const std::uint16_t* d = v.data<std::uint16_t>();
            const float fa = half_to_float(d[a]), fb = half_to_float(d[b]);
            return fa < fb ? -1 : (fa > fb ? 1 : 0);
        }
        case TypeId::Float32:
            return cmp_numeric<float>(v, a, b);
        case TypeId::Float64:
            return cmp_numeric<double>(v, a, b);
        case TypeId::String:
        case TypeId::Binary:
        case TypeId::FixedSizeBinary: {
            // read_bytes gives the row extent for both the offset-backed and
            // the fixed-width layouts.
            int c = read_bytes(v, a).compare(read_bytes(v, b));
            return c < 0 ? -1 : (c > 0 ? 1 : 0);
        }
        case TypeId::Decimal128: {
            const std::uint8_t* d = v.data<std::uint8_t>();
            return compare_decimal128(d + static_cast<std::size_t>(a) * 16,
                                      d + static_cast<std::size_t>(b) * 16);
        }
        case TypeId::Decimal256: {
            const std::uint8_t* d = v.data<std::uint8_t>();
            return compare_decimal256(d + static_cast<std::size_t>(a) * 32,
                                      d + static_cast<std::size_t>(b) * 32);
        }
        default:
            // Nested and Unknown reach no case: they have no total order, and
            // every entry point refuses them before comparing a row.
            return 0;
    }
}

Series argsort_scalar(const Series& v, bool descending) {
    const std::int64_t n = v.length();
    std::vector<std::int64_t> idx(static_cast<std::size_t>(n));
    std::iota(idx.begin(), idx.end(), 0);
    const bool has_nulls = v.null_count() > 0;
    std::stable_sort(idx.begin(), idx.end(),
                     [&](std::int64_t a, std::int64_t b) {
                         if (has_nulls) {
                             bool na = v.is_null(a), nb = v.is_null(b);
                             if (na || nb) return nb && !na;  // nulls last
                         }
                         int c = cmp_row(v, a, b);
                         return descending ? c > 0 : c < 0;
                     });
    return Series::flat_i64(idx.data(), n);
}

// SIMD fast path. Map each value to an order-preserving unsigned key (signed
// ints flip the sign bit; floats flip the sign bit, or all bits when negative),
// pack (key << width | row-index) into a wider int, and VQSort: sorting by the
// full word orders by key with the index breaking ties, so it is stable.
// Descending inverts the key bits and keeps the index ascending, stable both
// ways.

std::uint32_t sortable_f32(float f) {
    std::uint32_t u = std::bit_cast<std::uint32_t>(f);
    return u ^ ((u >> 31) ? 0xFFFFFFFFu : 0x80000000u);
}
std::uint64_t sortable_f64(double f) {
    std::uint64_t u = std::bit_cast<std::uint64_t>(f);
    return u ^ ((u >> 63) ? ~0ULL : 0x8000000000000000ULL);
}

constexpr std::int64_t PACK_GRAIN = std::int64_t{1} << 16;

// An Int64 index column of `n` rows filled from `at(i)` in parallel, straight
// into the column's buffer.
template <class At>
Series index_column(std::int64_t n, At at) {
    auto* out = new dftu_series();
    out->type = TypeId::Int64;
    out->encoding = Encoding::Flat;
    out->length = n;
    out->data =
        Buffer::allocate(static_cast<std::size_t>(n) * sizeof(std::int64_t));
    auto* d = reinterpret_cast<std::int64_t*>(out->data->data());
    parallel_for(n, PACK_GRAIN, [&](std::int64_t b, std::int64_t e) {
        for (std::int64_t i = b; i < e; ++i) d[i] = at(i);
    });
    return Series{out};
}

// The first k of `num` words in sorted order. Both degenerate ends are kept
// away from highway's partial sort, whose non-vector fallback (every SVE
// target takes it) builds a heap of k + 1 keys: at k == num that reads and
// swaps one key past the end, and at k == 0 it heap-sorts an empty range,
// whose loop index underflows and never terminates.
template <class P>
void sort_first(P* p, std::size_t num, std::size_t k) {
    if (k == 0 || num < 2) return;
    if (k >= num)
        vqsort(p, num);
    else
        hwy::VQPartialSort(p, num, k, hwy::SortAscending());
}

// The first k of n packed words in sorted order, a run at a time: each run
// is packed into a buffer of its own size, partially sorted, and its first
// k kept; the survivors are partially sorted once more. Never holds more
// than the runs in flight plus the survivors.
template <class P, class PackFn, class IndexFn>
Series topk_packed(std::int64_t n, std::int64_t k, PackFn pack,
                   IndexFn index_of) {
    constexpr std::int64_t RUN = std::int64_t{1} << 18;
    const auto nruns = static_cast<std::size_t>((n + RUN - 1) / RUN);
    std::vector<std::vector<P>> cand(nruns);
    parallel_for(static_cast<std::int64_t>(nruns), 1,
                 [&](std::int64_t c0, std::int64_t c1) {
                     std::vector<P> buf;
                     for (std::int64_t c = c0; c < c1; ++c) {
                         const std::int64_t lo = c * RUN;
                         const std::int64_t hi = std::min(n, lo + RUN);
                         buf.resize(static_cast<std::size_t>(hi - lo));
                         for (std::int64_t i = lo; i < hi; ++i)
                             buf[static_cast<std::size_t>(i - lo)] = pack(i);
                         const auto take =
                             static_cast<std::size_t>(std::min(k, hi - lo));
                         sort_first(buf.data(), buf.size(), take);
                         cand[static_cast<std::size_t>(c)].assign(
                             buf.begin(),
                             buf.begin() + static_cast<std::ptrdiff_t>(take));
                     }
                 });
    std::vector<P> merged;
    merged.reserve(static_cast<std::size_t>(k) * nruns);
    for (const std::vector<P>& part : cand)
        merged.insert(merged.end(), part.begin(), part.end());
    const std::size_t kk = std::min(static_cast<std::size_t>(k), merged.size());
    sort_first(merged.data(), merged.size(), kk);
    return index_column(static_cast<std::int64_t>(kk), [&](std::int64_t i) {
        return index_of(merged[static_cast<std::size_t>(i)]);
    });
}

// Pack 32-bit keys with the index into uint64 and (partial-)sort. `k < 0` sorts
// fully; otherwise only the first k indices are produced.
template <class KeyFn>
Series argsort_pack32(std::int64_t n, bool descending, std::int64_t k,
                      KeyFn key_of) {
    auto pack = [&](std::int64_t i) {
        std::uint32_t key = key_of(i);
        if (descending) key = ~key;
        return (static_cast<std::uint64_t>(key) << 32) |
               static_cast<std::uint32_t>(i);
    };
    if (k >= 0 && k < n)
        return topk_packed<std::uint64_t>(n, k, pack, [](std::uint64_t p) {
            return static_cast<std::int64_t>(static_cast<std::uint32_t>(p));
        });
    Scratch<std::uint64_t> packed(static_cast<std::size_t>(n));
    parallel_for(n, PACK_GRAIN, [&](std::int64_t b, std::int64_t e) {
        for (std::int64_t i = b; i < e; ++i)
            packed[static_cast<std::size_t>(i)] = pack(i);
    });
    // The key is the high 32 bits.
    Scratch<std::uint64_t> sorted;
    const bool done =
        n > (std::int64_t{1} << 20) && parallel_backend_installed() &&
        bucket_packed_sort(
            packed.data(), static_cast<std::size_t>(n), std::size_t{1} << 10,
            [](std::uint64_t x) { return x >> 32; }, sorted);
    if (done) {
        packed.swap(sorted);
        sorted.reset();
    } else {
        parallel_packed_sort(
            packed.data(), static_cast<std::size_t>(n),
            [](std::uint64_t a, std::uint64_t b) { return a < b; });
    }
    return index_column(n, [&](std::int64_t i) {
        return static_cast<std::int64_t>(
            static_cast<std::uint32_t>(packed[static_cast<std::size_t>(i)]));
    });
}

// Pack 64-bit keys with the index into uint128 (hi = key, lo = index).
template <class KeyFn>
Series argsort_pack64(std::int64_t n, bool descending, std::int64_t k,
                      KeyFn key_of) {
    auto pack = [&](std::int64_t i) {
        std::uint64_t key = key_of(i);
        if (descending) key = ~key;
        hwy::uint128_t p;
        p.hi = key;
        p.lo = static_cast<std::uint64_t>(i);
        return p;
    };
    if (k >= 0 && k < n)
        return topk_packed<hwy::uint128_t>(
            n, k, pack, [](const hwy::uint128_t& p) {
                return static_cast<std::int64_t>(p.lo);
            });
    Scratch<hwy::uint128_t> packed(static_cast<std::size_t>(n));
    parallel_for(n, PACK_GRAIN, [&](std::int64_t b, std::int64_t e) {
        for (std::int64_t i = b; i < e; ++i)
            packed[static_cast<std::size_t>(i)] = pack(i);
    });
    // The key is `hi`.
    Scratch<hwy::uint128_t> sorted;
    const bool done =
        n > (std::int64_t{1} << 20) && parallel_backend_installed() &&
        bucket_packed_sort(
            packed.data(), static_cast<std::size_t>(n), std::size_t{1} << 10,
            [](const hwy::uint128_t& x) { return x.hi; }, sorted);
    if (done) {
        packed.swap(sorted);
        sorted.reset();
    } else {
        parallel_packed_sort(
            packed.data(), static_cast<std::size_t>(n),
            [](const hwy::uint128_t& a, const hwy::uint128_t& b) {
                return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo);
            });
    }
    return index_column(n, [&](std::int64_t i) {
        return static_cast<std::int64_t>(
            packed[static_cast<std::size_t>(i)].lo);
    });
}

// Try the SIMD path; returns an invalid Series when the type/shape is not
// eligible (variable-width, nested, has nulls, or too many rows to pack). `k`
// is passed through: `k < 0` = full sort, else a partial sort of the first k.
Series argsort_simd(const Series& v, bool descending, std::int64_t k) {
    const std::int64_t n = v.length();
    if (v.null_count() > 0) return Series{};
    const bool wide_index = n > std::numeric_limits<std::uint32_t>::max();

    switch (physical_type(v.type())) {
        case TypeId::Bool: {
            const std::uint8_t* b = v.data<std::uint8_t>();
            if (wide_index) return Series{};
            return argsort_pack32(n, descending, k, [&](std::int64_t i) {
                return static_cast<std::uint32_t>((b[i >> 3] >> (i & 7)) & 1);
            });
        }
        case TypeId::Int8:
            if (wide_index) return Series{};
            return argsort_pack32(n, descending, k, [&](std::int64_t i) {
                return static_cast<std::uint32_t>(
                    static_cast<std::uint8_t>(v.data<std::int8_t>()[i]) ^
                    0x80u);
            });
        case TypeId::Int16:
            if (wide_index) return Series{};
            return argsort_pack32(n, descending, k, [&](std::int64_t i) {
                return static_cast<std::uint32_t>(
                    static_cast<std::uint16_t>(v.data<std::int16_t>()[i]) ^
                    0x8000u);
            });
        case TypeId::Int32:
            if (wide_index) return Series{};
            return argsort_pack32(n, descending, k, [&](std::int64_t i) {
                return static_cast<std::uint32_t>(v.data<std::int32_t>()[i]) ^
                       0x80000000u;
            });
        case TypeId::Uint8:
            if (wide_index) return Series{};
            return argsort_pack32(n, descending, k, [&](std::int64_t i) {
                return static_cast<std::uint32_t>(v.data<std::uint8_t>()[i]);
            });
        case TypeId::Uint16:
            if (wide_index) return Series{};
            return argsort_pack32(n, descending, k, [&](std::int64_t i) {
                return static_cast<std::uint32_t>(v.data<std::uint16_t>()[i]);
            });
        case TypeId::Uint32:
            if (wide_index) return Series{};
            return argsort_pack32(n, descending, k, [&](std::int64_t i) {
                return v.data<std::uint32_t>()[i];
            });
        case TypeId::Float32:
            if (wide_index) return Series{};
            return argsort_pack32(n, descending, k, [&](std::int64_t i) {
                return sortable_f32(v.data<float>()[i]);
            });
        case TypeId::Int64:
            return argsort_pack64(n, descending, k, [&](std::int64_t i) {
                return static_cast<std::uint64_t>(v.data<std::int64_t>()[i]) ^
                       0x8000000000000000ULL;
            });
        case TypeId::Uint64:
            return argsort_pack64(n, descending, k, [&](std::int64_t i) {
                return v.data<std::uint64_t>()[i];
            });
        case TypeId::Float64:
            return argsort_pack64(n, descending, k, [&](std::int64_t i) {
                return sortable_f64(v.data<double>()[i]);
            });
        default:
            return Series{};  // String / nested: scalar
    }
}

}  // namespace

int compare_rows(const Series& v, std::int64_t a, std::int64_t b) {
    return cmp_row(v, a, b);
}

Series argsort(const Series& v, bool descending) {
    if (refuse_nested_value("argsort", v.type())) return Series{};
    Series simd = argsort_simd(v, descending, -1);
    if (simd.valid()) return simd;
    return argsort_scalar(v, descending);
}

Series topk_indices(const Series& v, std::int64_t k, bool largest) {
    if (refuse_nested_value("top_k", v.type())) return Series{};
    const std::int64_t n = v.length();
    k = k < 0 ? 0 : (k > n ? n : k);
    // largest -> descending key order; the first k are the k largest.
    Series simd = argsort_simd(v, largest, k);
    if (simd.valid()) return simd;
    Series full = argsort_scalar(v, largest);  // strings / nulls / nested
    const std::int64_t* idx = full.data<std::int64_t>();
    std::vector<std::int64_t> head(idx, idx + k);
    return Series::flat_i64(head.data(), k);
}

}  // namespace dftracer::utils::dataframe

extern "C" dftu_series* dftu_series_argsort(const dftu_series* v,
                                            int32_t descending) {
    if (!v) return nullptr;
    DFTU_FLAT_INPUT(v, dftu_series_argsort, descending);
    dftracer::utils::dataframe::Series col{const_cast<dftu_series*>(v)};
    dftracer::utils::dataframe::Series out =
        dftracer::utils::dataframe::argsort(col, descending != 0);
    col.release();  // do not free the borrowed input
    return out.release();
}
