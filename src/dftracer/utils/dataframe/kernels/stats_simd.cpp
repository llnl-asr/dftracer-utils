// SIMD companions to the stats kernels: is_between (two lane compares + And,
// packed into a bool bitmap, mirroring compare_simd) and dot (a fused-multiply
// accumulate then a horizontal ReduceSum, like the reduce_simd sum). The
// windowed / sequential / search stats stay scalar in stats.cpp.

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/column_read.h>
#include <dftracer/utils/dataframe/internal/scalar.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "dftracer/utils/dataframe/kernels/stats_simd.cpp"
#include <hwy/foreach_target.h>  // must precede highway.h
#include <hwy/highway.h>

HWY_BEFORE_NAMESPACE();
namespace dftracer::utils::dataframe {
namespace HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// Vectorized lo <= x <= hi, packed into `out`. Bits are built 64 at a time so
// every block store lands on a byte boundary regardless of the lane count; the
// tail is packed scalar. Mirrors compare_simd's compare_bits.
template <class T>
void between_bits(const T* p, std::int64_t n, T lo, T hi, std::uint8_t* out) {
    const hn::ScalableTag<T> d;
    const std::size_t lanes = hn::Lanes(d);
    const auto vlo = hn::Set(d, lo);
    const auto vhi = hn::Set(d, hi);
    const std::uint64_t lane_mask =
        lanes >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << lanes) - 1);
    std::int64_t i = 0;
    for (; i + 64 <= n; i += 64) {
        std::uint64_t bits = 0;
        for (std::size_t c = 0; c < 64; c += lanes) {
            const auto v = hn::LoadU(d, p + i + static_cast<std::int64_t>(c));
            const auto m = hn::And(hn::Ge(v, vlo), hn::Le(v, vhi));
            std::uint64_t cb = 0;
            hn::StoreMaskBits(d, m, reinterpret_cast<std::uint8_t*>(&cb));
            bits |= (cb & lane_mask) << c;
        }
        std::memcpy(out + (i >> 3), &bits, 8);
    }
    for (; i < n; ++i)
        if (p[i] >= lo && p[i] <= hi)
            out[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
}

void BetweenI64(const void* p, std::int64_t n, dftu_scalar lo, dftu_scalar hi,
                std::uint8_t* out) {
    between_bits<std::int64_t>(static_cast<const std::int64_t*>(p), n,
                               scalar_as<std::int64_t>(lo),
                               scalar_as<std::int64_t>(hi), out);
}
void BetweenU64(const void* p, std::int64_t n, dftu_scalar lo, dftu_scalar hi,
                std::uint8_t* out) {
    between_bits<std::uint64_t>(static_cast<const std::uint64_t*>(p), n,
                                scalar_as<std::uint64_t>(lo),
                                scalar_as<std::uint64_t>(hi), out);
}
void BetweenF64(const void* p, std::int64_t n, dftu_scalar lo, dftu_scalar hi,
                std::uint8_t* out) {
    between_bits<double>(static_cast<const double*>(p), n,
                         scalar_as<double>(lo), scalar_as<double>(hi), out);
}
void BetweenI32(const void* p, std::int64_t n, dftu_scalar lo, dftu_scalar hi,
                std::uint8_t* out) {
    between_bits<std::int32_t>(static_cast<const std::int32_t*>(p), n,
                               scalar_as<std::int32_t>(lo),
                               scalar_as<std::int32_t>(hi), out);
}
void BetweenU32(const void* p, std::int64_t n, dftu_scalar lo, dftu_scalar hi,
                std::uint8_t* out) {
    between_bits<std::uint32_t>(static_cast<const std::uint32_t*>(p), n,
                                scalar_as<std::uint32_t>(lo),
                                scalar_as<std::uint32_t>(hi), out);
}
void BetweenF32(const void* p, std::int64_t n, dftu_scalar lo, dftu_scalar hi,
                std::uint8_t* out) {
    between_bits<float>(static_cast<const float*>(p), n, scalar_as<float>(lo),
                        scalar_as<float>(hi), out);
}
void BetweenI16(const void* p, std::int64_t n, dftu_scalar lo, dftu_scalar hi,
                std::uint8_t* out) {
    between_bits<std::int16_t>(static_cast<const std::int16_t*>(p), n,
                               scalar_as<std::int16_t>(lo),
                               scalar_as<std::int16_t>(hi), out);
}
void BetweenU16(const void* p, std::int64_t n, dftu_scalar lo, dftu_scalar hi,
                std::uint8_t* out) {
    between_bits<std::uint16_t>(static_cast<const std::uint16_t*>(p), n,
                                scalar_as<std::uint16_t>(lo),
                                scalar_as<std::uint16_t>(hi), out);
}
void BetweenI8(const void* p, std::int64_t n, dftu_scalar lo, dftu_scalar hi,
               std::uint8_t* out) {
    between_bits<std::int8_t>(static_cast<const std::int8_t*>(p), n,
                              scalar_as<std::int8_t>(lo),
                              scalar_as<std::int8_t>(hi), out);
}
void BetweenU8(const void* p, std::int64_t n, dftu_scalar lo, dftu_scalar hi,
               std::uint8_t* out) {
    between_bits<std::uint8_t>(static_cast<const std::uint8_t*>(p), n,
                               scalar_as<std::uint8_t>(lo),
                               scalar_as<std::uint8_t>(hi), out);
}

// Two accumulators over a fused multiply-add break the serial dependency of the
// scalar dot loop; the horizontal ReduceSum folds them at the end.
double dot_f64(const double* a, const double* b, std::int64_t n) {
    const hn::ScalableTag<double> d;
    const std::int64_t lanes = static_cast<std::int64_t>(hn::Lanes(d));
    auto acc0 = hn::Zero(d);
    auto acc1 = hn::Zero(d);
    std::int64_t i = 0;
    for (; i + 2 * lanes <= n; i += 2 * lanes) {
        acc0 = hn::MulAdd(hn::LoadU(d, a + i), hn::LoadU(d, b + i), acc0);
        acc1 = hn::MulAdd(hn::LoadU(d, a + i + lanes),
                          hn::LoadU(d, b + i + lanes), acc1);
    }
    for (; i + lanes <= n; i += lanes)
        acc0 = hn::MulAdd(hn::LoadU(d, a + i), hn::LoadU(d, b + i), acc0);
    double s = hn::ReduceSum(d, hn::Add(acc0, acc1));
    for (; i < n; ++i) s += a[i] * b[i];
    return s;
}

}  // namespace HWY_NAMESPACE
}  // namespace dftracer::utils::dataframe
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace dftracer::utils::dataframe {

HWY_EXPORT(BetweenI64);
HWY_EXPORT(BetweenU64);
HWY_EXPORT(BetweenF64);
HWY_EXPORT(BetweenI32);
HWY_EXPORT(BetweenU32);
HWY_EXPORT(BetweenF32);
HWY_EXPORT(BetweenI16);
HWY_EXPORT(BetweenU16);
HWY_EXPORT(BetweenI8);
HWY_EXPORT(BetweenU8);
HWY_EXPORT(dot_f64);

namespace {

// Exactly the types read_f64 decodes to a real value; anything else must
// refuse rather than compare against its default-case 0.
bool between_numeric(TypeId t) {
    return is_arithmetic_type(t) || is_temporal_type(t);
}

}  // namespace

extern "C" {

dftu_series* dftu_series_is_between(const dftu_series* v, dftu_scalar lo,
                                    dftu_scalar hi) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_is_between(flat_v, lo, hi));

    if (!v || v->encoding != Encoding::Flat || !between_numeric(v->type))
        return nullptr;
    auto* out = new dftu_series();
    out->type = TypeId::Bool;
    out->encoding = Encoding::Flat;
    out->length = v->length;
    out->null_count = v->null_count;
    out->validity = v->validity;  // shared: nulls propagate
    std::size_t bytes = buffer_bytes(TypeId::Bool, v->length);
    out->data = Buffer::allocate(bytes == 0 ? 1 : bytes);
    std::memset(out->data->data(), 0, out->data->size());
    const void* p = v->data->data();
    std::uint8_t* bits = out->data->data();
    const std::int64_t n = v->length;
    switch (v->type) {
        case TypeId::Int64:
            HWY_DYNAMIC_DISPATCH(BetweenI64)(p, n, lo, hi, bits);
            break;
        case TypeId::Uint64:
            HWY_DYNAMIC_DISPATCH(BetweenU64)(p, n, lo, hi, bits);
            break;
        case TypeId::Float64:
            HWY_DYNAMIC_DISPATCH(BetweenF64)(p, n, lo, hi, bits);
            break;
        case TypeId::Int32:
            HWY_DYNAMIC_DISPATCH(BetweenI32)(p, n, lo, hi, bits);
            break;
        case TypeId::Uint32:
            HWY_DYNAMIC_DISPATCH(BetweenU32)(p, n, lo, hi, bits);
            break;
        case TypeId::Float32:
            HWY_DYNAMIC_DISPATCH(BetweenF32)(p, n, lo, hi, bits);
            break;
        case TypeId::Int16:
            HWY_DYNAMIC_DISPATCH(BetweenI16)(p, n, lo, hi, bits);
            break;
        case TypeId::Uint16:
            HWY_DYNAMIC_DISPATCH(BetweenU16)(p, n, lo, hi, bits);
            break;
        case TypeId::Int8:
            HWY_DYNAMIC_DISPATCH(BetweenI8)(p, n, lo, hi, bits);
            break;
        case TypeId::Uint8:
            HWY_DYNAMIC_DISPATCH(BetweenU8)(p, n, lo, hi, bits);
            break;
        default: {
            // Any remaining numeric encoding: scalar in the widened double
            // domain.
            Series c{const_cast<dftu_series*>(v)};
            const double dlo = scalar_as<double>(lo);
            const double dhi = scalar_as<double>(hi);
            for (std::int64_t i = 0; i < n; ++i) {
                const double x = read_f64(c, i);
                if (x >= dlo && x <= dhi)
                    bits[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
            }
            c.release();
            break;
        }
    }
    return out;
}

dftu_scalar dftu_series_dot(const dftu_series* v, const dftu_series* other) {
    dftu_scalar out;
    out.kind = DFTU_SCALAR_TAG_F64;
    out.value.d = 0.0;
    if (!v || !other || v->length != other->length) return out;
    if (v->encoding != Encoding::Flat) {
        dftu_series* flat = dftu_series_materialize(v);
        if (!flat) return out;
        out = dftu_series_dot(flat, other);
        dftu_series_free(flat);
        return out;
    }
    if (other->encoding != Encoding::Flat) {
        dftu_series* flat = dftu_series_materialize(other);
        if (!flat) return out;
        out = dftu_series_dot(v, flat);
        dftu_series_free(flat);
        return out;
    }
    if (v->encoding == Encoding::Flat && other->encoding == Encoding::Flat &&
        v->type == TypeId::Float64 && other->type == TypeId::Float64 &&
        !v->validity && !other->validity) {
        out.value.d = HWY_DYNAMIC_DISPATCH(dot_f64)(
            reinterpret_cast<const double*>(v->data->data()),
            reinterpret_cast<const double*>(other->data->data()), v->length);
        return out;
    }
    Series a{const_cast<dftu_series*>(v)};
    Series b{const_cast<dftu_series*>(other)};
    const std::int64_t n = a.length();
    const bool an = a.null_count() > 0;
    const bool bn = b.null_count() > 0;
    double s = 0.0;
    for (std::int64_t i = 0; i < n; ++i) {
        if ((an && a.is_null(i)) || (bn && b.is_null(i))) continue;
        s += read_f64(a, i) * read_f64(b, i);
    }
    a.release();
    b.release();
    out.value.d = s;
    return out;
}

}  // extern "C"

}  // namespace dftracer::utils::dataframe
#endif  // HWY_ONCE
