// Selection / reshape and hash-predicate Series ops that compose the existing
// SIMD primitives (argsort, topk_indices, take, filter) or a single hash pass:
// is_unique/is_duplicated/is_sorted/is_in, drop_nulls, sort, head/tail,
// reverse, shift, top_k/bottom_k, sample. None is a lane kernel: the numeric
// heavy lifting is delegated to argsort/topk_indices (SIMD) and take/filter.

#include <dftracer/utils/core/common/hash/splitmix64.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/column_read.h>
#include <dftracer/utils/dataframe/internal/compare_simd.h>  // pack_flags
#include <dftracer/utils/dataframe/internal/radix_dedup.h>   // parallel dedup
#include <dftracer/utils/dataframe/kernels/sort.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/dataframe/series.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace dftracer::utils::dataframe {

namespace {

// Borrow a C ABI handle as a Series without owning it (release before return).
Series borrow(const dftu_series* v) {
    return Series{const_cast<dftu_series*>(v)};
}

// A bit-packed Bool column from per-row flags (no validity).
Series bool_from_flags(const std::vector<char>& flags) {
    const std::int64_t n = static_cast<std::int64_t>(flags.size());
    std::vector<std::uint8_t> packed(static_cast<std::size_t>((n + 7) / 8), 0);
    pack_flags(flags.data(), n, packed.data());
    return Series::flat(TypeId::Bool, packed.data(), n);
}

std::vector<std::int64_t> to_index_vector(const Series& idx) {
    const std::int64_t* p = idx.data<std::int64_t>();
    return std::vector<std::int64_t>(p, p + idx.length());
}

// Per-row occurrence count of each row's own value, keyed in the column's
// domain (nulls all share one group, counted separately). Radix-partitioned
// over the non-null rows, mirroring the eager DataFrame dedup helpers.
std::vector<std::int64_t> count_values(const Series& v) {
    const std::int64_t n = v.length();
    const bool is_bytes = value_domain(v.type()) == ValueDomain::Bytes;
    const bool has_nulls = v.null_count() > 0;
    const std::int64_t nullc = v.null_count();

    std::vector<std::int64_t> nonnull_idx;
    if (has_nulls) {
        nonnull_idx.reserve(static_cast<std::size_t>(n - nullc));
        for (std::int64_t i = 0; i < n; ++i)
            if (!v.is_null(i)) nonnull_idx.push_back(i);
    }
    const std::int64_t m =
        has_nulls ? static_cast<std::int64_t>(nonnull_idx.size()) : n;
    auto idx_at = [&](std::int64_t j) {
        return has_nulls ? nonnull_idx[static_cast<std::size_t>(j)] : j;
    };

    std::vector<std::int64_t> sub_counts =
        is_bytes ? radix_counts_by<std::string>(
                       m,
                       [&](std::int64_t j) {
                           return std::string(read_bytes(v, idx_at(j)));
                       })
                 : radix_counts_by<double>(m, [&](std::int64_t j) {
                       return read_f64(v, idx_at(j));
                   });

    std::vector<std::int64_t> counts(static_cast<std::size_t>(n), 0);
    for (std::int64_t j = 0; j < m; ++j)
        counts[static_cast<std::size_t>(idx_at(j))] =
            sub_counts[static_cast<std::size_t>(j)];
    if (has_nulls)
        for (std::int64_t i = 0; i < n; ++i)
            if (v.is_null(i)) counts[static_cast<std::size_t>(i)] = nullc;
    return counts;
}

// keep_when_unique: true builds is_unique, false builds is_duplicated.
Series occurrence_mask(const Series& v, bool keep_when_unique) {
    if (refuse_nested_value(keep_when_unique ? "is_unique" : "is_duplicated",
                            v.type()))
        return Series{};
    const std::vector<std::int64_t> counts = count_values(v);
    const std::int64_t n = v.length();
    std::vector<char> flags(static_cast<std::size_t>(n), 0);
    parallel_for(n, std::int64_t{1} << 15, [&](std::int64_t b, std::int64_t e) {
        for (std::int64_t i = b; i < e; ++i) {
            const bool unique = counts[static_cast<std::size_t>(i)] == 1;
            flags[static_cast<std::size_t>(i)] =
                (keep_when_unique ? unique : !unique) ? 1 : 0;
        }
    });
    return bool_from_flags(flags);
}

bool is_sorted_impl(const Series& v, bool descending) {
    const std::int64_t n = v.length();
    if (n < 2) return true;
    if (refuse_nested_value("is_sorted", v.type())) return false;
    bool simd = false;
    if (is_sorted_numeric(*v.handle(), descending, &simd)) return simd;
    for (std::int64_t i = 1; i < n; ++i) {
        const int cmp = compare_rows(v, i - 1, i);
        if (descending ? cmp < 0 : cmp > 0) return false;
    }
    return true;
}
Series is_in_impl(const Series& v_in, const Series& values_in) {
    if (refuse_nested_value("is_in", v_in.type())) return Series{};
    // FLAT Float64 with a small needle set: SIMD broadcast-compare.
    {
        const std::int64_t n = v_in.length();
        std::vector<std::uint8_t> packed(static_cast<std::size_t>((n + 7) / 8),
                                         0);
        if (is_in_f64_simd(*v_in.handle(), *values_in.handle(), packed.data()))
            return Series::flat(TypeId::Bool, packed.data(), n);
    }
    // read_bytes only sees a FLAT column's buffers, so a sliced or dictionary
    // needle would otherwise read empty and match nothing.
    auto flatten = [](const Series& c) {
        return c.encoding() == Encoding::Flat
                   ? c.share()
                   : Series{dftu_series_materialize(c.handle())};
    };
    const Series v = flatten(v_in);
    const Series values = flatten(values_in);
    const bool is_bytes = value_domain(v.type()) == ValueDomain::Bytes;
    std::unordered_set<double> num_set;
    std::unordered_set<std::string> str_set;
    const std::int64_t m = values.length();
    const bool vals_null = values.null_count() > 0;
    const ValueDomain vals_domain = value_domain(values.type());
    // A fixed-width byte row only means the same value in a needle column of
    // the same type and width.
    const bool vals_bytes = vals_domain == ValueDomain::Bytes &&
                            (narrow_varwidth_type(v.type()) == TypeId::String ||
                             narrow_varwidth_type(v.type()) == TypeId::Binary ||
                             values.data_type() == v.data_type());
    for (std::int64_t j = 0; j < m; ++j) {
        if (vals_null && values.is_null(j)) continue;
        if (is_bytes) {
            if (vals_bytes) str_set.insert(std::string(read_bytes(values, j)));
        } else if (vals_domain == ValueDomain::Numeric) {
            num_set.insert(read_f64(values, j));
        }
    }
    const std::int64_t n = v.length();
    const bool has_nulls = v.null_count() > 0;
    std::vector<char> flags(static_cast<std::size_t>(n), 0);
    constexpr std::int64_t GRAIN = 1 << 15;
    auto probe = [&](std::int64_t b, std::int64_t e) {
        for (std::int64_t i = b; i < e; ++i) {
            if (has_nulls && v.is_null(i)) continue;
            const bool hit =
                is_bytes ? str_set.count(std::string(read_bytes(v, i))) > 0
                         : num_set.count(read_f64(v, i)) > 0;
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
    std::vector<std::int64_t> idx(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i) idx[static_cast<std::size_t>(i)] = i;
    return v.take(idx);
}

Series tail_impl(const Series& v, std::int64_t n) {
    const std::int64_t len = v.length();
    n = clamp_n(n, len);
    std::vector<std::int64_t> idx(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i)
        idx[static_cast<std::size_t>(i)] = len - n + i;
    return v.take(idx);
}

Series reverse_impl(const Series& v) {
    const std::int64_t n = v.length();
    std::vector<std::int64_t> idx(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i)
        idx[static_cast<std::size_t>(i)] = n - 1 - i;
    return v.take(idx);
}

// A negative source index gathers a null (take's OUTER-fill), so vacated rows
// become null for any column type.
Series shift_impl(const Series& v, std::int64_t n) {
    const std::int64_t len = v.length();
    std::vector<std::int64_t> idx(static_cast<std::size_t>(len));
    for (std::int64_t i = 0; i < len; ++i) {
        const std::int64_t s = i - n;
        idx[static_cast<std::size_t>(i)] = (s >= 0 && s < len) ? s : -1;
    }
    return v.take(idx);
}

Series sample_impl(const Series& v, std::int64_t n, std::uint64_t seed) {
    const std::int64_t len = v.length();
    n = clamp_n(n, len);
    std::vector<std::int64_t> order(static_cast<std::size_t>(len));
    for (std::int64_t i = 0; i < len; ++i)
        order[static_cast<std::size_t>(i)] = i;
    auto key = [seed](std::int64_t i) {
        return dftracer::utils::hash::splitmix64(static_cast<std::uint64_t>(i) +
                                                 seed);
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
}  // namespace dftracer::utils::dataframe

using dftracer::utils::dataframe::borrow;
using dftracer::utils::dataframe::Series;

extern "C" {

dftu_series* dftu_series_is_unique(const dftu_series* v) {
    if (!v) return nullptr;
    DFTU_FLAT_INPUT(v, dftu_series_is_unique);
    Series c = borrow(v);
    Series r = dftracer::utils::dataframe::occurrence_mask(c, true);
    c.release();
    return r.release();
}
dftu_series* dftu_series_is_duplicated(const dftu_series* v) {
    if (!v) return nullptr;
    DFTU_FLAT_INPUT(v, dftu_series_is_duplicated);
    Series c = borrow(v);
    Series r = dftracer::utils::dataframe::occurrence_mask(c, false);
    c.release();
    return r.release();
}
int32_t dftu_series_is_sorted(const dftu_series* v, int32_t descending) {
    if (!v) return 0;
    DFTU_FLAT_INPUT(v, dftu_series_is_sorted, descending);
    Series c = borrow(v);
    bool r = dftracer::utils::dataframe::is_sorted_impl(c, descending != 0);
    c.release();
    return r ? 1 : 0;
}
dftu_series* dftu_series_drop_nulls(const dftu_series* v) {
    if (!v) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_drop_nulls(flat_v));
    Series c = borrow(v);
    const std::int64_t n = c.length();
    std::vector<char> flags(static_cast<std::size_t>(n), 0);
    const bool has_nulls = c.null_count() > 0;
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
    Series c = borrow(v);
    Series cv = borrow(values);
    Series r = dftracer::utils::dataframe::is_in_impl(c, cv);
    c.release();
    cv.release();
    return r.release();
}
dftu_series* dftu_series_sort(const dftu_series* v, int32_t descending) {
    if (!v) return nullptr;
    DFTU_FLAT_INPUT(v, dftu_series_sort, descending);
    Series c = borrow(v);
    Series order = dftracer::utils::dataframe::argsort(c, descending != 0);
    if (!order.valid()) {
        c.release();
        return nullptr;
    }
    Series r = c.take(dftracer::utils::dataframe::to_index_vector(order));
    c.release();
    return r.release();
}
dftu_series* dftu_series_head(const dftu_series* v, int64_t n) {
    if (!v) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_head(flat_v, n));
    Series c = borrow(v);
    Series r = dftracer::utils::dataframe::head_impl(c, n);
    c.release();
    return r.release();
}
dftu_series* dftu_series_tail(const dftu_series* v, int64_t n) {
    if (!v) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_tail(flat_v, n));
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
dftu_series* dftu_series_shift(const dftu_series* v, int64_t n) {
    if (!v) return nullptr;
    Series c = borrow(v);
    Series r = dftracer::utils::dataframe::shift_impl(c, n);
    c.release();
    return r.release();
}
dftu_series* dftu_series_top_k(const dftu_series* v, int64_t k) {
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
dftu_series* dftu_series_bottom_k(const dftu_series* v, int64_t k) {
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
dftu_series* dftu_series_sample(const dftu_series* v, int64_t n,
                                uint64_t seed) {
    if (!v) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_sample(flat_v, n, seed));
    Series c = borrow(v);
    Series r = dftracer::utils::dataframe::sample_impl(c, n, seed);
    c.release();
    return r.release();
}

}  // extern "C"
