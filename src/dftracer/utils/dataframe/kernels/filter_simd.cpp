#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/filter_simd.h>
#include <dftracer/utils/dataframe/internal/scalar.h>

#include <cstddef>
#include <cstdint>

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "dftracer/utils/dataframe/kernels/filter_simd.cpp"
#include <hwy/foreach_target.h>  // must precede highway.h
#include <hwy/highway.h>

HWY_BEFORE_NAMESPACE();
namespace dftracer::utils::dataframe {
namespace HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// Indices where p[i] > thr. The comparison is vectorized; its mask bits are
// extracted with StoreMaskBits and scanned to emit indices as plain integers
// (i + set-lane), so there is no cross-type mask and every type is exact.
template <class T>
std::int64_t compact_gt_lanes(const T* p, std::int64_t n, T thr,
                              std::int64_t* out) {
    const hn::ScalableTag<T> d;
    const std::size_t lanes = hn::Lanes(d);
    const auto vthr = hn::Set(d, thr);
    std::int64_t count = 0;
    std::int64_t i = 0;
    for (; i + static_cast<std::int64_t>(lanes) <= n;
         i += static_cast<std::int64_t>(lanes)) {
        const auto m = hn::Gt(hn::LoadU(d, p + i), vthr);
        std::uint64_t bits = 0;
        hn::StoreMaskBits(d, m, reinterpret_cast<std::uint8_t*>(&bits));
        while (bits != 0) {
            unsigned b = static_cast<unsigned>(__builtin_ctzll(bits));
            out[count++] = static_cast<std::int64_t>(i + b);
            bits &= bits - 1;
        }
    }
    for (; i < n; ++i)
        if (p[i] > thr) out[count++] = static_cast<std::int64_t>(i);
    return count;
}

std::int64_t CompactI64(const void* p, std::int64_t n, dftu_scalar thr,
                        std::int64_t* out) {
    return compact_gt_lanes<std::int64_t>(static_cast<const std::int64_t*>(p),
                                          n, scalar_as<std::int64_t>(thr), out);
}
std::int64_t CompactU64(const void* p, std::int64_t n, dftu_scalar thr,
                        std::int64_t* out) {
    return compact_gt_lanes<std::uint64_t>(static_cast<const std::uint64_t*>(p),
                                           n, scalar_as<std::uint64_t>(thr),
                                           out);
}
std::int64_t CompactF64(const void* p, std::int64_t n, dftu_scalar thr,
                        std::int64_t* out) {
    return compact_gt_lanes<double>(static_cast<const double*>(p), n,
                                    scalar_as<double>(thr), out);
}
std::int64_t CompactI32(const void* p, std::int64_t n, dftu_scalar thr,
                        std::int64_t* out) {
    return compact_gt_lanes<std::int32_t>(static_cast<const std::int32_t*>(p),
                                          n, scalar_as<std::int32_t>(thr), out);
}
std::int64_t CompactU32(const void* p, std::int64_t n, dftu_scalar thr,
                        std::int64_t* out) {
    return compact_gt_lanes<std::uint32_t>(static_cast<const std::uint32_t*>(p),
                                           n, scalar_as<std::uint32_t>(thr),
                                           out);
}
std::int64_t CompactF32(const void* p, std::int64_t n, dftu_scalar thr,
                        std::int64_t* out) {
    return compact_gt_lanes<float>(static_cast<const float*>(p), n,
                                   scalar_as<float>(thr), out);
}
std::int64_t CompactI16(const void* p, std::int64_t n, dftu_scalar thr,
                        std::int64_t* out) {
    return compact_gt_lanes<std::int16_t>(static_cast<const std::int16_t*>(p),
                                          n, scalar_as<std::int16_t>(thr), out);
}
std::int64_t CompactU16(const void* p, std::int64_t n, dftu_scalar thr,
                        std::int64_t* out) {
    return compact_gt_lanes<std::uint16_t>(static_cast<const std::uint16_t*>(p),
                                           n, scalar_as<std::uint16_t>(thr),
                                           out);
}
std::int64_t CompactI8(const void* p, std::int64_t n, dftu_scalar thr,
                       std::int64_t* out) {
    return compact_gt_lanes<std::int8_t>(static_cast<const std::int8_t*>(p), n,
                                         scalar_as<std::int8_t>(thr), out);
}
std::int64_t CompactU8(const void* p, std::int64_t n, dftu_scalar thr,
                       std::int64_t* out) {
    return compact_gt_lanes<std::uint8_t>(static_cast<const std::uint8_t*>(p),
                                          n, scalar_as<std::uint8_t>(thr), out);
}

}  // namespace HWY_NAMESPACE
}  // namespace dftracer::utils::dataframe
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace dftracer::utils::dataframe {

HWY_EXPORT(CompactI64);
HWY_EXPORT(CompactU64);
HWY_EXPORT(CompactF64);
HWY_EXPORT(CompactI32);
HWY_EXPORT(CompactU32);
HWY_EXPORT(CompactF32);
HWY_EXPORT(CompactI16);
HWY_EXPORT(CompactU16);
HWY_EXPORT(CompactI8);
HWY_EXPORT(CompactU8);

std::int64_t compact_gt(const dftu_series& v, dftu_scalar threshold,
                        std::int64_t* out) {
    if (v.validity) return -1;  // nulls need masking; caller does scalar
    const void* p = v.data->data();
    std::int64_t n = v.length;
    switch (v.type) {
        case TypeId::Int64:
            return HWY_DYNAMIC_DISPATCH(CompactI64)(p, n, threshold, out);
        case TypeId::Uint64:
            return HWY_DYNAMIC_DISPATCH(CompactU64)(p, n, threshold, out);
        case TypeId::Float64:
            return HWY_DYNAMIC_DISPATCH(CompactF64)(p, n, threshold, out);
        case TypeId::Int32:
            return HWY_DYNAMIC_DISPATCH(CompactI32)(p, n, threshold, out);
        case TypeId::Uint32:
            return HWY_DYNAMIC_DISPATCH(CompactU32)(p, n, threshold, out);
        case TypeId::Float32:
            return HWY_DYNAMIC_DISPATCH(CompactF32)(p, n, threshold, out);
        case TypeId::Int16:
            return HWY_DYNAMIC_DISPATCH(CompactI16)(p, n, threshold, out);
        case TypeId::Uint16:
            return HWY_DYNAMIC_DISPATCH(CompactU16)(p, n, threshold, out);
        case TypeId::Int8:
            return HWY_DYNAMIC_DISPATCH(CompactI8)(p, n, threshold, out);
        case TypeId::Uint8:
            return HWY_DYNAMIC_DISPATCH(CompactU8)(p, n, threshold, out);
        default:
            return -1;  // non-numeric: scalar path
    }
}

}  // namespace dftracer::utils::dataframe
#endif  // HWY_ONCE
