// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/column_read.h>
#include <dftracer/utils/dataframe/internal/float_divmod.h>
#include <dftracer/utils/dataframe/kernels/kernels.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/vector.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>

// floordiv, mod and pow (Python semantics: floor toward -inf, a remainder with
// the divisor's sign; a zero divisor is null, except that a float floor
// division by zero gives numpy's and pandas' inf or NaN), ffill / bfill, and
// the String -> number parse cast. Scalar loops over Int64, Uint64 or Float64:
// the operands are widened to Uint64 when one is Uint64 and the other unsigned,
// to Int64 when both are integral, else to Float64, as numpy does (Uint64 with
// a signed integer is Float64).

namespace dftracer::utils::dataframe {

namespace {

enum class ExtraOp { FloorDiv, Mod, Pow };

bool integral(TypeId t) {
    switch (t) {
        case TypeId::Bool:
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
        case TypeId::Uint64:
            return true;
        default:
            return false;
    }
}

dftu_series* alloc_flat(TypeId type, std::int64_t n) {
    auto* out = new dftu_series();
    out->type = type;
    out->encoding = Encoding::Flat;
    out->length = n;
    out->null_count = 0;
    out->data = new_buffer(buffer_bytes(type, n));
    return out;
}

std::int64_t floordiv_i(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

std::int64_t mod_i(std::int64_t a, std::int64_t b) {
    std::int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}

double mod_d(double a, double b) {
    double r = std::fmod(a, b);
    if (r != 0.0 && ((r < 0) != (b < 0))) r += b;
    return r;
}

bool pow_i(std::int64_t a, std::int64_t b, std::int64_t& out) {
    if (b < 0) return false;
    // Unsigned products wrap as numpy's int64 power does, with no signed
    // overflow.
    std::uint64_t r = 1;
    std::uint64_t base = static_cast<std::uint64_t>(a);
    while (b > 0) {
        if (b & 1) r *= base;
        b >>= 1;
        if (b) base *= base;
    }
    out = static_cast<std::int64_t>(r);
    return true;
}

bool is_signed_int(TypeId t) {
    return t == TypeId::Int8 || t == TypeId::Int16 || t == TypeId::Int32 ||
           t == TypeId::Int64;
}

// The working type of two operands: Uint64 for two unsigned integers when
// one is Uint64, Int64 for other integers (when the op keeps integers), else
// Float64.
TypeId common_type(TypeId a, TypeId b, ExtraOp op) {
    if (integral(a) && integral(b)) {
        const bool wide = a == TypeId::Uint64 || b == TypeId::Uint64;
        if (!wide || op == ExtraOp::Pow) return TypeId::Int64;
        return is_signed_int(a) || is_signed_int(b) ? TypeId::Float64
                                                    : TypeId::Uint64;
    }
    return TypeId::Float64;
}

dftu_series* to(const dftu_series* v, TypeId t) {
    if (v->type == t && v->encoding == Encoding::Flat)
        return dftu_series_share(v);
    dftu_series* flat = dftu_series_materialize(v);
    if (!flat) return nullptr;
    if (flat->type == t) return flat;
    dftu_series* out = dftu_series_cast(flat, static_cast<dftu_dtype>(t));
    dftu_series_free(flat);
    return out;
}

// A scalar as a one-type broadcast column, so the scalar forms share the
// column loop.
dftu_series* broadcast(dftu_scalar s, std::int64_t n) {
    if (s.kind == DFTU_SCALAR_TAG_F64) {
        dftu_series* out = alloc_flat(TypeId::Float64, n);
        auto* p = reinterpret_cast<double*>(writable(out->data));
        for (std::int64_t i = 0; i < n; ++i) p[i] = s.value.d;
        return out;
    }
    if (s.kind == DFTU_SCALAR_TAG_I64) {
        dftu_series* out = alloc_flat(TypeId::Int64, n);
        auto* p = reinterpret_cast<std::int64_t*>(writable(out->data));
        for (std::int64_t i = 0; i < n; ++i) p[i] = s.value.i;
        return out;
    }
    return nullptr;
}

// The row loop over Int64 or Float64 cells, `bv(i)` the divisor cell and
// `b_null(i)` its null test (a scalar divisor is a constant); chunked in
// parallel, the validity bitmap allocated up front (chunks are byte
// aligned, so no two write one byte) and dropped again when nothing is
// null.
template <class T, class GetB, class NullB>
void run_extra(const dftu_series& fa, dftu_series& out, std::int64_t n,
               ExtraOp op, bool may_null, GetB bv, NullB b_null) {
    const auto* pa = reinterpret_cast<const T*>(fa.data.data());
    auto* po = reinterpret_cast<T*>(writable(out.data));
    std::uint8_t* valid = nullptr;
    if (may_null) {
        out.validity = new_buffer(buffer_bytes(TypeId::Bool, n));
        std::memset(writable(out.validity), 0xff, out.validity.size());
        valid = writable(out.validity);
    }
    constexpr std::int64_t GRAIN = std::int64_t{1} << 16;
    const std::int64_t nulls = parallel_reduce<std::int64_t>(
        n, GRAIN, 0,
        [&](std::int64_t lo, std::int64_t hi) {
            std::int64_t k = 0;
            auto null_at = [&](std::int64_t i) {
                valid[i >> 3] &= static_cast<std::uint8_t>(~(1u << (i & 7)));
                ++k;
            };
            for (std::int64_t i = lo; i < hi; ++i) {
                po[i] = T{};
                if (may_null && (!own_valid(fa, i) || b_null(i))) {
                    null_at(i);
                    continue;
                }
                const T x = pa[i];
                const T y = bv(i);
                if constexpr (std::is_same_v<T, std::uint64_t>) {
                    // Pow never reaches Uint64 (common_type widens it to
                    // Int64), and a positive divisor floors as it truncates.
                    if (y == 0 || op == ExtraOp::Pow)
                        null_at(i);
                    else
                        po[i] = op == ExtraOp::FloorDiv ? x / y : x % y;
                } else if constexpr (std::is_same_v<T, std::int64_t>) {
                    switch (op) {
                        case ExtraOp::FloorDiv:
                            if (y == 0)
                                null_at(i);
                            else
                                po[i] = floordiv_i(x, y);
                            break;
                        case ExtraOp::Mod:
                            if (y == 0)
                                null_at(i);
                            else
                                po[i] = mod_i(x, y);
                            break;
                        case ExtraOp::Pow:
                            if (!pow_i(x, y, po[i])) null_at(i);
                            break;
                    }
                } else {
                    switch (op) {
                        case ExtraOp::FloorDiv:
                            po[i] = y == 0.0 ? x / y : float_floordiv(x, y);
                            break;
                        case ExtraOp::Mod:
                            // pandas' NaN here is its missing value.
                            if (y == 0.0)
                                null_at(i);
                            else
                                po[i] = mod_d(x, y);
                            break;
                        case ExtraOp::Pow:
                            po[i] = std::pow(x, y);
                            break;
                    }
                }
            }
            return k;
        },
        [](std::int64_t x, std::int64_t y) { return x + y; });
    out.null_count = nulls;
    if (nulls == 0) out.validity = {};
}

dftu_series* extra_binop(const dftu_series* a, const dftu_series* b,
                         ExtraOp op) {
    if (!a || !b || a->length != b->length) return nullptr;
    if (!is_arithmetic_type(a->type) || !is_arithmetic_type(b->type))
        return nullptr;
    const TypeId t = common_type(a->type, b->type, op);
    dftu_series* fa = to(a, t);
    dftu_series* fb = to(b, t);
    if (!fa || !fb) {
        if (fa) dftu_series_free(fa);
        if (fb) dftu_series_free(fb);
        return nullptr;
    }
    const std::int64_t n = a->length;
    dftu_series* out = alloc_flat(t, n);
    // A divisor column may hold a zero anywhere, so the bitmap is always
    // needed here (pow over Int64 may overflow too).
    if (t == TypeId::Int64) {
        const auto* pb = reinterpret_cast<const std::int64_t*>(fb->data.data());
        run_extra<std::int64_t>(
            *fa, *out, n, op, true, [pb](std::int64_t i) { return pb[i]; },
            [fb](std::int64_t i) { return !own_valid(*fb, i); });
    } else if (t == TypeId::Uint64) {
        const auto* pb =
            reinterpret_cast<const std::uint64_t*>(fb->data.data());
        run_extra<std::uint64_t>(
            *fa, *out, n, op, true, [pb](std::int64_t i) { return pb[i]; },
            [fb](std::int64_t i) { return !own_valid(*fb, i); });
    } else {
        const auto* pb = reinterpret_cast<const double*>(fb->data.data());
        run_extra<double>(
            *fa, *out, n, op, true, [pb](std::int64_t i) { return pb[i]; },
            [fb](std::int64_t i) { return !own_valid(*fb, i); });
    }
    dftu_series_free(fa);
    dftu_series_free(fb);
    return out;
}

// The scalar forms: the divisor is a constant, so the loop needs no
// broadcast column and, with a non-zero divisor over a column without
// nulls, no bitmap.
dftu_series* extra_scalar(const dftu_series* a, dftu_scalar s, ExtraOp op,
                          bool reverse) {
    if (!a || !is_arithmetic_type(a->type)) return nullptr;
    if (s.kind != DFTU_SCALAR_TAG_F64 && s.kind != DFTU_SCALAR_TAG_I64)
        return nullptr;
    const bool int_scalar = s.kind == DFTU_SCALAR_TAG_I64 && integral(a->type);
    // A Uint64 column keeps its type over a non-negative integer, as numpy.
    const bool wide =
        int_scalar && a->type == TypeId::Uint64 && op != ExtraOp::Pow;
    const TypeId t = !int_scalar      ? TypeId::Float64
                     : !wide          ? TypeId::Int64
                     : s.value.i >= 0 ? TypeId::Uint64
                                      : TypeId::Float64;
    dftu_series* fa = to(a, t);
    if (!fa) return nullptr;
    const std::int64_t n = a->length;
    dftu_series* out = alloc_flat(t, n);
    if (reverse) {
        // scalar op column: the column is the divisor, so the broadcast
        // column form applies.
        dftu_series* b = broadcast(s, n);
        if (b && t == TypeId::Uint64) {
            dftu_series* wide_b = to(b, t);
            dftu_series_free(b);
            b = wide_b;
        }
        dftu_series* r = b ? extra_binop(b, fa, op) : nullptr;
        dftu_series_free(b);
        dftu_series_free(fa);
        dftu_series_free(out);
        return r;
    }
    if (t == TypeId::Uint64) {
        const auto y = static_cast<std::uint64_t>(s.value.i);
        const bool may_null = static_cast<bool>(fa->validity.block()) || y == 0;
        run_extra<std::uint64_t>(
            *fa, *out, n, op, may_null, [y](std::int64_t) { return y; },
            [](std::int64_t) { return false; });
    } else if (t == TypeId::Int64) {
        const std::int64_t y = s.value.i;
        const bool may_null = static_cast<bool>(fa->validity.block()) ||
                              y == 0 || op == ExtraOp::Pow;
        run_extra<std::int64_t>(
            *fa, *out, n, op, may_null, [y](std::int64_t) { return y; },
            [](std::int64_t) { return false; });
    } else {
        const double y = s.kind == DFTU_SCALAR_TAG_F64
                             ? s.value.d
                             : static_cast<double>(s.value.i);
        const bool may_null = static_cast<bool>(fa->validity.block()) ||
                              (y == 0.0 && op == ExtraOp::Mod);
        run_extra<double>(
            *fa, *out, n, op, may_null, [y](std::int64_t) { return y; },
            [](std::int64_t) { return false; });
    }
    dftu_series_free(fa);
    return out;
}

// The row each position takes its value from under a forward (or backward)
// fill: itself when present, else the nearest present row before (after) it,
// -1 when there is none (take keeps that null).
dftu_series* fill_along(const dftu_series* in, bool forward) {
    if (!in) return nullptr;
    if (dftracer::utils::dataframe::cell_null_count(*in) == 0)
        return dftu_series_share(in);
    const std::int64_t n = in->length;
    dstd::Vector<std::int64_t> idx(static_cast<std::size_t>(n), -1);
    std::int64_t last = -1;
    for (std::int64_t k = 0; k < n; ++k) {
        const std::int64_t i = forward ? k : n - 1 - k;
        if (!dftracer::utils::dataframe::cell_is_null(in, i)) last = i;
        idx[static_cast<std::size_t>(i)] = last;
    }
    return dftu_series_take(in, idx.data(), n);
}

}  // namespace

Series floordiv(const Series& a, const Series& b) {
    return Series{dftu_series_floordiv(a.handle(), b.handle())};
}
Series mod(const Series& a, const Series& b) {
    return Series{dftu_series_mod(a.handle(), b.handle())};
}
Series pow(const Series& a, const Series& b) {
    return Series{dftu_series_pow(a.handle(), b.handle())};
}
Series ffill(const Series& v) { return Series{dftu_series_ffill(v.handle())}; }
Series bfill(const Series& v) { return Series{dftu_series_bfill(v.handle())}; }

}  // namespace dftracer::utils::dataframe

#define DFTRACER_UTILS_EXTRA_COLUMN_ENTRY(name, op)                  \
    dftu_series* dftu_series_##name(const dftu_series* a,            \
                                    const dftu_series* b) {          \
        DFTU_FLAT_OPERAND(a, flat_a, dftu_series_##name(flat_a, b)); \
        DFTU_FLAT_OPERAND(b, flat_b, dftu_series_##name(a, flat_b)); \
        return dftracer::utils::dataframe::extra_binop(              \
            a, b, dftracer::utils::dataframe::ExtraOp::op);          \
    }
#define DFTRACER_UTILS_EXTRA_SCALAR_ENTRY(name, op)                         \
    dftu_series* dftu_series_##name##_scalar(                               \
        const dftu_series* a, dftu_scalar s, std::int32_t reverse) {        \
        DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_##name##_scalar, s,      \
                                    reverse);                               \
        DFTU_FLAT_OPERAND(a, flat_a,                                        \
                          dftu_series_##name##_scalar(flat_a, s, reverse)); \
        return dftracer::utils::dataframe::extra_scalar(                    \
            a, s, dftracer::utils::dataframe::ExtraOp::op, reverse != 0);   \
    }

DFTRACER_UTILS_EXTRA_COLUMN_ENTRY(floordiv, FloorDiv)
DFTRACER_UTILS_EXTRA_COLUMN_ENTRY(mod, Mod)
DFTRACER_UTILS_EXTRA_COLUMN_ENTRY(pow, Pow)
DFTRACER_UTILS_EXTRA_SCALAR_ENTRY(floordiv, FloorDiv)
DFTRACER_UTILS_EXTRA_SCALAR_ENTRY(mod, Mod)
DFTRACER_UTILS_EXTRA_SCALAR_ENTRY(pow, Pow)

#undef DFTRACER_UTILS_EXTRA_COLUMN_ENTRY
#undef DFTRACER_UTILS_EXTRA_SCALAR_ENTRY

dftu_series* dftu_series_ffill(const dftu_series* v) {
    return dftracer::utils::dataframe::fill_along(v, true);
}
dftu_series* dftu_series_bfill(const dftu_series* v) {
    return dftracer::utils::dataframe::fill_along(v, false);
}
