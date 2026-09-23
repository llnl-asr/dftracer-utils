#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/kernels/kernels.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/dataframe/types.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

// floordiv, mod and pow (Python semantics: floor toward -inf, a remainder with
// the divisor's sign, a zero divisor null), ffill / bfill, and the String ->
// number parse cast. Scalar loops over Int64 or Float64: the operands are
// widened to Int64 when both are integral, else to Float64.

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

bool is_valid(const dftu_series& v, std::int64_t i) {
    if (!v.validity) return true;
    return (v.validity->data()[i >> 3] >> (i & 7)) & 1;
}

dftu_series* alloc_flat(TypeId type, std::int64_t n) {
    auto* out = new dftu_series();
    out->type = type;
    out->encoding = Encoding::Flat;
    out->length = n;
    out->null_count = 0;
    out->data = Buffer::allocate(buffer_bytes(type, n));
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
    std::int64_t r = 1;
    std::int64_t base = a;
    while (b > 0) {
        if (b & 1) r *= base;
        b >>= 1;
        if (b) base *= base;
    }
    out = r;
    return true;
}

// The operands as two flat columns of one type: Int64 when both are integral
// (and the op keeps integers), else Float64. Returns the working type.
TypeId common_type(TypeId a, TypeId b) {
    if (integral(a) && integral(b)) return TypeId::Int64;
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
        auto* p = reinterpret_cast<double*>(out->data->data());
        for (std::int64_t i = 0; i < n; ++i) p[i] = s.value.d;
        return out;
    }
    if (s.kind == DFTU_SCALAR_TAG_I64) {
        dftu_series* out = alloc_flat(TypeId::Int64, n);
        auto* p = reinterpret_cast<std::int64_t*>(out->data->data());
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
    const auto* pa = reinterpret_cast<const T*>(fa.data->data());
    auto* po = reinterpret_cast<T*>(out.data->data());
    std::uint8_t* valid = nullptr;
    if (may_null) {
        out.validity = Buffer::allocate(buffer_bytes(TypeId::Bool, n));
        std::memset(out.validity->data(), 0xff, out.validity->size());
        valid = out.validity->data();
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
                if (may_null && (!is_valid(fa, i) || b_null(i))) {
                    null_at(i);
                    continue;
                }
                const T x = pa[i];
                const T y = bv(i);
                if constexpr (std::is_same_v<T, std::int64_t>) {
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
                            if (y == 0.0)
                                null_at(i);
                            else
                                po[i] = std::floor(x / y);
                            break;
                        case ExtraOp::Mod:
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
    if (nulls == 0) out.validity.reset();
}

dftu_series* extra_binop(const dftu_series* a, const dftu_series* b,
                         ExtraOp op) {
    if (!a || !b || a->length != b->length) return nullptr;
    if (!is_arithmetic_type(a->type) || !is_arithmetic_type(b->type))
        return nullptr;
    const TypeId t = common_type(a->type, b->type);
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
        const auto* pb =
            reinterpret_cast<const std::int64_t*>(fb->data->data());
        run_extra<std::int64_t>(
            *fa, *out, n, op, true, [pb](std::int64_t i) { return pb[i]; },
            [fb](std::int64_t i) { return !is_valid(*fb, i); });
    } else {
        const auto* pb = reinterpret_cast<const double*>(fb->data->data());
        run_extra<double>(
            *fa, *out, n, op, true, [pb](std::int64_t i) { return pb[i]; },
            [fb](std::int64_t i) { return !is_valid(*fb, i); });
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
    const TypeId t = s.kind == DFTU_SCALAR_TAG_I64 && integral(a->type)
                         ? TypeId::Int64
                         : TypeId::Float64;
    dftu_series* fa = to(a, t);
    if (!fa) return nullptr;
    const std::int64_t n = a->length;
    dftu_series* out = alloc_flat(t, n);
    if (reverse) {
        // scalar op column: the column is the divisor, so the broadcast
        // column form applies.
        dftu_series* b = broadcast(s, n);
        dftu_series* r = extra_binop(b, fa, op);
        dftu_series_free(b);
        dftu_series_free(fa);
        dftu_series_free(out);
        return r;
    }
    if (t == TypeId::Int64) {
        const std::int64_t y = s.value.i;
        const bool may_null =
            fa->validity != nullptr || y == 0 || op == ExtraOp::Pow;
        run_extra<std::int64_t>(
            *fa, *out, n, op, may_null, [y](std::int64_t) { return y; },
            [](std::int64_t) { return false; });
    } else {
        const double y = s.kind == DFTU_SCALAR_TAG_F64
                             ? s.value.d
                             : static_cast<double>(s.value.i);
        const bool may_null = fa->validity != nullptr || y == 0.0;
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
    if (in->null_count == 0) return dftu_series_share(in);
    dftu_series* v = dftu_series_materialize(in);
    if (!v) return nullptr;
    const std::int64_t n = v->length;
    std::vector<std::int64_t> idx(static_cast<std::size_t>(n), -1);
    std::int64_t last = -1;
    if (forward) {
        for (std::int64_t i = 0; i < n; ++i) {
            if (is_valid(*v, i)) last = i;
            idx[static_cast<std::size_t>(i)] = last;
        }
    } else {
        for (std::int64_t i = n - 1; i >= 0; --i) {
            if (is_valid(*v, i)) last = i;
            idx[static_cast<std::size_t>(i)] = last;
        }
    }
    dftu_series* out = dftu_series_take(v, idx.data(), n);
    dftu_series_free(v);
    return out;
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

extern "C" {

dftu_series* dftu_series_floordiv(const dftu_series* a, const dftu_series* b) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_floordiv(flat_a, b));
    DFTU_FLAT_OPERAND(b, flat_b, dftu_series_floordiv(a, flat_b));

    return dftracer::utils::dataframe::extra_binop(
        a, b, dftracer::utils::dataframe::ExtraOp::FloorDiv);
}
dftu_series* dftu_series_mod(const dftu_series* a, const dftu_series* b) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_mod(flat_a, b));
    DFTU_FLAT_OPERAND(b, flat_b, dftu_series_mod(a, flat_b));

    return dftracer::utils::dataframe::extra_binop(
        a, b, dftracer::utils::dataframe::ExtraOp::Mod);
}
dftu_series* dftu_series_pow(const dftu_series* a, const dftu_series* b) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_pow(flat_a, b));
    DFTU_FLAT_OPERAND(b, flat_b, dftu_series_pow(a, flat_b));

    return dftracer::utils::dataframe::extra_binop(
        a, b, dftracer::utils::dataframe::ExtraOp::Pow);
}
dftu_series* dftu_series_floordiv_scalar(const dftu_series* a, dftu_scalar s,
                                         int32_t reverse) {
    DFTU_FLAT_OPERAND(a, flat_a,
                      dftu_series_floordiv_scalar(flat_a, s, reverse));

    return dftracer::utils::dataframe::extra_scalar(
        a, s, dftracer::utils::dataframe::ExtraOp::FloorDiv, reverse != 0);
}
dftu_series* dftu_series_mod_scalar(const dftu_series* a, dftu_scalar s,
                                    int32_t reverse) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_mod_scalar(flat_a, s, reverse));

    return dftracer::utils::dataframe::extra_scalar(
        a, s, dftracer::utils::dataframe::ExtraOp::Mod, reverse != 0);
}
dftu_series* dftu_series_pow_scalar(const dftu_series* a, dftu_scalar s,
                                    int32_t reverse) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_pow_scalar(flat_a, s, reverse));

    return dftracer::utils::dataframe::extra_scalar(
        a, s, dftracer::utils::dataframe::ExtraOp::Pow, reverse != 0);
}
dftu_series* dftu_series_ffill(const dftu_series* v) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_ffill(flat_v));

    return dftracer::utils::dataframe::fill_along(v, true);
}
dftu_series* dftu_series_bfill(const dftu_series* v) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_bfill(flat_v));

    return dftracer::utils::dataframe::fill_along(v, false);
}

}  // extern "C"
