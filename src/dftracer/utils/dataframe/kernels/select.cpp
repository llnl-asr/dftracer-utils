// SPDX-License-Identifier: MIT
// Selection / reshape and hash-predicate Series ops that compose the existing
// SIMD primitives (argsort, topk_indices, take, filter) or a single hash pass:
// is_unique/is_duplicated/is_sorted/is_in, drop_nulls, sort, head/tail,
// reverse, shift, top_k/bottom_k, sample. None is a lane kernel: the numeric
// heavy lifting is delegated to argsort/topk_indices (SIMD) and take/filter.

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/column_read.h>
#include <dftracer/utils/dataframe/internal/compare_simd.h>  // pack_flags
#include <dftracer/utils/dataframe/internal/float_key.h>
#include <dftracer/utils/dataframe/internal/radix_dedup.h>   // parallel dedup
#include <dftracer/utils/dataframe/internal/string_reader.h>
#include <dftracer/utils/dataframe/internal/value_ids.h>
#include <dftracer/utils/dataframe/kernels/sort.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/hash/splitmix64.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace dftracer::utils::dataframe {

namespace {

// Borrow a C ABI handle as a Series without owning it (release before return).
Series borrow(const dftu_series* v) {
    return Series{const_cast<dftu_series*>(v)};
}

// A bit-packed Bool column from per-row flags (no validity).
Series bool_from_flags(dstd::Span<const char> flags) {
    const std::int64_t n = static_cast<std::int64_t>(flags.size());
    dstd::Vector<std::uint8_t> packed(static_cast<std::size_t>((n + 7) / 8), 0);
    pack_flags(flags, packed.data());
    return Series::flat(TypeId::Bool, packed.data(), n);
}

// radix_counts_by over the keys of `m` rows. A view walks its layers on every
// read and the count reads each key three times, so a view's keys are first
// read, in parallel, into one array.
template <class Key, class KeyOf>
dstd::Vector<std::int64_t> key_counts(std::int64_t m, bool view, KeyOf key) {
    if (!view) return radix_counts_by<Key>(m, key);
    dstd::Vector<Key> keys(static_cast<std::size_t>(m));
    parallel_for(m, std::int64_t{1} << 16, [&](std::int64_t b, std::int64_t e) {
        for (std::int64_t j = b; j < e; ++j)
            keys[static_cast<std::size_t>(j)] = key(j);
    });
    return radix_counts_by<Key>(
        m, [&](std::int64_t j) { return keys[static_cast<std::size_t>(j)]; });
}

// Per-row occurrence count of each row's own value, keyed in the column's
// domain (nulls all share one group, counted separately). Radix-partitioned
// over the non-null rows, mirroring the eager DataFrame dedup helpers.
dstd::Vector<std::int64_t> count_values(const Series& v) {
    const std::int64_t n = v.length();
    const dftu_series* h = v.handle();
    const bool is_bytes = value_domain(v.type()) == ValueDomain::Bytes;
    const std::int64_t nullc = cell_null_count(*h);
    const bool has_nulls = nullc > 0;

    dstd::Vector<std::int64_t> nonnull_idx;
    if (has_nulls) {
        nonnull_idx.reserve(static_cast<std::size_t>(n - nullc));
        for (std::int64_t i = 0; i < n; ++i)
            if (!cell_is_null(h, i)) nonnull_idx.push_back(i);
    }
    const std::int64_t m =
        has_nulls ? static_cast<std::int64_t>(nonnull_idx.size()) : n;
    auto idx_at = [&](std::int64_t j) {
        return has_nulls ? nonnull_idx[static_cast<std::size_t>(j)] : j;
    };

    const bool view = v.encoding() != Encoding::Flat;
    dstd::Vector<std::int64_t> sub_counts =
        is_bytes ? key_counts<dstd::StringView>(
                       m, view,
                       [&](std::int64_t j) { return cell_bytes(h, idx_at(j)); })
                 : key_counts<std::uint64_t>(m, view, [&](std::int64_t j) {
                       // Every NaN is one value and -0.0 is +0.0 (Polars).
                       const double d = cell_f64(h, idx_at(j));
                       if (d != d) return std::uint64_t{0x7ff8000000000000ULL};
                       return std::bit_cast<std::uint64_t>(d == 0.0 ? 0.0 : d);
                   });

    dstd::Vector<std::int64_t> counts(static_cast<std::size_t>(n), 0);
    for (std::int64_t j = 0; j < m; ++j)
        counts[static_cast<std::size_t>(idx_at(j))] =
            sub_counts[static_cast<std::size_t>(j)];
    if (has_nulls)
        for (std::int64_t i = 0; i < n; ++i)
            if (cell_is_null(h, i)) counts[static_cast<std::size_t>(i)] = nullc;
    return counts;
}

// keep_when_unique: true builds is_unique, false builds is_duplicated.
Series occurrence_mask(const Series& v, bool keep_when_unique) {
    if (refuse_nested_value(keep_when_unique ? "is_unique" : "is_duplicated",
                            v.type()))
        return Series{};
    const dstd::Vector<std::int64_t> counts = count_values(v);
    const std::int64_t n = v.length();
    dstd::Vector<char> flags(static_cast<std::size_t>(n), 0);
    parallel_for(n, std::int64_t{1} << 15, [&](std::int64_t b, std::int64_t e) {
        for (std::int64_t i = b; i < e; ++i) {
            const bool unique = counts[static_cast<std::size_t>(i)] == 1;
            flags[static_cast<std::size_t>(i)] =
                (keep_when_unique ? unique : !unique) ? 1 : 0;
        }
    });
    return bool_from_flags(flags);
}

// keep 0: mark every repeat of a value but its first row; 1: but its last row;
// otherwise every row of a repeated value (is_duplicated). A null is one value.
Series duplicated_keep(const Series& v, std::int32_t keep) {
    if (keep != 0 && keep != 1) return occurrence_mask(v, false);
    if (refuse_nested_value("duplicated", v.type())) return Series{};
    ValueIds ids;
    const Encoding encoding = v.encoding();
    const dftu_series& h = *v.handle();
    if (has_constant_layer(h) ||
        ((encoding == Encoding::Selection || encoding == Encoding::Chunked) &&
         !is_string_column(h))) {
        // One copy: value_ids reads no CONSTANT layer and numbers only a FLAT
        // column of numbers in parallel.
        const Series flat = v.materialize();
        if (!flat.valid() || !value_ids(*flat.handle(), ids)) return Series{};
    } else if (!value_ids(*v.handle(), ids)) {
        return Series{};
    }
    const std::int64_t n = v.length();
    dstd::Vector<std::int64_t> last;
    if (keep == 1) {
        last.assign(ids.first.size(), 0);
        for (std::int64_t i = 0; i < n; ++i)
            last[static_cast<std::size_t>(
                ids.ids[static_cast<std::size_t>(i)])] = i;
    }
    const dstd::Vector<std::int64_t>& kept = keep == 0 ? ids.first : last;
    dstd::Vector<char> flags(static_cast<std::size_t>(n), 0);
    for (std::int64_t i = 0; i < n; ++i)
        flags[static_cast<std::size_t>(i)] =
            kept[static_cast<std::size_t>(
                ids.ids[static_cast<std::size_t>(i)])] != i;
    return bool_from_flags(flags);
}

bool is_sorted_impl(const Series& v, bool descending) {
    const std::int64_t n = v.length();
    if (n < 2) return true;
    if (refuse_nested_value("is_sorted", v.type())) return false;
    bool simd = false;
    if (v.encoding() == Encoding::Chunked)
        return is_sorted_impl(v.materialize(), descending);
    if (is_sorted_numeric(*v.handle(), descending, &simd)) return simd;
    // Nulls come last, as argsort puts them; the value under a null is never
    // compared.
    bool null_seen = false;
    for (std::int64_t i = 0; i < n; ++i) {
        if (v.is_null(i)) {
            null_seen = true;
            continue;
        }
        if (null_seen) return false;
        if (i == 0) continue;
        const int cmp = compare_rows(v, i - 1, i);
        if (descending ? cmp < 0 : cmp > 0) return false;
    }
    return true;
}
Series is_in_strings(const dftu_series& v, const dftu_series& values) {
    dstd::HashSet<dstd::StringView> set;
    dstd::StringView s;
    for (std::int64_t j = 0; j < values.length; ++j)
        if (str_at(&values, j, s)) set.insert(s);
    const std::int64_t n = v.length;
    dstd::Vector<char> flags(static_cast<std::size_t>(n), 0);
    dstd::Vector<char> entry_hit;
    const std::int32_t* codes = nullptr;
    const bool dict = v.encoding == Encoding::Dictionary && v.child();
    if (dict) {
        const dftu_series* d = v.child().get();
        entry_hit.assign(static_cast<std::size_t>(d->length), 0);
        for (std::int64_t j = 0; j < d->length; ++j)
            entry_hit[static_cast<std::size_t>(j)] =
                str_at(d, j, s) && set.count(s) > 0;
        codes = reinterpret_cast<const std::int32_t*>(v.data.data());
    }
    constexpr std::int64_t GRAIN = 1 << 15;
    auto probe = [&](std::int64_t b, std::int64_t e) {
        dstd::StringView t;
        for (std::int64_t i = b; i < e; ++i) {
            bool hit;
            if (dict) {
                const auto code = static_cast<std::uint32_t>(codes[i]);
                hit = code < entry_hit.size() && entry_hit[code] &&
                      (!v.validity.block() ||
                       ((v.validity.data()[i >> 3] >> (i & 7)) & 1));
            } else {
                hit = str_at(&v, i, t) && set.count(t) > 0;
            }
            flags[static_cast<std::size_t>(i)] = hit ? 1 : 0;
        }
    };
    if (parallel_backend_installed() && n > GRAIN)
        parallel_for(n, GRAIN, probe);
    else
        probe(0, n);
    return bool_from_flags(flags);
}

// A number as an is_in key: -0.0 is 0.0 and every NaN is one value.
std::uint64_t number_key(double x) {
    return canonical_f64_bits(std::bit_cast<std::uint64_t>(x));
}

enum class IntKind { None, Signed, Unsigned };

IntKind int_kind(TypeId t) {
    switch (t) {
        case TypeId::Bool:
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
            return IntKind::Signed;
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
        case TypeId::Uint64:
            return IntKind::Unsigned;
        default:
            return IntKind::None;
    }
}

bool is_float_type(TypeId t) {
    return t == TypeId::Float32 || t == TypeId::Float64;
}

// 2^63 and 2^64 as doubles, the ends of the int64 and uint64 ranges.
constexpr double TWO_POW_63 = 9223372036854775808.0;
constexpr double TWO_POW_64 = 18446744073709551616.0;

// Integers on both sides compare exactly: a double key would merge values
// above 2^53, such as 2^63 and 2^63 + 1 in a Uint64 column. A float needle
// against an integer column matches only the integer it equals exactly.
Series is_in_integers(const Series& v, const Series& values) {
    dstd::HashSet<std::uint64_t> non_negative;
    dstd::HashSet<std::int64_t> negative;
    const bool v_unsigned = int_kind(v.type()) == IntKind::Unsigned;
    const bool vals_unsigned = int_kind(values.type()) == IntKind::Unsigned;
    const bool vals_float = is_float_type(values.type());
    const dftu_series* vh = v.handle();
    const dftu_series* mh = values.handle();
    for (std::int64_t j = 0; j < values.length(); ++j) {
        if (cell_is_null(mh, j)) continue;
        if (vals_float) {
            const double d = cell_f64(mh, j);
            if (!(d >= -TWO_POW_63 && d < TWO_POW_64) || std::floor(d) != d)
                continue;
            if (d < 0)
                negative.insert(static_cast<std::int64_t>(d));
            else
                non_negative.insert(static_cast<std::uint64_t>(d));
            continue;
        }
        if (vals_unsigned) {
            non_negative.insert(cell_u64(mh, j));
            continue;
        }
        const std::int64_t x = cell_i64(mh, j);
        if (x < 0)
            negative.insert(x);
        else
            non_negative.insert(static_cast<std::uint64_t>(x));
    }
    const std::int64_t n = v.length();
    dstd::Vector<char> flags(static_cast<std::size_t>(n), 0);
    constexpr std::int64_t GRAIN = 1 << 15;
    auto probe = [&](std::int64_t b, std::int64_t e) {
        for (std::int64_t i = b; i < e; ++i) {
            const dftu_series* leaf = vh;
            std::int64_t j = i;
            if (!find_leaf(leaf, j)) continue;
            bool hit;
            if (v_unsigned) {
                hit = non_negative.count(cell_u64(leaf, j)) > 0;
            } else {
                const std::int64_t x = cell_i64(leaf, j);
                hit =
                    x < 0
                        ? negative.count(x) > 0
                        : non_negative.count(static_cast<std::uint64_t>(x)) > 0;
            }
            flags[static_cast<std::size_t>(i)] = hit ? 1 : 0;
        }
    };
    if (parallel_backend_installed() && n > GRAIN)
        parallel_for(n, GRAIN, probe);
    else
        probe(0, n);
    return bool_from_flags(flags);
}

Series is_in_impl(const Series& v, const Series& values) {
    if (refuse_nested_value("is_in", v.type())) return Series{};
    // FLAT Float64 with a small needle set: SIMD broadcast-compare.
    {
        const std::int64_t n = v.length();
        dstd::Vector<std::uint8_t> packed(static_cast<std::size_t>((n + 7) / 8),
                                          0);
        if (is_in_f64_simd(*v.handle(), *values.handle(), packed.data()))
            return Series::flat(TypeId::Bool, packed.data(), n);
    }
    // is_in_strings reads through str_at, which reads no CONSTANT layer; the
    // byte path below does.
    if (is_string_column(*v.handle()) && is_string_column(*values.handle()) &&
        !has_constant_layer(*v.handle()) &&
        !has_constant_layer(*values.handle()))
        return is_in_strings(*v.handle(), *values.handle());
    const IntKind v_int = int_kind(v.type());
    const IntKind vals_int = int_kind(values.type());
    if (v_int != IntKind::None &&
        (vals_int != IntKind::None || is_float_type(values.type())))
        return is_in_integers(v, values);
    const bool is_bytes = value_domain(v.type()) == ValueDomain::Bytes;
    const bool v_float = is_float_type(v.type());
    const dftu_series* vh = v.handle();
    const dftu_series* mh = values.handle();
    dstd::HashSet<std::uint64_t> num_set;
    dstd::HashSet<dstd::StringView> str_set;  // views into `values`
    const std::int64_t m = values.length();
    const ValueDomain vals_domain = value_domain(values.type());
    // A fixed-width byte row only means the same value in a needle column of
    // the same type and width.
    const bool vals_bytes = vals_domain == ValueDomain::Bytes &&
                            (narrow_varwidth_type(v.type()) == TypeId::String ||
                             narrow_varwidth_type(v.type()) == TypeId::Binary ||
                             values.data_type() == v.data_type());
    for (std::int64_t j = 0; j < m; ++j) {
        if (cell_is_null(mh, j)) continue;
        if (is_bytes) {
            if (vals_bytes) str_set.insert(cell_bytes(mh, j));
        } else if (v_float && vals_int == IntKind::Unsigned) {
            // An integer needle matches a float row only when the double
            // holds it exactly.
            const std::uint64_t x = cell_u64(mh, j);
            const double d = static_cast<double>(x);
            if (d < TWO_POW_64 && static_cast<std::uint64_t>(d) == x)
                num_set.insert(number_key(d));
        } else if (v_float && vals_int == IntKind::Signed) {
            const std::int64_t x = cell_i64(mh, j);
            const double d = static_cast<double>(x);
            if (d < TWO_POW_63 && static_cast<std::int64_t>(d) == x)
                num_set.insert(number_key(d));
        } else if (vals_domain == ValueDomain::Numeric) {
            num_set.insert(number_key(cell_f64(mh, j)));
        }
    }
    const std::int64_t n = v.length();
    dstd::Vector<char> flags(static_cast<std::size_t>(n), 0);
    constexpr std::int64_t GRAIN = 1 << 15;
    auto probe = [&](std::int64_t b, std::int64_t e) {
        for (std::int64_t i = b; i < e; ++i) {
            const dftu_series* leaf = vh;
            std::int64_t j = i;
            if (!find_leaf(leaf, j)) continue;
            const bool hit =
                is_bytes ? str_set.count(cell_bytes(leaf, j)) > 0
                         : num_set.count(number_key(cell_f64(leaf, j))) > 0;
            flags[static_cast<std::size_t>(i)] = hit ? 1 : 0;
        }
    };
    if (parallel_backend_installed() && n > GRAIN) {
        parallel_for(n, GRAIN, probe);
    } else {
        probe(0, n);
    }
    return bool_from_flags(flags);
}

std::int64_t clamp_n(std::int64_t n, std::int64_t len) {
    return n < 0 ? 0 : (n > len ? len : n);
}

Series head_impl(const Series& v, std::int64_t n) {
    n = clamp_n(n, v.length());
    dstd::Vector<std::int64_t> idx(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i) idx[static_cast<std::size_t>(i)] = i;
    return v.take(idx);
}

Series tail_impl(const Series& v, std::int64_t n) {
    const std::int64_t len = v.length();
    n = clamp_n(n, len);
    dstd::Vector<std::int64_t> idx(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i)
        idx[static_cast<std::size_t>(i)] = len - n + i;
    return v.take(idx);
}

Series reverse_impl(const Series& v) {
    const std::int64_t n = v.length();
    dstd::Vector<std::int64_t> idx(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i)
        idx[static_cast<std::size_t>(i)] = n - 1 - i;
    return v.take(idx);
}

// A negative source index gathers a null (take's OUTER-fill), so vacated rows
// become null for any column type.
Series shift_impl(const Series& v, std::int64_t n) {
    const std::int64_t len = v.length();
    dstd::Vector<std::int64_t> idx(static_cast<std::size_t>(len));
    for (std::int64_t i = 0; i < len; ++i) {
        const std::int64_t s = i - n;
        idx[static_cast<std::size_t>(i)] = (s >= 0 && s < len) ? s : -1;
    }
    return v.take(idx);
}

Series sample_impl(const Series& v, std::int64_t n, std::uint64_t seed) {
    const std::int64_t len = v.length();
    n = clamp_n(n, len);
    dstd::Vector<std::int64_t> order(static_cast<std::size_t>(len));
    for (std::int64_t i = 0; i < len; ++i)
        order[static_cast<std::size_t>(i)] = i;
    auto key = [seed](std::int64_t i) {
        return dftracer::utils::dstd::hash::splitmix64(
            static_cast<std::uint64_t>(i) + seed);
    };
    // The n smallest-hash rows are the sample; nth_element partitions in
    // O(len).
    if (n < len)
        std::nth_element(
            order.begin(), order.begin() + n, order.end(),
            [&](std::int64_t a, std::int64_t b) { return key(a) < key(b); });
    order.resize(static_cast<std::size_t>(n));
    std::sort(order.begin(), order.end());  // stable ascending row order
    return v.take(order);
}

}  // namespace

dstd::Vector<std::int64_t> to_index_vector(const Series& idx) {
    if (!idx.is_flat()) return to_index_vector(idx.materialize());
    const std::int64_t* p = idx.data<std::int64_t>();
    return dstd::Vector<std::int64_t>(p, p + idx.length());
}
}  // namespace dftracer::utils::dataframe

using dftracer::utils::dataframe::borrow;
using dftracer::utils::dataframe::Series;

dftu_series* dftu_series_is_unique(const dftu_series* v) {
    if (!v) return nullptr;
    Series c = borrow(v);
    Series r = dftracer::utils::dataframe::occurrence_mask(c, true);
    c.release();
    return r.release();
}
dftu_series* dftu_series_is_duplicated(const dftu_series* v) {
    if (!v) return nullptr;
    Series c = borrow(v);
    Series r = dftracer::utils::dataframe::occurrence_mask(c, false);
    c.release();
    return r.release();
}
dftu_series* dftu_series_duplicated(const dftu_series* v, std::int32_t keep) {
    if (!v) return nullptr;
    Series c = borrow(v);
    Series r = dftracer::utils::dataframe::duplicated_keep(c, keep);
    c.release();
    return r.release();
}
std::int32_t dftu_series_is_sorted(const dftu_series* v,
                                   std::int32_t descending) {
    if (!v) return 0;
    DFTU_FLAT_INPUT(v, dftu_series_is_sorted, descending);
    Series c = borrow(v);
    bool r = dftracer::utils::dataframe::is_sorted_impl(c, descending != 0);
    c.release();
    return r ? 1 : 0;
}
dftu_series* dftu_series_drop_nulls(const dftu_series* v) {
    if (!v) return nullptr;
    Series c = borrow(v);
    const std::int64_t n = c.length();
    dftracer::utils::dstd::Vector<char> flags(static_cast<std::size_t>(n), 0);
    const bool has_nulls = c.null_count() > 0 || v->rowwise_nulls();
    for (std::int64_t i = 0; i < n; ++i)
        flags[static_cast<std::size_t>(i)] =
            (!has_nulls || !c.is_null(i)) ? 1 : 0;
    Series mask = dftracer::utils::dataframe::bool_from_flags(flags);
    Series r = c.filter(mask);
    c.release();
    return r.release();
}
dftu_series* dftu_series_is_in(const dftu_series* v,
                               const dftu_series* values) {
    if (!v || !values) return nullptr;
    DFTU_PER_CHUNK(v, dftu_series_is_in, values);
    Series c = borrow(v);
    Series cv = borrow(values);
    Series r = dftracer::utils::dataframe::is_in_impl(c, cv);
    c.release();
    cv.release();
    return r.release();
}
dftu_series* dftu_series_sort(const dftu_series* v, std::int32_t descending) {
    return dftu_series_sort_nulls(v, descending, 1);
}
dftu_series* dftu_series_sort_nulls(const dftu_series* v,
                                    std::int32_t descending,
                                    std::int32_t nulls_last) {
    if (!v) return nullptr;
    DFTU_FLAT_INPUT(v, dftu_series_sort_nulls, descending, nulls_last);
    Series c = borrow(v);
    Series order = dftracer::utils::dataframe::argsort(c, descending != 0,
                                                       nulls_last != 0);
    if (!order.valid()) {
        c.release();
        return nullptr;
    }
    Series r = c.take(dftracer::utils::dataframe::to_index_vector(order));
    c.release();
    return r.release();
}
dftu_series* dftu_series_head(const dftu_series* v, std::int64_t n) {
    if (!v) return nullptr;
    Series c = borrow(v);
    Series r = dftracer::utils::dataframe::head_impl(c, n);
    c.release();
    return r.release();
}
dftu_series* dftu_series_tail(const dftu_series* v, std::int64_t n) {
    if (!v) return nullptr;
    Series c = borrow(v);
    Series r = dftracer::utils::dataframe::tail_impl(c, n);
    c.release();
    return r.release();
}
dftu_series* dftu_series_reverse(const dftu_series* v) {
    if (!v) return nullptr;
    Series c = borrow(v);
    Series r = dftracer::utils::dataframe::reverse_impl(c);
    c.release();
    return r.release();
}
dftu_series* dftu_series_shift(const dftu_series* v, std::int64_t n) {
    if (!v) return nullptr;
    Series c = borrow(v);
    Series r = dftracer::utils::dataframe::shift_impl(c, n);
    c.release();
    return r.release();
}
dftu_series* dftu_series_top_k(const dftu_series* v, std::int64_t k) {
    if (!v) return nullptr;
    DFTU_FLAT_INPUT(v, dftu_series_top_k, k);
    Series c = borrow(v);
    Series idx = dftracer::utils::dataframe::topk_indices(c, k, true);
    if (!idx.valid()) {
        c.release();
        return nullptr;
    }
    Series r = c.take(dftracer::utils::dataframe::to_index_vector(idx));
    c.release();
    return r.release();
}
dftu_series* dftu_series_bottom_k(const dftu_series* v, std::int64_t k) {
    if (!v) return nullptr;
    DFTU_FLAT_INPUT(v, dftu_series_bottom_k, k);
    Series c = borrow(v);
    Series idx = dftracer::utils::dataframe::topk_indices(c, k, false);
    if (!idx.valid()) {
        c.release();
        return nullptr;
    }
    Series r = c.take(dftracer::utils::dataframe::to_index_vector(idx));
    c.release();
    return r.release();
}
dftu_series* dftu_series_sample(const dftu_series* v, std::int64_t n,
                                std::uint64_t seed) {
    if (!v) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_sample(flat_v, n, seed));
    Series c = borrow(v);
    Series r = dftracer::utils::dataframe::sample_impl(c, n, seed);
    c.release();
    return r.release();
}
