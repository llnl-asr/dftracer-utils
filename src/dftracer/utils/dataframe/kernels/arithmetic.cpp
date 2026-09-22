#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/decimal_arith.h>
#include <dftracer/utils/dataframe/internal/numeric_dispatch.h>
#include <dftracer/utils/dataframe/internal/scalar.h>
#include <dftracer/utils/dataframe/internal/temporal_arith.h>
#include <dftracer/utils/dataframe/internal/type_promotion.h>
#include <dftracer/utils/dataframe/kernels/arithmetic.h>
#include <dftracer/utils/dataframe/parallel.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <vector>

// Highway runtime dispatch: foreach_target.h recompiles this TU once per ISA,
// then HWY_DYNAMIC_DISPATCH selects the best at load time.
#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "dftracer/utils/dataframe/kernels/arithmetic.cpp"
#include <hwy/foreach_target.h>  // must precede highway.h
#include <hwy/highway.h>

HWY_BEFORE_NAMESPACE();
namespace dftracer::utils::dataframe {
namespace HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

#define DF_DEFINE_BINOP(NAME, HWYOP, SCALAROP)                             \
    template <class T>                                                     \
    void NAME(const void* av, const void* bv, void* ov, std::size_t n) {   \
        const T* a = static_cast<const T*>(av);                            \
        const T* b = static_cast<const T*>(bv);                            \
        T* out = static_cast<T*>(ov);                                      \
        const hn::ScalableTag<T> d;                                        \
        const std::size_t lanes = hn::Lanes(d);                            \
        std::size_t i = 0;                                                 \
        for (; i + lanes <= n; i += lanes)                                 \
            hn::StoreU(HWYOP(hn::LoadU(d, a + i), hn::LoadU(d, b + i)), d, \
                       out + i);                                           \
        for (; i < n; ++i) out[i] = SCALAROP;                              \
    }

DF_DEFINE_BINOP(AddImpl, hn::Add, a[i] + b[i])
DF_DEFINE_BINOP(SubImpl, hn::Sub, a[i] - b[i])
DF_DEFINE_BINOP(MulImpl, hn::Mul, a[i] * b[i])

#undef DF_DEFINE_BINOP

// Division: floats use the SIMD hn::Div; integers have no SIMD divide, so a
// scalar loop with a divide-by-zero guard (result 0). The DSL promotes to
// double before dividing, so the integer path is a defensive fallback.
template <class T>
void DivImpl(const void* av, const void* bv, void* ov, std::size_t n) {
    const T* a = static_cast<const T*>(av);
    const T* b = static_cast<const T*>(bv);
    T* out = static_cast<T*>(ov);
    if constexpr (std::is_floating_point_v<T>) {
        const hn::ScalableTag<T> d;
        const std::size_t lanes = hn::Lanes(d);
        std::size_t i = 0;
        for (; i + lanes <= n; i += lanes)
            hn::StoreU(hn::Div(hn::LoadU(d, a + i), hn::LoadU(d, b + i)), d,
                       out + i);
        for (; i < n; ++i) out[i] = a[i] / b[i];
    } else {
        for (std::size_t i = 0; i < n; ++i)
            out[i] = b[i] != 0 ? static_cast<T>(a[i] / b[i]) : T{0};
    }
}

// Broadcast a scalar across the column. The scalar is converted to the column
// type T, so every numeric type is represented exactly (no double round-trip).
#define DF_DEFINE_SCALAR(NAME, HWYOP, SCALAROP)                          \
    template <class T>                                                   \
    void NAME(const void* av, dftu_scalar sc, void* ov, std::size_t n) { \
        const T* a = static_cast<const T*>(av);                          \
        T* out = static_cast<T*>(ov);                                    \
        const T s = scalar_as<T>(sc);                                    \
        const hn::ScalableTag<T> d;                                      \
        const auto vs = hn::Set(d, s);                                   \
        const std::size_t lanes = hn::Lanes(d);                          \
        std::size_t i = 0;                                               \
        for (; i + lanes <= n; i += lanes)                               \
            hn::StoreU(HWYOP(hn::LoadU(d, a + i), vs), d, out + i);      \
        for (; i < n; ++i) out[i] = SCALAROP;                            \
    }

DF_DEFINE_SCALAR(AddSImpl, hn::Add, a[i] + s)
DF_DEFINE_SCALAR(SubSImpl, hn::Sub, a[i] - s)
DF_DEFINE_SCALAR(MulSImpl, hn::Mul, a[i] * s)

#undef DF_DEFINE_SCALAR

// Broadcast division: floats SIMD, integers scalar with a zero guard.
template <class T>
void DivSImpl(const void* av, dftu_scalar sc, void* ov, std::size_t n) {
    const T* a = static_cast<const T*>(av);
    T* out = static_cast<T*>(ov);
    const T s = scalar_as<T>(sc);
    if constexpr (std::is_floating_point_v<T>) {
        const hn::ScalableTag<T> d;
        const auto vs = hn::Set(d, s);
        const std::size_t lanes = hn::Lanes(d);
        std::size_t i = 0;
        for (; i + lanes <= n; i += lanes)
            hn::StoreU(hn::Div(hn::LoadU(d, a + i), vs), d, out + i);
        for (; i < n; ++i) out[i] = a[i] / s;
    } else {
        for (std::size_t i = 0; i < n; ++i)
            out[i] = s != 0 ? static_cast<T>(a[i] / s) : T{0};
    }
}

// EXPERIMENT (op 2 measurement, see benchmarks/dataframe_parallel_bench.cpp):
// fan the add out across row ranges via parallel_for to measure whether a
// bandwidth-bound elementwise op is worth parallelizing on this machine.
constexpr std::size_t ADD_PARALLEL_GRAIN = 1 << 20;
template <class T>
void AddParallelImpl(const void* av, const void* bv, void* ov, std::size_t n) {
    if (!parallel_backend_installed() || n < ADD_PARALLEL_GRAIN) {
        AddImpl<T>(av, bv, ov, n);
        return;
    }
    const T* a = static_cast<const T*>(av);
    const T* b = static_cast<const T*>(bv);
    T* out = static_cast<T*>(ov);
    parallel_for(static_cast<std::int64_t>(n),
                 static_cast<std::int64_t>(ADD_PARALLEL_GRAIN),
                 [&](std::int64_t beg, std::int64_t end) {
                     AddImpl<T>(a + beg, b + beg, out + beg,
                                static_cast<std::size_t>(end - beg));
                 });
}

void AddKernel(std::int32_t type, const void* a, const void* b, void* out,
               std::size_t n) {
    DF_NUMERIC_DISPATCH(static_cast<TypeId>(type), AddParallelImpl, a, b, out,
                        n)
}
void SubKernel(std::int32_t type, const void* a, const void* b, void* out,
               std::size_t n) {
    DF_NUMERIC_DISPATCH(static_cast<TypeId>(type), SubImpl, a, b, out, n)
}
void MulKernel(std::int32_t type, const void* a, const void* b, void* out,
               std::size_t n) {
    DF_NUMERIC_DISPATCH(static_cast<TypeId>(type), MulImpl, a, b, out, n)
}
void AddScalarKernel(std::int32_t type, const void* a, dftu_scalar s, void* out,
                     std::size_t n) {
    DF_NUMERIC_DISPATCH(static_cast<TypeId>(type), AddSImpl, a, s, out, n)
}
void SubScalarKernel(std::int32_t type, const void* a, dftu_scalar s, void* out,
                     std::size_t n) {
    DF_NUMERIC_DISPATCH(static_cast<TypeId>(type), SubSImpl, a, s, out, n)
}
void MulScalarKernel(std::int32_t type, const void* a, dftu_scalar s, void* out,
                     std::size_t n) {
    DF_NUMERIC_DISPATCH(static_cast<TypeId>(type), MulSImpl, a, s, out, n)
}
void DivKernel(std::int32_t type, const void* a, const void* b, void* out,
               std::size_t n) {
    DF_NUMERIC_DISPATCH(static_cast<TypeId>(type), DivImpl, a, b, out, n)
}
void DivScalarKernel(std::int32_t type, const void* a, dftu_scalar s, void* out,
                     std::size_t n) {
    DF_NUMERIC_DISPATCH(static_cast<TypeId>(type), DivSImpl, a, s, out, n)
}

}  // namespace HWY_NAMESPACE
}  // namespace dftracer::utils::dataframe
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace dftracer::utils::dataframe {

HWY_EXPORT(AddKernel);
HWY_EXPORT(SubKernel);
HWY_EXPORT(MulKernel);
HWY_EXPORT(AddScalarKernel);
HWY_EXPORT(SubScalarKernel);
HWY_EXPORT(MulScalarKernel);
HWY_EXPORT(DivKernel);
HWY_EXPORT(DivScalarKernel);

namespace {

using BinOp = ArithOp;

bool is_numeric(TypeId t) { return is_arithmetic_type(t); }
bool is_decimal_type(TypeId t) {
    return t == TypeId::Decimal128 || t == TypeId::Decimal256;
}

bool is_float_type(TypeId t) {
    return t == TypeId::Float32 || t == TypeId::Float64;
}
bool is_signed_int_type(TypeId t) {
    return t == TypeId::Int8 || t == TypeId::Int16 || t == TypeId::Int32 ||
           t == TypeId::Int64;
}
bool is_unsigned_int_type(TypeId t) {
    return t == TypeId::Uint8 || t == TypeId::Uint16 || t == TypeId::Uint32 ||
           t == TypeId::Uint64;
}

// numpy/pandas array-array result_type for a numeric binary op: widen within
// the same signedness, promote a float/int mix to Float64, and promote a
// signed/unsigned mix to Float64 unless the signed side is strictly wider (so
// it already covers the unsigned side's range).
TypeId promote_common(TypeId a, TypeId b) {
    if (a == b) return a;
    if (is_float_type(a) || is_float_type(b)) return TypeId::Float64;
    const std::size_t wa = byte_width(a).value_or(0);
    const std::size_t wb = byte_width(b).value_or(0);
    if (is_signed_int_type(a) && is_signed_int_type(b)) return wa >= wb ? a : b;
    if (is_unsigned_int_type(a) && is_unsigned_int_type(b))
        return wa >= wb ? a : b;
    TypeId signed_t = is_signed_int_type(a) ? a : b;
    TypeId unsigned_t = is_unsigned_int_type(a) ? a : b;
    return byte_width(signed_t).value_or(0) > byte_width(unsigned_t).value_or(0)
               ? signed_t
               : TypeId::Float64;
}

// See temporal_scalarop_result (temporal_arith.h) for the rule.
dftu_series* temporal_scalar_op(const dftu_series* a, dftu_scalar s, BinOp op) {
    auto res = temporal_scalarop_result(a->type, a->time_unit, op);
    if (!res) {
        DFTRACER_UTILS_LOG_ERROR(
            "arithmetic: scalar op not defined for temporal type '%s'",
            type_name(a->type));
        return nullptr;
    }
    auto* out = new dftu_series();
    out->type = res->id;
    out->encoding = Encoding::Flat;
    out->length = a->length;
    out->null_count = a->null_count;
    out->validity = a->validity;
    out->time_unit = res->unit;
    out->data = Buffer::allocate(static_cast<std::size_t>(a->length) *
                                 byte_width(TypeId::Int64).value_or(0));
    const std::int32_t t = static_cast<std::int32_t>(TypeId::Int64);
    const void* pa = a->data->data();
    void* po = out->data->data();
    const std::size_t n = static_cast<std::size_t>(a->length);
    if (op == BinOp::Mul)
        HWY_DYNAMIC_DISPATCH(MulScalarKernel)(t, pa, s, po, n);
    else
        HWY_DYNAMIC_DISPATCH(DivScalarKernel)(t, pa, s, po, n);
    return out;
}

// Decimal-by-scalar deliberately stays on the lossy Float64 path below: a
// bare scalar has no scale of its own to compute an exact target scale from.
dftu_series* scalar_op(const dftu_series* a, dftu_scalar s, BinOp op) {
    if (a->encoding != Encoding::Flat) return nullptr;
    if (is_temporal_type(a->type)) return temporal_scalar_op(a, s, op);
    if (!is_numeric(a->type)) return nullptr;

    // Float16 has no arithmetic kernel; Decimal128/256 have no exact one.
    // Both promote here, once, before any dispatch below sees them.
    dftu_series* promoted = nullptr;
    a = promote_for_arithmetic(a, promoted);

    dftu_series* casted = nullptr;
    const dftu_series* src = a;
    if (s.kind == DFTU_SCALAR_TAG_F64 && !is_float_type(a->type)) {
        casted = dftu_series_cast(a, static_cast<dftu_dtype>(TypeId::Float64));
        if (!casted) return nullptr;
        src = casted;
    }

    auto* out = new dftu_series();
    out->type = src->type;
    out->encoding = Encoding::Flat;
    out->length = src->length;
    out->null_count = src->null_count;
    out->validity = src->validity;
    out->data = Buffer::allocate(static_cast<std::size_t>(src->length) *
                                 byte_width(src->type).value_or(0));

    std::int32_t t = static_cast<std::int32_t>(src->type);
    const void* pa = src->data->data();
    void* po = out->data->data();
    std::size_t n = static_cast<std::size_t>(src->length);
    switch (op) {
        case BinOp::Add:
            HWY_DYNAMIC_DISPATCH(AddScalarKernel)(t, pa, s, po, n);
            break;
        case BinOp::Sub:
            HWY_DYNAMIC_DISPATCH(SubScalarKernel)(t, pa, s, po, n);
            break;
        case BinOp::Mul:
            HWY_DYNAMIC_DISPATCH(MulScalarKernel)(t, pa, s, po, n);
            break;
        case BinOp::Div:
            HWY_DYNAMIC_DISPATCH(DivScalarKernel)(t, pa, s, po, n);
            break;
    }
    if (casted) dftu_series_free(casted);
    if (promoted) dftu_series_free(promoted);
    return out;
}

// A column-column result is null where either input is: validity is the AND
// of the two bitmaps (a missing bitmap means all valid), null_count recounted.
void and_validity(const dftu_series* a, const dftu_series* b,
                  dftu_series* out) {
    if (!a->validity && !b->validity) return;
    const std::size_t bytes = buffer_bytes(TypeId::Bool, out->length);
    auto v = Buffer::allocate(bytes);
    std::uint8_t* dst = v->data();
    const std::uint8_t* va = a->validity ? a->validity->data() : nullptr;
    const std::uint8_t* vb = b->validity ? b->validity->data() : nullptr;
    for (std::size_t i = 0; i < bytes; ++i)
        dst[i] = static_cast<std::uint8_t>((va ? va[i] : 0xFF) &
                                           (vb ? vb[i] : 0xFF));
    std::int64_t nulls = 0;
    for (std::int64_t i = 0; i < out->length; ++i)
        if (!((dst[i >> 3] >> (i & 7)) & 1)) ++nulls;
    out->validity = std::move(v);
    out->null_count = nulls;
}

// See temporal_binop_result (temporal_arith.h) for the rule table.
dftu_series* temporal_binop(const dftu_series* a, const dftu_series* b,
                            BinOp op) {
    auto res = temporal_binop_result(a->type, a->time_unit, a->timezone,
                                     b->type, b->time_unit, b->timezone, op);
    if (!res) {
        DFTRACER_UTILS_LOG_ERROR(
            "arithmetic: op not defined for temporal types '%s' and '%s'",
            type_name(a->type), type_name(b->type));
        return nullptr;
    }

    std::vector<std::int64_t> tmp_a, tmp_b;
    const auto* pa = static_cast<const std::int64_t*>(
        static_cast<const void*>(a->data->data()));
    const auto* pb = static_cast<const std::int64_t*>(
        static_cast<const void*>(b->data->data()));
    const std::size_t n = static_cast<std::size_t>(a->length);
    if (a->time_unit != res->unit) {
        auto rescaled = rescale_ticks_up(pa, n, a->time_unit, res->unit);
        if (!rescaled) {
            DFTRACER_UTILS_LOG_ERROR(
                "arithmetic: temporal rescale overflowed int64");
            return nullptr;
        }
        tmp_a = std::move(*rescaled);
        pa = tmp_a.data();
    }
    if (b->time_unit != res->unit) {
        auto rescaled = rescale_ticks_up(pb, n, b->time_unit, res->unit);
        if (!rescaled) {
            DFTRACER_UTILS_LOG_ERROR(
                "arithmetic: temporal rescale overflowed int64");
            return nullptr;
        }
        tmp_b = std::move(*rescaled);
        pb = tmp_b.data();
    }

    auto* out = new dftu_series();
    out->type = res->id;
    out->encoding = Encoding::Flat;
    out->length = a->length;
    out->time_unit = res->unit;
    out->timezone = res->timezone;
    out->data = Buffer::allocate(n * byte_width(TypeId::Int64).value_or(0));
    const std::int32_t t = static_cast<std::int32_t>(TypeId::Int64);
    void* po = out->data->data();
    if (op == BinOp::Add)
        HWY_DYNAMIC_DISPATCH(AddKernel)(t, pa, pb, po, n);
    else
        HWY_DYNAMIC_DISPATCH(SubKernel)(t, pa, pb, po, n);
    and_validity(a, b, out);
    return out;
}

// See decimal_arith.h for the scale/precision rules.
dftu_series* decimal_binop_exact(const dftu_series* a, const dftu_series* b,
                                 BinOp op) {
    const bool is256 = a->type == TypeId::Decimal256;
    const std::int32_t max_prec =
        is256 ? DECIMAL256_MAX_PRECISION : DECIMAL128_MAX_PRECISION;

    std::int32_t result_scale = 0;
    std::int32_t result_precision = 0;
    switch (op) {
        case BinOp::Add:
        case BinOp::Sub:
            result_scale = std::max(a->decimal_scale, b->decimal_scale);
            result_precision = std::min(
                max_prec, std::max(a->decimal_precision - a->decimal_scale,
                                   b->decimal_precision - b->decimal_scale) +
                              result_scale + 1);
            break;
        case BinOp::Mul:
            result_scale = a->decimal_scale + b->decimal_scale;
            result_precision = std::min(
                max_prec, a->decimal_precision + b->decimal_precision + 1);
            break;
        case BinOp::Div:
            result_scale = a->decimal_scale + DECIMAL_DIVIDE_SCALE_INCREMENT;
            result_precision =
                std::min(max_prec, a->decimal_precision + b->decimal_scale +
                                       DECIMAL_DIVIDE_SCALE_INCREMENT);
            break;
    }
    if (result_scale > max_prec || result_precision <= 0) {
        DFTRACER_UTILS_LOG_ERROR(
            "arithmetic: decimal result scale exceeds declared precision");
        return nullptr;
    }

    auto* out = new dftu_series();
    out->type = a->type;
    out->encoding = Encoding::Flat;
    out->length = a->length;
    out->decimal_scale = result_scale;
    out->decimal_precision = result_precision;
    const std::size_t width = is256 ? 32 : 16;
    const std::int64_t n = a->length;
    out->data = Buffer::allocate(static_cast<std::size_t>(n) * width);
    const auto* pa = static_cast<const std::uint8_t*>(a->data->data());
    const auto* pb = static_cast<const std::uint8_t*>(b->data->data());
    auto* po = static_cast<std::uint8_t*>(out->data->data());

    bool ok = true;
    if (!is256) {
        for (std::int64_t i = 0; i < n && ok; ++i) {
            const i128 va = load_i128(pa + static_cast<std::size_t>(i) * 16);
            const i128 vb = load_i128(pb + static_cast<std::size_t>(i) * 16);
            std::optional<i128> r;
            if (op == BinOp::Add || op == BinOp::Sub) {
                auto ra = rescale_up_i128(va, a->decimal_scale, result_scale);
                auto rb = rescale_up_i128(vb, b->decimal_scale, result_scale);
                if (ra && rb) {
                    const i128 rhs = op == BinOp::Add ? *rb : -*rb;
                    const i128 sum = *ra + rhs;
                    const bool overflow = ((*ra ^ sum) & (rhs ^ sum)) < 0;
                    if (!overflow && i128_fits_precision(sum, result_precision))
                        r = sum;
                }
            } else if (op == BinOp::Mul) {
                r = mul_checked_i128(va, vb, result_precision);
            } else {
                r = muldiv_checked_i128(
                    va, b->decimal_scale + DECIMAL_DIVIDE_SCALE_INCREMENT, vb,
                    result_precision);
            }
            if (!r) {
                ok = false;
                break;
            }
            store_i128(po + static_cast<std::size_t>(i) * 16, *r);
        }
    } else {
        // Decimal256 exact multiply/divide need a 512-bit intermediate this
        // pass does not build; decimal_binop_exact is only called for those
        // ops when is256 is false (see binop below), so only Add/Sub reach
        // here.
        for (std::int64_t i = 0; i < n && ok; ++i) {
            const Limbs256 va =
                load_limbs256(pa + static_cast<std::size_t>(i) * 32);
            const Limbs256 vb =
                load_limbs256(pb + static_cast<std::size_t>(i) * 32);
            auto ra = rescale_up_256(va, a->decimal_scale, result_scale);
            auto rb = rescale_up_256(vb, b->decimal_scale, result_scale);
            std::optional<Limbs256> r;
            if (ra && rb)
                r = addsub_checked_256(*ra, *rb, op == BinOp::Sub,
                                       result_precision);
            if (!r) {
                ok = false;
                break;
            }
            store_limbs256(po + static_cast<std::size_t>(i) * 32, *r);
        }
    }

    if (!ok) {
        DFTRACER_UTILS_LOG_ERROR(
            "arithmetic: decimal op overflowed its declared precision");
        delete out;
        return nullptr;
    }
    and_validity(a, b, out);
    return out;
}

dftu_series* binop(const dftu_series* a, const dftu_series* b, BinOp op) {
    if (a->encoding != Encoding::Flat || b->encoding != Encoding::Flat)
        return nullptr;
    if (a->length != b->length) return nullptr;
    if (is_temporal_type(a->type) || is_temporal_type(b->type))
        return temporal_binop(a, b, op);

    if (is_decimal_type(a->type) && a->type == b->type) {
        // Decimal256 has no exact multiply/divide here (see
        // decimal_binop_exact); those combos fall through to the documented
        // lossy Float64 path below instead of refusing outright.
        const bool exact_supported = a->type == TypeId::Decimal128 ||
                                     op == BinOp::Add || op == BinOp::Sub;
        if (exact_supported) return decimal_binop_exact(a, b, op);
    }

    if (!is_numeric(a->type) || !is_numeric(b->type)) return nullptr;

    const dftu_series* orig_a = a;
    const dftu_series* orig_b = b;
    dftu_series* promoted_a = nullptr;
    dftu_series* promoted_b = nullptr;
    a = promote_for_arithmetic(a, promoted_a);
    b = promote_for_arithmetic(b, promoted_b);

    dftu_series* casted_a = nullptr;
    dftu_series* casted_b = nullptr;
    const dftu_series* pa_src = a;
    const dftu_series* pb_src = b;
    if (a->type != b->type) {
        TypeId common = promote_common(a->type, b->type);
        if (a->type != common) {
            casted_a = dftu_series_cast(a, static_cast<dftu_dtype>(common));
            if (!casted_a) return nullptr;
            pa_src = casted_a;
        }
        if (b->type != common) {
            casted_b = dftu_series_cast(b, static_cast<dftu_dtype>(common));
            if (!casted_b) {
                if (casted_a) dftu_series_free(casted_a);
                return nullptr;
            }
            pb_src = casted_b;
        }
    }

    auto* out = new dftu_series();
    out->type = pa_src->type;
    out->encoding = Encoding::Flat;
    out->length = pa_src->length;
    out->data = Buffer::allocate(static_cast<std::size_t>(pa_src->length) *
                                 byte_width(pa_src->type).value_or(0));

    std::int32_t t = static_cast<std::int32_t>(pa_src->type);
    const void* pa = pa_src->data->data();
    const void* pb = pb_src->data->data();
    void* po = out->data->data();
    std::size_t n = static_cast<std::size_t>(pa_src->length);
    switch (op) {
        case BinOp::Add:
            HWY_DYNAMIC_DISPATCH(AddKernel)(t, pa, pb, po, n);
            break;
        case BinOp::Sub:
            HWY_DYNAMIC_DISPATCH(SubKernel)(t, pa, pb, po, n);
            break;
        case BinOp::Mul:
            HWY_DYNAMIC_DISPATCH(MulKernel)(t, pa, pb, po, n);
            break;
        case BinOp::Div:
            HWY_DYNAMIC_DISPATCH(DivKernel)(t, pa, pb, po, n);
            break;
    }
    and_validity(orig_a, orig_b, out);
    if (casted_a) dftu_series_free(casted_a);
    if (casted_b) dftu_series_free(casted_b);
    if (promoted_a) dftu_series_free(promoted_a);
    if (promoted_b) dftu_series_free(promoted_b);
    return out;
}

}  // namespace

Series add(const Series& a, const Series& b) {
    return Series{dftu_series_add(a.handle(), b.handle())};
}
Series sub(const Series& a, const Series& b) {
    return Series{dftu_series_sub(a.handle(), b.handle())};
}
Series mul(const Series& a, const Series& b) {
    return Series{dftu_series_mul(a.handle(), b.handle())};
}
Series div(const Series& a, const Series& b) {
    return Series{dftu_series_div(a.handle(), b.handle())};
}
dftu_series* add_columns(const dftu_series* a, const dftu_series* b) {
    return binop(a, b, BinOp::Add);
}
dftu_series* sub_columns(const dftu_series* a, const dftu_series* b) {
    return binop(a, b, BinOp::Sub);
}
dftu_series* mul_columns(const dftu_series* a, const dftu_series* b) {
    return binop(a, b, BinOp::Mul);
}
dftu_series* div_columns(const dftu_series* a, const dftu_series* b) {
    return binop(a, b, BinOp::Div);
}
dftu_series* add_scalar_col(const dftu_series* a, dftu_scalar s) {
    return scalar_op(a, s, BinOp::Add);
}
dftu_series* sub_scalar_col(const dftu_series* a, dftu_scalar s) {
    return scalar_op(a, s, BinOp::Sub);
}
dftu_series* mul_scalar_col(const dftu_series* a, dftu_scalar s) {
    return scalar_op(a, s, BinOp::Mul);
}
dftu_series* div_scalar_col(const dftu_series* a, dftu_scalar s) {
    return scalar_op(a, s, BinOp::Div);
}

}  // namespace dftracer::utils::dataframe

dftu_series* dftu_series_add(const dftu_series* a, const dftu_series* b) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_add(flat_a, b));
    DFTU_FLAT_OPERAND(b, flat_b, dftu_series_add(a, flat_b));

    return dftracer::utils::dataframe::add_columns(a, b);
}
dftu_series* dftu_series_sub(const dftu_series* a, const dftu_series* b) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_sub(flat_a, b));
    DFTU_FLAT_OPERAND(b, flat_b, dftu_series_sub(a, flat_b));

    return dftracer::utils::dataframe::sub_columns(a, b);
}
dftu_series* dftu_series_mul(const dftu_series* a, const dftu_series* b) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_mul(flat_a, b));
    DFTU_FLAT_OPERAND(b, flat_b, dftu_series_mul(a, flat_b));

    return dftracer::utils::dataframe::mul_columns(a, b);
}
dftu_series* dftu_series_div(const dftu_series* a, const dftu_series* b) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_div(flat_a, b));
    DFTU_FLAT_OPERAND(b, flat_b, dftu_series_div(a, flat_b));

    return dftracer::utils::dataframe::div_columns(a, b);
}
dftu_series* dftu_series_add_scalar(const dftu_series* a, dftu_scalar s) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_add_scalar(flat_a, s));

    return dftracer::utils::dataframe::add_scalar_col(a, s);
}
dftu_series* dftu_series_sub_scalar(const dftu_series* a, dftu_scalar s) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_sub_scalar(flat_a, s));

    return dftracer::utils::dataframe::sub_scalar_col(a, s);
}
dftu_series* dftu_series_mul_scalar(const dftu_series* a, dftu_scalar s) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_mul_scalar(flat_a, s));

    return dftracer::utils::dataframe::mul_scalar_col(a, s);
}
dftu_series* dftu_series_div_scalar(const dftu_series* a, dftu_scalar s) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_div_scalar(flat_a, s));

    return dftracer::utils::dataframe::div_scalar_col(a, s);
}

#endif  // HWY_ONCE
