#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/numeric_dispatch.h>
#include <dftracer/utils/dataframe/internal/radix_dedup.h>  // parallel dedup
#include <dftracer/utils/dataframe/internal/reduce_simd.h>
#include <dftracer/utils/dataframe/internal/type_promotion.h>
#include <dftracer/utils/dataframe/kernels/reduce.h>
#include <dftracer/utils/dataframe/parallel.h>

#include <algorithm>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace dftracer::utils::dataframe {

dftu_scalar sum(const Series& v) {
    return dftu_series_reduce(v.handle(), DFTU_REDUCE_SUM);
}
dftu_scalar min(const Series& v) {
    return dftu_series_reduce(v.handle(), DFTU_REDUCE_MIN);
}
dftu_scalar max(const Series& v) {
    return dftu_series_reduce(v.handle(), DFTU_REDUCE_MAX);
}
std::int64_t count(const Series& v) { return dftu_series_count(v.handle()); }

double mean(const Series& v) {
    std::int64_t n = count(v);
    if (n == 0) return 0.0;
    return scalar_value<double>(sum(v)) / static_cast<double>(n);
}

dftu_scalar product(const Series& v) { return dftu_series_product(v.handle()); }
bool all(const Series& v) { return dftu_series_all(v.handle()) != 0; }
bool any(const Series& v) { return dftu_series_any(v.handle()) != 0; }
std::int64_t arg_min(const Series& v) {
    return dftu_series_arg_min(v.handle());
}
std::int64_t arg_max(const Series& v) {
    return dftu_series_arg_max(v.handle());
}
dftu_scalar mode(const Series& v) { return dftu_series_mode(v.handle()); }

}  // namespace dftracer::utils::dataframe

namespace {

bool is_valid(const dftu_series& v, std::int64_t i) {
    if (!v.validity) return true;
    return (v.validity->data()[i >> 3] & (1u << (i & 7))) != 0;
}

template <class T>
void store_domain(dftu_scalar& out, T value) {
    if constexpr (std::is_floating_point_v<T>) {
        out.kind = DFTU_SCALAR_TAG_F64;
        out.value.d = static_cast<double>(value);
    } else if constexpr (std::is_unsigned_v<T>) {
        out.kind = DFTU_SCALAR_TAG_U64;
        out.value.u = static_cast<std::uint64_t>(value);
    } else {
        out.kind = DFTU_SCALAR_TAG_I64;
        out.value.i = static_cast<std::int64_t>(value);
    }
}

// Integer sums accumulate in the widest same-signedness type (no overflow of a
// narrow input); float/f32 sums accumulate in double to hold precision.
template <class T>
using sum_acc_t = std::conditional_t<
    std::is_floating_point_v<T>, double,
    std::conditional_t<std::is_unsigned_v<T>, std::uint64_t, std::int64_t>>;

template <class T>
void reduce_one(const dftu_series& v, std::int32_t op, dftu_scalar& out) {
    const T* p = reinterpret_cast<const T*>(v.data->data());
    const std::int64_t n = v.length;
    // A validity bitmap forces a per-element branch that blocks the compiler
    // from vectorizing the reduction; the far more common no-null column runs a
    // branch-free loop that auto-vectorizes (reduce_simd handles f64 sum before
    // we get here, but integer sum and all min/max land on this path).
    const bool has_null = v.validity != nullptr;

    if (op == DFTU_REDUCE_SUM) {
        sum_acc_t<T> acc = 0;
        if (has_null) {
            for (std::int64_t i = 0; i < n; ++i)
                if (is_valid(v, i)) acc += static_cast<sum_acc_t<T>>(p[i]);
        } else {
            for (std::int64_t i = 0; i < n; ++i)
                acc += static_cast<sum_acc_t<T>>(p[i]);
        }
        if constexpr (std::is_floating_point_v<T>) {
            out.kind = DFTU_SCALAR_TAG_F64;
            out.value.d = acc;
        } else if constexpr (std::is_unsigned_v<T>) {
            out.kind = DFTU_SCALAR_TAG_U64;
            out.value.u = acc;
        } else {
            out.kind = DFTU_SCALAR_TAG_I64;
            out.value.i = acc;
        }
        return;
    }

    const bool is_min = (op == DFTU_REDUCE_MIN);
    T best{};
    if (has_null) {
        bool found = false;
        for (std::int64_t i = 0; i < n; ++i) {
            if (!is_valid(v, i)) continue;
            if (!found) {
                best = p[i];
                found = true;
            } else if (is_min) {
                if (p[i] < best) best = p[i];
            } else if (p[i] > best) {
                best = p[i];
            }
        }
    } else if (n > 0) {
        // op hoisted out of the loop so each stays a branch-free min/max
        // reduction the compiler can vectorize.
        best = p[0];
        if (is_min) {
            for (std::int64_t i = 1; i < n; ++i)
                if (p[i] < best) best = p[i];
        } else {
            for (std::int64_t i = 1; i < n; ++i)
                if (p[i] > best) best = p[i];
        }
    }
    store_domain<T>(out, best);
}

// Product accumulates in the column's wide domain, skipping nulls (identity 1).
template <class T>
void product_one(const dftu_series& v, dftu_scalar& out) {
    const T* p = reinterpret_cast<const T*>(v.data->data());
    const std::int64_t n = v.length;
    const bool has_null = v.validity != nullptr;
    sum_acc_t<T> acc = 1;
    for (std::int64_t i = 0; i < n; ++i)
        if (!has_null || is_valid(v, i)) acc *= static_cast<sum_acc_t<T>>(p[i]);
    if constexpr (std::is_floating_point_v<T>) {
        out.kind = DFTU_SCALAR_TAG_F64;
        out.value.d = static_cast<double>(acc);
    } else if constexpr (std::is_unsigned_v<T>) {
        out.kind = DFTU_SCALAR_TAG_U64;
        out.value.u = static_cast<std::uint64_t>(acc);
    } else {
        out.kind = DFTU_SCALAR_TAG_I64;
        out.value.i = static_cast<std::int64_t>(acc);
    }
}

template <class T>
void argextreme_one(const dftu_series& v, bool is_min, std::int64_t& out_idx) {
    const T* p = reinterpret_cast<const T*>(v.data->data());
    const std::int64_t n = v.length;
    const bool has_null = v.validity != nullptr;
    out_idx = -1;
    T best{};
    for (std::int64_t i = 0; i < n; ++i) {
        if (has_null && !is_valid(v, i)) continue;
        if (out_idx < 0) {
            best = p[i];
            out_idx = i;
        } else if (is_min ? (p[i] < best) : (p[i] > best)) {
            best = p[i];
            out_idx = i;
        }
    }
}

// Serial fallback (and reference algorithm): strictly-greater keeps the value
// that first reached the top count, so ties resolve to whichever value's
// count first attains the eventual maximum.
template <class T>
void mode_serial(const T* p, const std::vector<std::int64_t>& idx,
                 dftu_scalar& out) {
    ankerl::unordered_dense::map<T, std::int64_t> counts;
    T best{};
    std::int64_t best_count = 0;
    for (std::int64_t j : idx) {
        const std::int64_t c = ++counts[p[j]];
        if (c > best_count) {
            best_count = c;
            best = p[j];
        }
    }
    store_domain<T>(out, best);
}

template <class T>
void mode_one(const dftu_series& v, dftu_scalar& out) {
    using dftracer::utils::dataframe::radix_counts_by;
    const T* p = reinterpret_cast<const T*>(v.data->data());
    const std::int64_t n = v.length;
    const bool has_null = v.validity != nullptr;

    std::vector<std::int64_t> idx;
    idx.reserve(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i)
        if (!has_null || is_valid(v, i)) idx.push_back(i);
    const std::int64_t m = static_cast<std::int64_t>(idx.size());

    if (m == 0) {
        store_domain<T>(out, T{});
        return;
    }
    if (!dftracer::utils::dataframe::parallel_backend_installed() ||
        m < dftracer::utils::dataframe::RADIX_DEDUP_MIN_ROWS) {
        mode_serial<T>(p, idx, out);
        return;
    }

    // Frequency of each valid position's value, radix-partitioned and merged
    // (the same shape as group_agg Count). A unique highest-frequency value
    // wins outright; a tie in the final count falls back to a bounded serial
    // rescan restricted to the tied candidates, to reproduce the sequential
    // "first-reaching-top" tie-break exactly (the last event on the winning
    // path can only ever be caused by a value at the eventual maximum count,
    // so skipping non-candidate rows cannot change the outcome).
    std::vector<std::int64_t> counts = radix_counts_by<T>(
        m, [&](std::int64_t j) { return p[idx[static_cast<std::size_t>(j)]]; });
    std::int64_t best_count = 0;
    for (std::int64_t c : counts) best_count = std::max(best_count, c);

    ankerl::unordered_dense::set<T> candidates;
    for (std::int64_t j = 0; j < m; ++j)
        if (counts[static_cast<std::size_t>(j)] == best_count)
            candidates.insert(p[idx[static_cast<std::size_t>(j)]]);

    if (candidates.size() == 1) {
        store_domain<T>(out, *candidates.begin());
        return;
    }

    std::vector<std::int64_t> tied;
    tied.reserve(idx.size());
    for (std::int64_t j : idx)
        if (candidates.count(p[j]) != 0) tied.push_back(j);
    mode_serial<T>(p, tied, out);
}

}  // namespace

dftu_scalar dftu_series_reduce(const dftu_series* v, dftu_reduce_op op) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_reduce(flat_v, op));

    using dftracer::utils::dataframe::is_arithmetic_type;
    using dftracer::utils::dataframe::is_temporal_type;
    using dftracer::utils::dataframe::physical_type;
    using dftracer::utils::dataframe::promote_for_arithmetic;
    using dftracer::utils::dataframe::type_name;
    using dftracer::utils::dataframe::TypeId;
    dftu_scalar out{};
    out.kind = DFTU_SCALAR_TAG_I64;
    if (v->encoding != dftracer::utils::dataframe::Encoding::Flat) return out;

    // MIN/MAX on any temporal column dispatch on its physical Int32/Int64
    // layout. SUM is additionally defined for Duration (a sum of durations
    // is a duration, in the same unit); every other temporal type has no
    // meaningful SUM (summing instants or times-of-day is not a rule this
    // engine defines) and every other op stays refused.
    if (is_temporal_type(v->type)) {
        const bool sum_ok =
            op == DFTU_REDUCE_SUM && v->type == TypeId::Duration;
        if (op != DFTU_REDUCE_MIN && op != DFTU_REDUCE_MAX && !sum_ok) {
            DFTRACER_UTILS_LOG_ERROR(
                "reduce: op %d is not defined for temporal type '%s'",
                static_cast<int>(op), type_name(v->type));
            return out;
        }
        DF_NUMERIC_DISPATCH(physical_type(v->type), reduce_one, *v, op, out)
        return out;
    }

    if (!is_arithmetic_type(v->type)) {
        DFTRACER_UTILS_LOG_ERROR("reduce: op %d has no meaning for type '%s'",
                                 static_cast<int>(op), type_name(v->type));
        return out;
    }

    // Float16 has no reduction kernel; Decimal128/256 have no exact one. Both
    // promote here, once, before any dispatch below sees them.
    dftu_series* promoted = nullptr;
    v = promote_for_arithmetic(v, promoted);

    if (dftracer::utils::dataframe::reduce(*v, op, out)) {
        if (promoted) dftu_series_free(promoted);
        return out;
    }
    DF_NUMERIC_DISPATCH(v->type, reduce_one, *v, op, out)
    if (promoted) dftu_series_free(promoted);
    return out;
}

int64_t dftu_series_count(const dftu_series* v) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_count(flat_v));

    return v->length - v->null_count;
}

dftu_scalar dftu_series_product(const dftu_series* v) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_product(flat_v));

    using dftracer::utils::dataframe::is_arithmetic_type;
    using dftracer::utils::dataframe::is_temporal_type;
    using dftracer::utils::dataframe::promote_for_arithmetic;
    using dftracer::utils::dataframe::type_name;
    dftu_scalar out{};
    out.kind = DFTU_SCALAR_TAG_I64;
    if (v->encoding != dftracer::utils::dataframe::Encoding::Flat) return out;
    if (is_temporal_type(v->type)) {
        DFTRACER_UTILS_LOG_ERROR("product: not defined for temporal type '%s'",
                                 type_name(v->type));
        return out;
    }
    if (!is_arithmetic_type(v->type)) {
        DFTRACER_UTILS_LOG_ERROR("product: no meaning for type '%s'",
                                 type_name(v->type));
        return out;
    }
    dftu_series* promoted = nullptr;
    v = promote_for_arithmetic(v, promoted);
    if (dftracer::utils::dataframe::product_simd(*v, out)) {
        if (promoted) dftu_series_free(promoted);
        return out;
    }
    DF_NUMERIC_DISPATCH(v->type, product_one, *v, out)
    if (promoted) dftu_series_free(promoted);
    return out;
}

int32_t dftu_series_all(const dftu_series* v) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_all(flat_v));

    using dftracer::utils::dataframe::TypeId;
    if (v->encoding != dftracer::utils::dataframe::Encoding::Flat ||
        v->type != TypeId::Bool)
        return 1;
    return dftracer::utils::dataframe::all_bits(*v) ? 1 : 0;
}

int32_t dftu_series_any(const dftu_series* v) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_any(flat_v));

    using dftracer::utils::dataframe::TypeId;
    if (v->encoding != dftracer::utils::dataframe::Encoding::Flat ||
        v->type != TypeId::Bool)
        return 0;
    return dftracer::utils::dataframe::any_bits(*v) ? 1 : 0;
}

int64_t dftu_series_arg_min(const dftu_series* v) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_arg_min(flat_v));

    using dftracer::utils::dataframe::is_arithmetic_type;
    using dftracer::utils::dataframe::is_temporal_type;
    using dftracer::utils::dataframe::physical_type;
    using dftracer::utils::dataframe::promote_for_arithmetic;
    using dftracer::utils::dataframe::type_name;
    if (v->encoding != dftracer::utils::dataframe::Encoding::Flat) return -1;
    if (is_temporal_type(v->type)) {
        std::int64_t idx = -1;
        DF_NUMERIC_DISPATCH(physical_type(v->type), argextreme_one, *v, true,
                            idx)
        return idx;
    }
    if (!is_arithmetic_type(v->type)) {
        DFTRACER_UTILS_LOG_ERROR("arg_min: no meaning for type '%s'",
                                 type_name(v->type));
        return -1;
    }
    dftu_series* promoted = nullptr;
    v = promote_for_arithmetic(v, promoted);
    std::int64_t idx = -1;
    if (!dftracer::utils::dataframe::arg_extreme_simd(*v, true, idx))
        DF_NUMERIC_DISPATCH(v->type, argextreme_one, *v, true, idx)
    if (promoted) dftu_series_free(promoted);
    return idx;
}

int64_t dftu_series_arg_max(const dftu_series* v) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_arg_max(flat_v));

    using dftracer::utils::dataframe::is_arithmetic_type;
    using dftracer::utils::dataframe::is_temporal_type;
    using dftracer::utils::dataframe::physical_type;
    using dftracer::utils::dataframe::promote_for_arithmetic;
    using dftracer::utils::dataframe::type_name;
    if (v->encoding != dftracer::utils::dataframe::Encoding::Flat) return -1;
    if (is_temporal_type(v->type)) {
        std::int64_t idx = -1;
        DF_NUMERIC_DISPATCH(physical_type(v->type), argextreme_one, *v, false,
                            idx)
        return idx;
    }
    if (!is_arithmetic_type(v->type)) {
        DFTRACER_UTILS_LOG_ERROR("arg_max: no meaning for type '%s'",
                                 type_name(v->type));
        return -1;
    }
    dftu_series* promoted = nullptr;
    v = promote_for_arithmetic(v, promoted);
    std::int64_t idx = -1;
    if (!dftracer::utils::dataframe::arg_extreme_simd(*v, false, idx))
        DF_NUMERIC_DISPATCH(v->type, argextreme_one, *v, false, idx)
    if (promoted) dftu_series_free(promoted);
    return idx;
}

dftu_scalar dftu_series_mode(const dftu_series* v) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_mode(flat_v));

    using dftracer::utils::dataframe::is_arithmetic_type;
    using dftracer::utils::dataframe::is_temporal_type;
    using dftracer::utils::dataframe::physical_type;
    using dftracer::utils::dataframe::promote_for_arithmetic;
    using dftracer::utils::dataframe::type_name;
    dftu_scalar out{};
    out.kind = DFTU_SCALAR_TAG_I64;
    if (v->encoding != dftracer::utils::dataframe::Encoding::Flat) return out;
    if (is_temporal_type(v->type)) {
        DF_NUMERIC_DISPATCH(physical_type(v->type), mode_one, *v, out)
        return out;
    }
    if (!is_arithmetic_type(v->type)) {
        DFTRACER_UTILS_LOG_ERROR("mode: no meaning for type '%s'",
                                 type_name(v->type));
        return out;
    }
    dftu_series* promoted = nullptr;
    v = promote_for_arithmetic(v, promoted);
    DF_NUMERIC_DISPATCH(v->type, mode_one, *v, out)
    if (promoted) dftu_series_free(promoted);
    return out;
}
