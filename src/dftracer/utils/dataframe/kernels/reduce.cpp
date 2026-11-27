// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/column_read.h>
#include <dftracer/utils/dataframe/internal/numeric_dispatch.h>
#include <dftracer/utils/dataframe/internal/radix_dedup.h>  // parallel dedup
#include <dftracer/utils/dataframe/internal/reduce_simd.h>
#include <dftracer/utils/dataframe/internal/scalar.h>
#include <dftracer/utils/dataframe/internal/string_reader.h>
#include <dftracer/utils/dataframe/internal/type_promotion.h>
#include <dftracer/utils/dataframe/kernels/reduce.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <algorithm>
#include <cstdint>
#include <type_traits>

namespace dftracer::utils::dataframe {

dstd::Result<dftu_scalar> sum(const Series& v) {
    return reduction_result(dftu_series_reduce(v.handle(), DFTU_REDUCE_SUM));
}
dstd::Result<dftu_scalar> min(const Series& v) {
    return reduction_result(dftu_series_reduce(v.handle(), DFTU_REDUCE_MIN));
}
dstd::Result<dftu_scalar> max(const Series& v) {
    return reduction_result(dftu_series_reduce(v.handle(), DFTU_REDUCE_MAX));
}
std::int64_t count(const Series& v) { return dftu_series_count(v.handle()); }

dstd::Result<double> mean(const Series& v) {
    const dstd::Result<dftu_scalar> total_sum = sum(v);
    if (!total_sum) return total_sum.error();
    const double total = scalar_value<double>(*total_sum);
    const std::int64_t n = count(v);
    return n == 0 ? 0.0 : total / static_cast<double>(n);
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
    if (!v.validity.block()) return true;
    return (v.validity.data()[i >> 3] & (1u << (i & 7))) != 0;
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
    const dftracer::utils::dataframe::ColumnCells<T> rows(v);
    const std::int64_t n = v.length;
    // A validity bitmap forces a per-element branch that blocks the compiler
    // from vectorizing the reduction; the far more common no-null column runs a
    // branch-free loop that auto-vectorizes (reduce_simd handles f64 sum before
    // we get here, but integer sum and all min/max land on this path).
    const bool has_null = rows.maybe_null();

    if (op == DFTU_REDUCE_SUM) {
        sum_acc_t<T> acc = 0;
        if (has_null) {
            for (std::int64_t i = 0; i < n; ++i)
                if (rows.valid(i)) acc += static_cast<sum_acc_t<T>>(rows.at(i));
        } else {
            for (std::int64_t i = 0; i < n; ++i)
                acc += static_cast<sum_acc_t<T>>(rows.at(i));
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
            if (!rows.valid(i)) continue;
            const T x = rows.at(i);
            if (!found) {
                best = x;
                found = true;
            } else if (is_min) {
                if (x < best) best = x;
            } else if (x > best) {
                best = x;
            }
        }
    } else if (n > 0) {
        // op hoisted out of the loop so each stays a branch-free min/max
        // reduction the compiler can vectorize.
        best = rows.at(0);
        if (is_min) {
            for (std::int64_t i = 1; i < n; ++i)
                if (rows.at(i) < best) best = rows.at(i);
        } else {
            for (std::int64_t i = 1; i < n; ++i)
                if (rows.at(i) > best) best = rows.at(i);
        }
    }
    store_domain<T>(out, best);
}

// Product accumulates in the column's wide domain, skipping nulls (identity 1).
template <class T>
void product_one(const dftu_series& v, dftu_scalar& out) {
    const dftracer::utils::dataframe::ColumnCells<T> rows(v);
    const std::int64_t n = v.length;
    const bool has_null = rows.maybe_null();
    // Integers multiply in uint64, which wraps; a signed product would
    // overflow.
    using Acc =
        std::conditional_t<std::is_floating_point_v<T>, double, std::uint64_t>;
    Acc acc = 1;
    for (std::int64_t i = 0; i < n; ++i)
        if (!has_null || rows.valid(i))
            acc *= static_cast<Acc>(static_cast<sum_acc_t<T>>(rows.at(i)));
    if constexpr (std::is_floating_point_v<T>) {
        out.kind = DFTU_SCALAR_TAG_F64;
        out.value.d = acc;
    } else if constexpr (std::is_unsigned_v<T>) {
        out.kind = DFTU_SCALAR_TAG_U64;
        out.value.u = acc;
    } else {
        out.kind = DFTU_SCALAR_TAG_I64;
        out.value.i = static_cast<std::int64_t>(acc);
    }
}

// The bitwise reduction `op` (0 AND, 1 OR, 2 XOR) over the valid values of an
// integer column; a column with no valid value gives the NULL scalar. A signed
// column gives I64 (sign-extended from its width), an unsigned one U64.
template <class T>
void bitwise_one(const dftu_series& v, int op, dftu_scalar& out) {
    const dftracer::utils::dataframe::ColumnCells<T> rows(v);
    const bool has_null = rows.maybe_null();
    bool seen = false;
    T acc{};
    for (std::int64_t i = 0; i < v.length; ++i) {
        if (has_null && !rows.valid(i)) continue;
        const T x = rows.at(i);
        if (!seen) {
            acc = x;
            seen = true;
        } else if (op == 0) {
            acc = static_cast<T>(acc & x);
        } else if (op == 1) {
            acc = static_cast<T>(acc | x);
        } else {
            acc = static_cast<T>(acc ^ x);
        }
    }
    if (!seen) {
        out.kind = DFTU_SCALAR_TAG_NULL;
        out.value.i = 0;
    } else if constexpr (std::is_unsigned_v<T>) {
        out.kind = DFTU_SCALAR_TAG_U64;
        out.value.u = static_cast<std::uint64_t>(acc);
    } else {
        out.kind = DFTU_SCALAR_TAG_I64;
        out.value.i = static_cast<std::int64_t>(acc);
    }
}

// All/any and the bool bitwise ops over blocks of a non-FLAT Bool column: the
// gathered bytes are 0/1, so each fold gives the answer the bit scan gives.
struct BitFold {
    bool seen = false;
    bool value = false;
};

template <class Partial, class Fold, class Merge>
bool bool_blocks(const dftu_series& v, Partial identity, Fold&& fold,
                 Merge&& merge, Partial& out) {
    using dftracer::utils::dataframe::each_block;
    using dftracer::utils::dataframe::RowBlock;
    const RowBlock<std::uint8_t> block(v, true);
    if (!block.usable()) return false;
    out = each_block<std::uint8_t, Partial>(
        block, 0, v.length, identity,
        [&](Partial& acc, std::int64_t, std::int64_t count, std::uint8_t* buf,
            std::uint8_t* valid, std::int64_t) {
            for (std::int64_t k = 0; k < count; ++k)
                if (((valid[k >> 3] >> (k & 7)) & 1) != 0)
                    acc = fold(acc, buf[k] != 0);
        },
        merge);
    return true;
}

// The same over a bit-packed Bool column: an I64 scalar of 0 or 1.
void bitwise_bool(const dftu_series& v, int op, dftu_scalar& out) {
    BitFold folded;
    if (v.encoding != dftracer::utils::dataframe::Encoding::Flat &&
        bool_blocks<BitFold>(
            v, BitFold{},
            [op](BitFold a, bool bit) {
                if (!a.seen) {
                    a.seen = true;
                    a.value = bit;
                } else if (op == 0) {
                    a.value = a.value && bit;
                } else if (op == 1) {
                    a.value = a.value || bit;
                } else {
                    a.value = a.value != bit;
                }
                return a;
            },
            [op](BitFold a, const BitFold& b) {
                if (!b.seen) return a;
                if (!a.seen) return b;
                if (op == 0) {
                    a.value = a.value && b.value;
                } else if (op == 1) {
                    a.value = a.value || b.value;
                } else {
                    a.value = a.value != b.value;
                }
                return a;
            },
            folded)) {
        out.kind = folded.seen ? DFTU_SCALAR_TAG_I64 : DFTU_SCALAR_TAG_NULL;
        out.value.i = folded.seen && folded.value ? 1 : 0;
        return;
    }
    const dftracer::utils::dataframe::ColumnCells<std::uint8_t> rows(v);
    const bool has_null = rows.maybe_null();
    bool seen = false;
    bool acc = false;
    for (std::int64_t i = 0; i < v.length; ++i) {
        if (has_null && !rows.valid(i)) continue;
        const bool bit = rows.bool_at(i);
        if (!seen)
            acc = bit;
        else if (op == 0)
            acc = acc && bit;
        else if (op == 1)
            acc = acc || bit;
        else
            acc = acc != bit;
        seen = true;
    }
    out.kind = seen ? DFTU_SCALAR_TAG_I64 : DFTU_SCALAR_TAG_NULL;
    out.value.i = seen && acc ? 1 : 0;
}

template <class T>
void argextreme_one(const dftu_series& v, bool is_min, std::int64_t& out_idx) {
    const dftracer::utils::dataframe::ColumnCells<T> rows(v);
    const std::int64_t n = v.length;
    const bool has_null = rows.maybe_null();
    out_idx = -1;
    T best{};
    for (std::int64_t i = 0; i < n; ++i) {
        if (has_null && !rows.valid(i)) continue;
        const T x = rows.at(i);
        if (out_idx < 0) {
            best = x;
            out_idx = i;
        } else if (is_min ? (x < best) : (x > best)) {
            best = x;
            out_idx = i;
        }
    }
}

// Serial fallback (and reference algorithm): strictly-greater keeps the value
// that first reached the top count, so ties resolve to whichever value's
// count first attains the eventual maximum.
template <class T>
void mode_serial(dftracer::utils::dstd::Span<const T> vals, dftu_scalar& out) {
    dftracer::utils::dstd::HashMap<T, std::int64_t> counts;
    T best{};
    std::int64_t best_count = 0;
    for (const T x : vals) {
        const std::int64_t c = ++counts[x];
        if (c > best_count) {
            best_count = c;
            best = x;
        }
    }
    store_domain<T>(out, best);
}

// The valid values of `v` in row order, each chunk of a CHUNKED column read
// through its own cells. A float NaN is missing, as in pandas' mode, and -0.0
// is 0.0: a zero is counted with its sign dropped.
template <class T>
void collect_mode_values(const dftu_series& v,
                         dftracer::utils::dstd::Vector<T>& vals) {
    using dftracer::utils::dataframe::ColumnCells;
    if (v.is_chunked()) {
        for (const auto& chunk : v.nested)
            if (chunk.series) collect_mode_values<T>(*chunk.series, vals);
        return;
    }
    const ColumnCells<T> cells(v);
    const bool has_null = cells.maybe_null();
    for (std::int64_t i = 0; i < v.length; ++i) {
        if (has_null && !cells.valid(i)) continue;
        T x = cells.at(i);
        if constexpr (std::is_floating_point_v<T>) {
            if (x != x) continue;
            if (x == T{}) x = T{};
        }
        vals.push_back(x);
    }
}

template <class T>
void mode_one(const dftu_series& v, dftu_scalar& out) {
    using dftracer::utils::dataframe::radix_counts_by;
    dftracer::utils::dstd::Vector<T> vals;
    vals.reserve(static_cast<std::size_t>(v.length));
    collect_mode_values<T>(v, vals);
    const std::int64_t m = static_cast<std::int64_t>(vals.size());

    if (m == 0) {
        store_domain<T>(out, T{});
        return;
    }
    if (!dftracer::utils::dataframe::parallel_backend_installed() ||
        m < dftracer::utils::dataframe::RADIX_DEDUP_MIN_ROWS) {
        mode_serial<T>(vals, out);
        return;
    }

    // Frequency of each valid position's value, radix-partitioned and merged
    // (the same shape as group_agg Count). A unique highest-frequency value
    // wins outright; a tie in the final count falls back to a bounded serial
    // rescan restricted to the tied candidates, to reproduce the sequential
    // "first-reaching-top" tie-break exactly (the last event on the winning
    // path can only ever be caused by a value at the eventual maximum count,
    // so skipping non-candidate rows cannot change the outcome).
    dftracer::utils::dstd::Vector<std::int64_t> counts = radix_counts_by<T>(
        m, [&](std::int64_t j) { return vals[static_cast<std::size_t>(j)]; });
    std::int64_t best_count = 0;
    for (std::int64_t c : counts) best_count = std::max(best_count, c);

    dftracer::utils::dstd::HashSet<T> candidates;
    for (std::int64_t j = 0; j < m; ++j)
        if (counts[static_cast<std::size_t>(j)] == best_count)
            candidates.insert(vals[static_cast<std::size_t>(j)]);

    if (candidates.size() == 1) {
        store_domain<T>(out, *candidates.begin());
        return;
    }

    dftracer::utils::dstd::Vector<T> tied;
    for (const T x : vals)
        if (candidates.count(x) != 0) tied.push_back(x);
    mode_serial<T>(tied, out);
}

const char* reduce_op_name(std::int32_t op) {
    switch (op) {
        case DFTU_REDUCE_SUM:
            return "sum";
        case DFTU_REDUCE_MIN:
            return "min";
        case DFTU_REDUCE_MAX:
            return "max";
        case DFTU_REDUCE_COUNT:
            return "count";
        case DFTU_REDUCE_MEAN:
            return "mean";
    }
    return "op";
}

// The ERR scalar a reduction returns instead of a value. The error and its text
// live in thread-local storage, so they stay valid until the next reduction on
// this thread (dftu_error borrows its message).
dftu_scalar refuse(dftracer::utils::dstd::StringView message) {
    static thread_local dftracer::utils::dstd::String text;
    static thread_local dftu_error err;
    text = message;
    err.domain = dftracer::utils::dstd::CORE_ERROR_DOMAIN.id;
    err.code = static_cast<std::int32_t>(
        dftracer::utils::dstd::Condition::Unsupported);
    err.status = DFTU_E_UNSUPPORTED;
    err.message = text.c_str();
    dftu_scalar out{};
    out.kind = DFTU_SCALAR_TAG_ERR;
    out.value.err = &err;
    return out;
}

dftu_scalar refuse_type(std::int32_t op, dftracer::utils::dataframe::TypeId t) {
    return refuse(
        dftracer::utils::dstd::fmt<"reduce: {} is not defined for type '{}'">(
            reduce_op_name(op), dftracer::utils::dataframe::type_name(t)));
}

// Smallest (MIN) or largest (MAX) valid value of a String or LargeString
// column, bytewise. The STR scalar borrows the value from the column; a column
// with no valid value gives the empty string, as a numeric column gives 0.
// Folds the valid rows of one non-CHUNKED string column into `best`, bytewise.
void scan_string(const dftu_series& v, bool is_min, bool& found,
                 dftracer::utils::dstd::StringView& best) {
    if (v.encoding != dftracer::utils::dataframe::Encoding::Flat) {
        dftracer::utils::dstd::StringView cur;
        for (std::int64_t i = 0; i < v.length; ++i) {
            if (!dftracer::utils::dataframe::str_at(&v, i, cur)) continue;
            if (!found || (is_min ? cur < best : cur > best)) {
                best = cur;
                found = true;
            }
        }
        return;
    }
    const char* base = static_cast<const char*>(dftu_series_data(&v));
    const bool wide = dftracer::utils::dataframe::is_wide_offset_type(v.type);
    const std::int32_t* off32 = wide ? nullptr : dftu_series_offsets(&v);
    const std::int64_t* off64 = wide ? dftu_series_offsets64(&v) : nullptr;
    if (base == nullptr || (off32 == nullptr && off64 == nullptr)) return;
    for (std::int64_t i = 0; i < v.length; ++i) {
        if (!is_valid(v, i)) continue;
        const std::int64_t lo = wide ? off64[i] : off32[i];
        const std::int64_t hi = wide ? off64[i + 1] : off32[i + 1];
        const dftracer::utils::dstd::StringView cur(
            base + lo, static_cast<std::size_t>(hi - lo));
        if (!found || (is_min ? cur < best : cur > best)) {
            best = cur;
            found = true;
        }
    }
}

dftu_scalar reduce_string(const dftu_series& v, std::int32_t op) {
    dftu_scalar out{};
    out.kind = DFTU_SCALAR_TAG_STR;
    out.len = 0;
    out.value.s = "";

    const bool is_min = op == DFTU_REDUCE_MIN;
    bool found = false;
    dftracer::utils::dstd::StringView best;
    if (v.encoding == dftracer::utils::dataframe::Encoding::Chunked) {
        for (const auto& chunk : v.nested)
            if (chunk.series) scan_string(*chunk.series, is_min, found, best);
    } else {
        scan_string(v, is_min, found, best);
    }
    if (found) {
        out.len = static_cast<std::uint32_t>(best.size());
        out.value.s = best.data();
    }
    return out;
}

double scalar_as_double(const dftu_scalar& s) {
    switch (s.kind) {
        case DFTU_SCALAR_TAG_U64:
            return static_cast<double>(s.value.u);
        case DFTU_SCALAR_TAG_F64:
            return s.value.d;
        default:
            return static_cast<double>(s.value.i);
    }
}

bool scalar_less(const dftu_scalar& a, const dftu_scalar& b) {
    if (a.kind == b.kind) {
        switch (a.kind) {
            case DFTU_SCALAR_TAG_U64:
                return a.value.u < b.value.u;
            case DFTU_SCALAR_TAG_F64:
                return a.value.d < b.value.d;
            default:
                return a.value.i < b.value.i;
        }
    }
    return scalar_as_double(a) < scalar_as_double(b);
}

bool is_float_like(dftracer::utils::dataframe::TypeId t) noexcept {
    using dftracer::utils::dataframe::TypeId;
    return t == TypeId::Float16 || t == TypeId::Float32 ||
           t == TypeId::Float64 || t == TypeId::Decimal128 ||
           t == TypeId::Decimal256;
}

// The SUM or the MIN/MAX of two reduced scalars (the chunks of a CHUNKED
// column). Chunks of one column share a type, so the kinds match; a mismatch
// (a U64 chunk against an I64 one) combines as double.

dftu_scalar combine_scalar(dftu_scalar acc, const dftu_scalar& r, int op) {
    if (op == DFTU_REDUCE_MIN) return scalar_less(r, acc) ? r : acc;
    if (op == DFTU_REDUCE_MAX) return scalar_less(acc, r) ? r : acc;
    if (acc.kind != r.kind) {
        acc.kind = DFTU_SCALAR_TAG_F64;
        acc.value.d = scalar_as_double(acc) + scalar_as_double(r);
        return acc;
    }
    switch (acc.kind) {
        case DFTU_SCALAR_TAG_U64:
            acc.value.u += r.value.u;
            break;
        case DFTU_SCALAR_TAG_F64:
            acc.value.d += r.value.d;
            break;
        default:
            acc.value.i = static_cast<std::int64_t>(
                static_cast<std::uint64_t>(acc.value.i) +
                static_cast<std::uint64_t>(r.value.i));
    }
    return acc;
}

// A borrowed FLAT series over one gathered block, so a kernel's own FLAT path
// runs on the block unchanged (SIMD included).
template <class T>
dftu_series* borrow_block(const dftu_series& v, T* buf, std::uint8_t* valid,
                          std::int64_t n, std::int64_t nvalid) noexcept {
    return dftu_series_new_flat_borrowed(
        static_cast<dftu_dtype>(physical_type(v.type)), buf, n,
        nvalid == n ? nullptr : valid, nullptr, nullptr);
}

dftu_scalar null_scalar() noexcept {
    dftu_scalar out{};
    out.kind = DFTU_SCALAR_TAG_NULL;
    return out;
}

// True for a physical type whose FLAT kernel is one serial pass over a whole
// contiguous buffer: splitting it into blocks changes the bits.
bool serial_only_layout(dftracer::utils::dataframe::TypeId t) noexcept {
    using dftracer::utils::dataframe::TypeId;
    return t == TypeId::Float32 || t == TypeId::Float64;
}

// The FLAT reduce on one block.
template <class T>
dftu_scalar reduce_block(const dftu_series& v, std::int32_t op, T* buf,
                         std::uint8_t* valid, std::int64_t n,
                         std::int64_t nvalid) {
    dftu_series* part = borrow_block(v, buf, valid, n, nvalid);
    if (part == nullptr) return null_scalar();
    const dftu_scalar out = dftu_series_reduce(part, op);
    dftu_series_free(part);
    return out;
}

// A non-FLAT column reduced a range of blocks at a time: RowBlock gathers
// CAPACITY rows (a bounded copy) and the same FLAT reduce runs on the block,
// the ranges fanned out over the parallel backend. Sum, min and max fold over
// ranges exactly as over rows, so the result matches the materialized column.
// False when the layer chain cannot be resolved (a CHUNKED column is reduced
// chunk by chunk instead) or the type must stay serial.
template <class T>
bool reduce_blocks(const dftu_series& v, std::int32_t op, dftu_scalar& out,
                   bool& handled) {
    using dftracer::utils::dataframe::each_block;
    using dftracer::utils::dataframe::RowBlock;
    if constexpr (std::is_floating_point_v<T>) {
        return false;
    } else {
        const RowBlock<T> block(v);
        if (!block.usable()) return false;
        handled = true;
        out = each_block<T>(
            block, 0, v.length, null_scalar(),
            [&](dftu_scalar& acc, std::int64_t, std::int64_t count, T* buf,
                std::uint8_t* valid, std::int64_t nvalid) {
                if (nvalid == 0 && op != DFTU_REDUCE_SUM) return;
                const dftu_scalar r =
                    reduce_block(v, op, buf, valid, count, nvalid);
                if (r.kind == DFTU_SCALAR_TAG_ERR) {
                    acc = r;
                    return;
                }
                if (r.kind == DFTU_SCALAR_TAG_NULL) return;
                acc = acc.kind == DFTU_SCALAR_TAG_NULL
                          ? r
                          : combine_scalar(acc, r, op);
            },
            [op](dftu_scalar a, const dftu_scalar& b) {
                if (a.kind == DFTU_SCALAR_TAG_ERR) return a;
                if (b.kind == DFTU_SCALAR_TAG_ERR) return b;
                if (b.kind == DFTU_SCALAR_TAG_NULL) return a;
                if (a.kind == DFTU_SCALAR_TAG_NULL) return b;
                return combine_scalar(a, b, op);
            });
        if (out.kind == DFTU_SCALAR_TAG_NULL)
            out = reduce_block<T>(v, op, nullptr, nullptr, 0, 0);
        return true;
    }
}

// The same, dispatched on the column's physical type.
inline bool reduce_in_blocks(const dftu_series& v, std::int32_t op,
                             dftu_scalar& out) {
    bool handled = false;
    DFTRACER_UTILS_NUMERIC_DISPATCH(physical_type(v.type), reduce_blocks, v, op,
                                    out, handled);
    return handled;
}

// Products merge by multiplying in the wide domain, which is the same value the
// row fold reaches (a narrow type's fold is exact in that domain).
dftu_scalar combine_product(dftu_scalar a, const dftu_scalar& b) {
    if (a.kind == DFTU_SCALAR_TAG_NULL) return b;
    if (b.kind == DFTU_SCALAR_TAG_NULL) return a;
    dftu_scalar out = a;
    if (a.kind == DFTU_SCALAR_TAG_U64 || b.kind == DFTU_SCALAR_TAG_U64) {
        const std::uint64_t x = a.kind == DFTU_SCALAR_TAG_U64
                                    ? a.value.u
                                    : static_cast<std::uint64_t>(a.value.i);
        const std::uint64_t y = b.kind == DFTU_SCALAR_TAG_U64
                                    ? b.value.u
                                    : static_cast<std::uint64_t>(b.value.i);
        out.kind = DFTU_SCALAR_TAG_U64;
        out.value.u = x * y;
        return out;
    }
    out.kind = DFTU_SCALAR_TAG_I64;
    out.value.i =
        static_cast<std::int64_t>(static_cast<std::uint64_t>(a.value.i) *
                                  static_cast<std::uint64_t>(b.value.i));
    return out;
}

// The FLAT product on one block.
template <class T>
dftu_scalar product_block(const dftu_series& v, T* buf, std::uint8_t* valid,
                          std::int64_t n, std::int64_t nvalid) {
    dftu_series* part = borrow_block(v, buf, valid, n, nvalid);
    if (part == nullptr) return null_scalar();
    const dftu_scalar out = dftu_series_product(part);
    dftu_series_free(part);
    return out;
}

// A non-FLAT integer column's product over blocks; false to stay serial.
template <class T>
bool product_blocks(const dftu_series& v, dftu_scalar& out, bool& handled) {
    using dftracer::utils::dataframe::each_block;
    using dftracer::utils::dataframe::RowBlock;
    if constexpr (std::is_floating_point_v<T>) {
        return false;
    } else {
        const RowBlock<T> block(v);
        if (!block.usable()) return false;
        handled = true;
        out = each_block<T>(
            block, 0, v.length, null_scalar(),
            [&](dftu_scalar& acc, std::int64_t, std::int64_t count, T* buf,
                std::uint8_t* valid, std::int64_t nvalid) {
                const dftu_scalar r =
                    product_block(v, buf, valid, count, nvalid);
                acc = acc.kind == DFTU_SCALAR_TAG_NULL
                          ? r
                          : combine_product(acc, r);
            },
            [](dftu_scalar a, dftu_scalar b) {
                return combine_product(std::move(a), b);
            });
        if (out.kind == DFTU_SCALAR_TAG_NULL)
            out = product_block<T>(v, nullptr, nullptr, 0, 0);
        return true;
    }
}

// The product of a CHUNKED integer column: each chunk runs its own kernel and
// the products multiply in uint64, which wraps as the row fold does.
dftu_scalar product_by_chunk(const dftu_series& v) {
    dftu_scalar acc = null_scalar();
    for (const auto& chunk : v.nested)
        if (chunk.series)
            acc = combine_product(acc, dftu_series_product(chunk.series.get()));
    if (acc.kind != DFTU_SCALAR_TAG_NULL) return acc;
    return product_block<std::uint8_t>(v, nullptr, nullptr, 0, 0);
}

inline bool product_in_blocks(const dftu_series& v, dftu_scalar& out) {
    bool handled = false;
    DFTRACER_UTILS_NUMERIC_DISPATCH(physical_type(v.type), product_blocks, v,
                                    out, handled);
    return handled;
}

// Bitwise ranges merge in the wide domain, which equals the sign or zero
// extension of the narrow result (the top bit of AND/OR/XOR is the op of the
// narrow top bits).
dftu_scalar combine_bits(dftu_scalar a, const dftu_scalar& b, int op) {
    if (a.kind == DFTU_SCALAR_TAG_NULL) return b;
    if (b.kind == DFTU_SCALAR_TAG_NULL) return a;
    dftu_scalar out = a;
    if (a.kind == DFTU_SCALAR_TAG_U64 || b.kind == DFTU_SCALAR_TAG_U64) {
        const std::uint64_t x = a.kind == DFTU_SCALAR_TAG_U64
                                    ? a.value.u
                                    : static_cast<std::uint64_t>(a.value.i);
        const std::uint64_t y = b.kind == DFTU_SCALAR_TAG_U64
                                    ? b.value.u
                                    : static_cast<std::uint64_t>(b.value.i);
        out.kind = DFTU_SCALAR_TAG_U64;
        out.value.u = op == 0 ? (x & y) : op == 1 ? (x | y) : (x ^ y);
        return out;
    }
    const std::int64_t x = a.value.i;
    const std::int64_t y = b.value.i;
    out.kind = DFTU_SCALAR_TAG_I64;
    out.value.i = op == 0 ? (x & y) : op == 1 ? (x | y) : (x ^ y);
    return out;
}

// The FLAT bitwise reduction `op` on one block.
template <class T>
dftu_scalar bitwise_block(const dftu_series& v, int op, T* buf,
                          std::uint8_t* valid, std::int64_t n,
                          std::int64_t nvalid) {
    dftu_series* part = borrow_block(v, buf, valid, n, nvalid);
    if (part == nullptr) return null_scalar();
    const dftu_scalar out = op == 0   ? dftu_series_bitwise_and(part)
                            : op == 1 ? dftu_series_bitwise_or(part)
                                      : dftu_series_bitwise_xor(part);
    dftu_series_free(part);
    return out;
}

template <class T>
bool bitwise_blocks(const dftu_series& v, int op, dftu_scalar& out,
                    bool& handled) {
    using dftracer::utils::dataframe::each_block;
    using dftracer::utils::dataframe::RowBlock;
    if constexpr (std::is_floating_point_v<T>) {
        return false;
    } else {
        const RowBlock<T> block(v);
        if (!block.usable()) return false;
        handled = true;
        out = each_block<T>(
            block, 0, v.length, null_scalar(),
            [&](dftu_scalar& acc, std::int64_t, std::int64_t count, T* buf,
                std::uint8_t* valid, std::int64_t nvalid) {
                const dftu_scalar r =
                    bitwise_block(v, op, buf, valid, count, nvalid);
                acc = acc.kind == DFTU_SCALAR_TAG_NULL
                          ? r
                          : combine_bits(acc, r, op);
            },
            [op](dftu_scalar a, dftu_scalar b) {
                return combine_bits(std::move(a), b, op);
            });
        return true;
    }
}

inline bool bitwise_in_blocks(const dftu_series& v, int op, dftu_scalar& out) {
    bool handled = false;
    DFTRACER_UTILS_NUMERIC_DISPATCH(physical_type(v.type), bitwise_blocks, v,
                                    op, out, handled);
    return handled;
}

// The extreme of one block: its value and its index in the column, -1 when the
// block has no valid row.
template <class T>
struct ArgExtreme {
    T value{};
    std::int64_t idx = -1;
};

// A non-FLAT integer column's arg_min/arg_max over blocks. The ranges merge in
// row order and a tie keeps the earlier range, so the earliest extreme wins as
// in the row fold.
template <class T>
bool arg_blocks(const dftu_series& v, bool is_min, std::int64_t& out,
                bool& handled) {
    using dftracer::utils::dataframe::each_block;
    using dftracer::utils::dataframe::RowBlock;
    if constexpr (std::is_floating_point_v<T>) {
        return false;
    } else {
        const RowBlock<T> block(v);
        if (!block.usable()) return false;
        handled = true;
        const ArgExtreme<T> acc = each_block<T>(
            block, 0, v.length, ArgExtreme<T>{},
            [&](ArgExtreme<T>& a, std::int64_t from, std::int64_t count, T* buf,
                std::uint8_t* valid, std::int64_t nvalid) {
                if (nvalid == 0) return;
                dftu_series* part = borrow_block(v, buf, valid, count, nvalid);
                if (part == nullptr) return;
                const std::int64_t local = is_min ? dftu_series_arg_min(part)
                                                  : dftu_series_arg_max(part);
                dftu_series_free(part);
                if (local < 0) return;
                const T x = buf[local];
                if (a.idx < 0) {
                    a.value = x;
                    a.idx = from + local;
                    return;
                }
                if (is_min ? (x < a.value) : (x > a.value)) {
                    a.value = x;
                    a.idx = from + local;
                }
            },
            [is_min](ArgExtreme<T> a, const ArgExtreme<T>& b) {
                if (b.idx < 0) return a;
                if (a.idx < 0) return b;
                if (is_min ? (b.value < a.value) : (b.value > a.value))
                    return b;
                return a;
            });
        out = acc.idx;
        return true;
    }
}

inline bool arg_in_blocks(const dftu_series& v, bool is_min,
                          std::int64_t& out) {
    bool handled = false;
    DFTRACER_UTILS_NUMERIC_DISPATCH(physical_type(v.type), arg_blocks, v,
                                    is_min, out, handled);
    return handled;
}

// arg_min/arg_max of a CHUNKED integer or temporal column: each chunk runs its
// own kernel and the winners merge in chunk order, a tie keeping the earlier
// chunk, so the lowest index of the extreme wins as in the row fold.
std::int64_t arg_by_chunk(const dftu_series& v, bool is_min) {
    using dftracer::utils::dataframe::cell_i64;
    using dftracer::utils::dataframe::cell_u64;
    const auto* starts = reinterpret_cast<const std::int64_t*>(v.data.data());
    using dftracer::utils::dataframe::TypeId;
    const bool is_unsigned =
        v.type == TypeId::Uint8 || v.type == TypeId::Uint16 ||
        v.type == TypeId::Uint32 || v.type == TypeId::Uint64;
    std::int64_t best_at = -1;
    std::int64_t best_i = 0;
    std::uint64_t best_u = 0;
    for (std::size_t k = 0; k < v.nested.size(); ++k) {
        const dftu_series* part = v.nested[k].series.get();
        if (part == nullptr) continue;
        const std::int64_t local =
            is_min ? dftu_series_arg_min(part) : dftu_series_arg_max(part);
        if (local < 0) continue;
        bool better = best_at < 0;
        if (is_unsigned) {
            const std::uint64_t x = cell_u64(part, local);
            better = better || (is_min ? x < best_u : x > best_u);
            if (better) best_u = x;
        } else {
            const std::int64_t x = cell_i64(part, local);
            better = better || (is_min ? x < best_i : x > best_i);
            if (better) best_i = x;
        }
        if (better) best_at = starts[k] + local;
    }
    return best_at;
}

}  // namespace

dftu_scalar dftu_series_reduce(const dftu_series* v, dftu_reduce_op op) {
    using dftracer::utils::dataframe::Encoding;
    using dftracer::utils::dataframe::is_arithmetic_type;
    using dftracer::utils::dataframe::is_temporal_type;
    using dftracer::utils::dataframe::physical_type;
    using dftracer::utils::dataframe::promote_for_arithmetic;
    using dftracer::utils::dataframe::TypeId;
    dftu_scalar out{};
    out.kind = DFTU_SCALAR_TAG_I64;

    // One column, one SUM/MIN/MAX flag: COUNT and MEAN are group_by flags, and
    // any other code is not an operation. The numeric kernel below treats every
    // code that is not SUM or MIN as MAX, so refuse them here.
    if (op != DFTU_REDUCE_SUM && op != DFTU_REDUCE_MIN && op != DFTU_REDUCE_MAX)
        return refuse(
            dftracer::utils::dstd::fmt<
                "reduce: {} is not a single-column reduction (sum, min "
                "or max)">(reduce_op_name(static_cast<std::int32_t>(op))));

    // A CHUNKED column reduces chunk by chunk: every chunk is read by the same
    // dispatch below, so a FLAT chunk keeps the SIMD path and no copy of the
    // whole column is made. The per-chunk scalars are combined here, a chunk
    // with no valid row left out. A float column joins its chunks first: the
    // kernel is one serial pass, so merged partials would change the bits and
    // the NaN rule.
    if (v->encoding == Encoding::Chunked) {
        const bool string_column =
            v->type == TypeId::String || v->type == TypeId::LargeString;
        if (!string_column && is_float_like(v->type)) {
            dftu_series* flat = dftu_series_materialize(v);
            if (flat == nullptr) return refuse_type(op, v->type);
            const dftu_scalar r = dftu_series_reduce(flat, op);
            dftu_series_free(flat);
            return r;
        }
        if (!string_column) {
            dftu_scalar acc{};
            acc.kind = DFTU_SCALAR_TAG_NULL;
            dftu_scalar first{};
            first.kind = DFTU_SCALAR_TAG_NULL;
            const std::int32_t nchunks = dftu_series_chunk_count(v);
            for (std::int32_t c = 0; c < nchunks; ++c) {
                dftu_series* part = dftu_series_chunk(v, c);
                if (part == nullptr) continue;
                const bool has_valid = dftu_series_count(part) > 0;
                const dftu_scalar r = dftu_series_reduce(part, op);
                dftu_series_free(part);
                if (r.kind == DFTU_SCALAR_TAG_ERR) return r;
                if (first.kind == DFTU_SCALAR_TAG_NULL) first = r;
                if (!has_valid || r.kind == DFTU_SCALAR_TAG_NULL) continue;
                acc = acc.kind == DFTU_SCALAR_TAG_NULL
                          ? r
                          : combine_scalar(acc, r, op);
            }
            if (acc.kind != DFTU_SCALAR_TAG_NULL) return acc;
            if (first.kind != DFTU_SCALAR_TAG_NULL) return first;
            return reduce_block<std::uint8_t>(*v, op, nullptr, nullptr, 0, 0);
        }
    }

    // MIN/MAX on any temporal column dispatch on its physical Int32/Int64
    // layout. SUM is additionally defined for Duration (a sum of durations
    // is a duration, in the same unit); every other temporal type has no
    // meaningful SUM (summing instants or times-of-day is not a rule this
    // engine defines) and every other op stays refused.
    if (is_temporal_type(v->type)) {
        const bool sum_ok =
            op == DFTU_REDUCE_SUM && v->type == TypeId::Duration;
        if (op != DFTU_REDUCE_MIN && op != DFTU_REDUCE_MAX && !sum_ok)
            return refuse_type(op, v->type);
        DFTRACER_UTILS_NUMERIC_DISPATCH(physical_type(v->type), reduce_one, *v,
                                        op, out)
        return out;
    }

    // A bool counts true as 1, as the group-by engine does: reduce it as int64.
    if (v->type == TypeId::Bool) {
        dftu_series* as_int = dftu_series_cast(v, DFTU_TYPE_INT64);
        if (as_int == nullptr) return refuse_type(op, v->type);
        dftu_scalar r = dftu_series_reduce(as_int, op);
        dftu_series_free(as_int);
        return r;
    }

    if (v->type == TypeId::String || v->type == TypeId::LargeString) {
        if (op == DFTU_REDUCE_SUM) return refuse_type(op, v->type);
        return reduce_string(*v, op);
    }

    if (!is_arithmetic_type(v->type)) return refuse_type(op, v->type);

    // Float16 has no reduction kernel; Decimal128/256 have no exact one. Both
    // promote here, once, before any dispatch below sees them.
    dftu_series* promoted = nullptr;
    v = promote_for_arithmetic(v, promoted);

    if (v->encoding == Encoding::Flat) {
        if (dftracer::utils::dataframe::reduce(*v, op, out)) {
            if (promoted) dftu_series_free(promoted);
            return out;
        }
        DFTRACER_UTILS_NUMERIC_DISPATCH(v->type, reduce_one, *v, op, out)
        if (promoted) dftu_series_free(promoted);
        return out;
    }
    // A float kernel is one serial pass over a contiguous buffer, so only the
    // materialized column reproduces its bits block for block: copy it once,
    // as the pre-block path did, and keep the parallel gather's speed.
    if (serial_only_layout(v->type)) {
        dftu_series* flat = dftu_series_materialize(v);
        if (flat != nullptr) {
            const dftu_scalar r = dftu_series_reduce(flat, op);
            dftu_series_free(flat);
            if (promoted) dftu_series_free(promoted);
            return r;
        }
    }
    // A non-FLAT column gathers a block of rows and runs the same FLAT reduce
    // on it, so the SIMD path stays and no copy of the column is made.
    if (reduce_in_blocks(*v, op, out)) {
        if (promoted) dftu_series_free(promoted);
        return out;
    }
    DFTRACER_UTILS_NUMERIC_DISPATCH(v->type, reduce_one, *v, op, out)
    if (promoted) dftu_series_free(promoted);
    return out;
}

std::int64_t dftu_series_count(const dftu_series* v) {
    if (v->rowwise_nulls()) {
        std::int64_t n = 0;
        for (std::int64_t i = 0; i < v->length; ++i)
            n += !dftu_series_is_null(v, i);
        return n;
    }
    return v->length - v->null_count;
}

dftu_scalar dftu_series_product(const dftu_series* v) {
    using dftracer::utils::dataframe::is_arithmetic_type;
    using dftracer::utils::dataframe::is_temporal_type;
    using dftracer::utils::dataframe::promote_for_arithmetic;
    using dftracer::utils::dataframe::type_name;
    dftu_scalar out{};
    out.kind = DFTU_SCALAR_TAG_I64;
    if (is_temporal_type(v->type)) {
        DFTRACER_UTILS_LOG_ERROR("product: not defined for temporal type '{}'",
                                 type_name(v->type));
        return out;
    }
    if (!is_arithmetic_type(v->type)) {
        DFTRACER_UTILS_LOG_ERROR("product: no meaning for type '{}'",
                                 type_name(v->type));
        return out;
    }
    dftu_series* promoted = nullptr;
    v = promote_for_arithmetic(v, promoted);
    if (v->encoding == dftracer::utils::dataframe::Encoding::Flat &&
        dftracer::utils::dataframe::product_simd(*v, out)) {
        if (promoted) dftu_series_free(promoted);
        return out;
    }
    // Floats stay on the materialized column, as reduce does above.
    if (v->encoding != dftracer::utils::dataframe::Encoding::Flat &&
        serial_only_layout(v->type)) {
        dftu_series* flat = dftu_series_materialize(v);
        if (flat != nullptr) {
            out = dftu_series_product(flat);
            dftu_series_free(flat);
            if (promoted) dftu_series_free(promoted);
            return out;
        }
    }
    if (v->is_chunked()) {
        out = product_by_chunk(*v);
        if (promoted) dftu_series_free(promoted);
        return out;
    }
    if (v->encoding != dftracer::utils::dataframe::Encoding::Flat &&
        product_in_blocks(*v, out)) {
        if (promoted) dftu_series_free(promoted);
        return out;
    }
    DFTRACER_UTILS_NUMERIC_DISPATCH(v->type, product_one, *v, out)
    if (promoted) dftu_series_free(promoted);
    return out;
}

namespace {

dftu_scalar bitwise_reduce(const dftu_series& v, int op,
                           dftracer::utils::dstd::StringView name) {
    using dftracer::utils::dataframe::TypeId;
    dftu_scalar out{};
    switch (v.type) {
        case TypeId::Bool:
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
        case TypeId::Uint64:
            break;
        default:
            return refuse(dftracer::utils::dstd::fmt<
                          "{}: the column must be an integer or a bool">(name));
    }
    // A CHUNKED column folds each chunk through this same reduction.
    if (v.is_chunked()) {
        dftu_scalar acc = null_scalar();
        for (const auto& chunk : v.nested) {
            if (!chunk.series) continue;
            const dftu_scalar r = bitwise_reduce(*chunk.series, op, name);
            if (r.kind == DFTU_SCALAR_TAG_ERR) return r;
            acc = combine_bits(acc, r, op);
        }
        return acc;
    }
    // A non-FLAT integer column reduces over gathered blocks; a Bool column
    // folds its gathered bytes in bitwise_bool.
    if (v.encoding != dftracer::utils::dataframe::Encoding::Flat &&
        v.type != TypeId::Bool && bitwise_in_blocks(v, op, out))
        return out;
    switch (v.type) {
        case TypeId::Bool:
            bitwise_bool(v, op, out);
            return out;
        case TypeId::Int8:
            bitwise_one<std::int8_t>(v, op, out);
            return out;
        case TypeId::Int16:
            bitwise_one<std::int16_t>(v, op, out);
            return out;
        case TypeId::Int32:
            bitwise_one<std::int32_t>(v, op, out);
            return out;
        case TypeId::Int64:
            bitwise_one<std::int64_t>(v, op, out);
            return out;
        case TypeId::Uint8:
            bitwise_one<std::uint8_t>(v, op, out);
            return out;
        case TypeId::Uint16:
            bitwise_one<std::uint16_t>(v, op, out);
            return out;
        case TypeId::Uint32:
            bitwise_one<std::uint32_t>(v, op, out);
            return out;
        case TypeId::Uint64:
            bitwise_one<std::uint64_t>(v, op, out);
            return out;
        default:
            break;
    }
    return refuse(dftracer::utils::dstd::fmt<
                  "{}: the column must be an integer or a bool">(name));
}

}  // namespace

dftu_scalar dftu_series_bitwise_and(const dftu_series* v) {
    return bitwise_reduce(*v, 0, "bitwise_and");
}

dftu_scalar dftu_series_bitwise_or(const dftu_series* v) {
    return bitwise_reduce(*v, 1, "bitwise_or");
}

dftu_scalar dftu_series_bitwise_xor(const dftu_series* v) {
    return bitwise_reduce(*v, 2, "bitwise_xor");
}

std::int32_t dftu_series_all(const dftu_series* v) {
    using dftracer::utils::dataframe::TypeId;
    if (v->type != TypeId::Bool) return 1;
    if (v->is_chunked()) {
        for (const auto& chunk : v->nested)
            if (chunk.series && !dftu_series_all(chunk.series.get())) return 0;
        return 1;
    }
    if (v->encoding == dftracer::utils::dataframe::Encoding::Flat)
        return dftracer::utils::dataframe::all_bits(*v) ? 1 : 0;
    std::uint8_t all = 1;
    if (bool_blocks<std::uint8_t>(
            *v, std::uint8_t{1},
            [](std::uint8_t a, bool bit) -> std::uint8_t {
                return bit ? a : 0;
            },
            [](std::uint8_t a, std::uint8_t b) -> std::uint8_t {
                return a & b;
            },
            all))
        return all != 0 ? 1 : 0;
    const dftracer::utils::dataframe::ColumnCells<std::uint8_t> rows(*v);
    for (std::int64_t i = 0; i < v->length; ++i)
        if (rows.valid(i) && !rows.bool_at(i)) return 0;
    return 1;
}

std::int32_t dftu_series_any(const dftu_series* v) {
    using dftracer::utils::dataframe::TypeId;
    if (v->type != TypeId::Bool) return 0;
    if (v->is_chunked()) {
        for (const auto& chunk : v->nested)
            if (chunk.series && dftu_series_any(chunk.series.get())) return 1;
        return 0;
    }
    if (v->encoding == dftracer::utils::dataframe::Encoding::Flat)
        return dftracer::utils::dataframe::any_bits(*v) ? 1 : 0;
    std::uint8_t any = 0;
    if (bool_blocks<std::uint8_t>(
            *v, std::uint8_t{0},
            [](std::uint8_t a, bool bit) -> std::uint8_t {
                return bit ? std::uint8_t{1} : a;
            },
            [](std::uint8_t a, std::uint8_t b) -> std::uint8_t {
                return a | b;
            },
            any))
        return any != 0 ? 1 : 0;
    const dftracer::utils::dataframe::ColumnCells<std::uint8_t> rows(*v);
    for (std::int64_t i = 0; i < v->length; ++i)
        if (rows.valid(i) && rows.bool_at(i)) return 1;
    return 0;
}

std::int64_t dftu_series_arg_min(const dftu_series* v) {
    using dftracer::utils::dataframe::is_arithmetic_type;
    using dftracer::utils::dataframe::is_temporal_type;
    using dftracer::utils::dataframe::physical_type;
    using dftracer::utils::dataframe::promote_for_arithmetic;
    using dftracer::utils::dataframe::type_name;
    if (is_temporal_type(v->type)) {
        std::int64_t idx = -1;
        if (v->is_chunked()) return arg_by_chunk(*v, true);
        DFTRACER_UTILS_NUMERIC_DISPATCH(physical_type(v->type), argextreme_one,
                                        *v, true, idx)
        return idx;
    }
    if (!is_arithmetic_type(v->type)) {
        DFTRACER_UTILS_LOG_ERROR("arg_min: no meaning for type '{}'",
                                 type_name(v->type));
        return -1;
    }
    dftu_series* promoted = nullptr;
    v = promote_for_arithmetic(v, promoted);
    std::int64_t idx = -1;
    if (v->encoding != dftracer::utils::dataframe::Encoding::Flat) {
        // Floats stay on the materialized column, as reduce does.
        if (serial_only_layout(v->type)) {
            dftu_series* flat = dftu_series_materialize(v);
            if (flat != nullptr) {
                idx = dftu_series_arg_min(flat);
                dftu_series_free(flat);
                if (promoted) dftu_series_free(promoted);
                return idx;
            }
        } else if (v->is_chunked()) {
            idx = arg_by_chunk(*v, true);
            if (promoted) dftu_series_free(promoted);
            return idx;
        } else if (arg_in_blocks(*v, true, idx)) {
            if (promoted) dftu_series_free(promoted);
            return idx;
        }
    }
    if (v->encoding != dftracer::utils::dataframe::Encoding::Flat ||
        !dftracer::utils::dataframe::arg_extreme_simd(*v, true, idx))
        DFTRACER_UTILS_NUMERIC_DISPATCH(v->type, argextreme_one, *v, true, idx)
    if (promoted) dftu_series_free(promoted);
    return idx;
}

std::int64_t dftu_series_arg_max(const dftu_series* v) {
    using dftracer::utils::dataframe::is_arithmetic_type;
    using dftracer::utils::dataframe::is_temporal_type;
    using dftracer::utils::dataframe::physical_type;
    using dftracer::utils::dataframe::promote_for_arithmetic;
    using dftracer::utils::dataframe::type_name;
    if (is_temporal_type(v->type)) {
        std::int64_t idx = -1;
        if (v->is_chunked()) return arg_by_chunk(*v, false);
        DFTRACER_UTILS_NUMERIC_DISPATCH(physical_type(v->type), argextreme_one,
                                        *v, false, idx)
        return idx;
    }
    if (!is_arithmetic_type(v->type)) {
        DFTRACER_UTILS_LOG_ERROR("arg_max: no meaning for type '{}'",
                                 type_name(v->type));
        return -1;
    }
    dftu_series* promoted = nullptr;
    v = promote_for_arithmetic(v, promoted);
    std::int64_t idx = -1;
    if (v->encoding != dftracer::utils::dataframe::Encoding::Flat) {
        // Floats stay on the materialized column, as reduce does.
        if (serial_only_layout(v->type)) {
            dftu_series* flat = dftu_series_materialize(v);
            if (flat != nullptr) {
                idx = dftu_series_arg_max(flat);
                dftu_series_free(flat);
                if (promoted) dftu_series_free(promoted);
                return idx;
            }
        } else if (v->is_chunked()) {
            idx = arg_by_chunk(*v, false);
            if (promoted) dftu_series_free(promoted);
            return idx;
        } else if (arg_in_blocks(*v, false, idx)) {
            if (promoted) dftu_series_free(promoted);
            return idx;
        }
    }
    if (v->encoding != dftracer::utils::dataframe::Encoding::Flat ||
        !dftracer::utils::dataframe::arg_extreme_simd(*v, false, idx))
        DFTRACER_UTILS_NUMERIC_DISPATCH(v->type, argextreme_one, *v, false, idx)
    if (promoted) dftu_series_free(promoted);
    return idx;
}

dftu_scalar dftu_series_mode(const dftu_series* v) {
    using dftracer::utils::dataframe::is_arithmetic_type;
    using dftracer::utils::dataframe::is_temporal_type;
    using dftracer::utils::dataframe::physical_type;
    using dftracer::utils::dataframe::promote_for_arithmetic;
    using dftracer::utils::dataframe::type_name;
    dftu_scalar out{};
    out.kind = DFTU_SCALAR_TAG_I64;
    if (is_temporal_type(v->type)) {
        DFTRACER_UTILS_NUMERIC_DISPATCH(physical_type(v->type), mode_one, *v,
                                        out)
        return out;
    }
    if (!is_arithmetic_type(v->type)) {
        DFTRACER_UTILS_LOG_ERROR("mode: no meaning for type '{}'",
                                 type_name(v->type));
        return out;
    }
    dftu_series* promoted = nullptr;
    v = promote_for_arithmetic(v, promoted);
    DFTRACER_UTILS_NUMERIC_DISPATCH(v->type, mode_one, *v, out)
    if (promoted) dftu_series_free(promoted);
    return out;
}
