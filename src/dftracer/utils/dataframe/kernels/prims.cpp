// SPDX-License-Identifier: MIT
// Unary numeric primitives (ilog2/mix64/popcount/...) as a hand-written Highway
// dataframe kernel over an Int64/Uint64 column, producing an Int64 column. The
// scalar tail reuses the core bit/hash primitives so the SIMD body and the tail
// stay in agreement with the JIT/plugin path (plugins/prims.h mirrors them).

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/buffers.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/kernels/prims.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/bits.h>
#include <dftracer/utils/dstd/hash/constants.h>
#include <dftracer/utils/dstd/hash/splitmix64.h>

#include <cstddef>
#include <cstdint>

// Highway runtime dispatch: recompile this TU once per ISA, pick the best at
// load time.
#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "dftracer/utils/dataframe/kernels/prims.cpp"
#include <hwy/foreach_target.h>  // must precede highway.h
#include <hwy/highway.h>

HWY_BEFORE_NAMESPACE();
namespace dftracer::utils::dataframe {
namespace HWY_NAMESPACE {

void PrimImpl(std::int32_t op, const std::uint64_t* a, std::int64_t* out_i64,
              std::size_t n) {
    auto* out = reinterpret_cast<std::uint64_t*>(out_i64);
    const hwy::HWY_NAMESPACE::ScalableTag<std::uint64_t> d;
    const std::size_t lanes = hwy::HWY_NAMESPACE::Lanes(d);
    const auto zero = hwy::HWY_NAMESPACE::Zero(d);
    std::size_t i = 0;

    switch (static_cast<Prim>(op)) {
        case Prim::Ilog2:
            for (; i + lanes <= n; i += lanes) {
                const auto x = hwy::HWY_NAMESPACE::LoadU(d, a + i);
                auto r = hwy::HWY_NAMESPACE::Sub(
                    hwy::HWY_NAMESPACE::Set(d, 63),
                    hwy::HWY_NAMESPACE::LeadingZeroCount(x));
                r = hwy::HWY_NAMESPACE::IfThenElseZero(
                    hwy::HWY_NAMESPACE::Ne(x, zero), r);
                hwy::HWY_NAMESPACE::StoreU(r, d, out + i);
            }
            for (; i < n; ++i)
                out[i] = static_cast<std::uint64_t>(
                    dftracer::utils::dstd::bits::ilog2_u64(a[i]));
            break;
        case Prim::BitWidth:
            for (; i + lanes <= n; i += lanes) {
                const auto x = hwy::HWY_NAMESPACE::LoadU(d, a + i);
                auto r = hwy::HWY_NAMESPACE::Sub(
                    hwy::HWY_NAMESPACE::Set(d, 64),
                    hwy::HWY_NAMESPACE::LeadingZeroCount(x));
                r = hwy::HWY_NAMESPACE::IfThenElseZero(
                    hwy::HWY_NAMESPACE::Ne(x, zero), r);
                hwy::HWY_NAMESPACE::StoreU(r, d, out + i);
            }
            for (; i < n; ++i)
                out[i] = static_cast<std::uint64_t>(
                    dftracer::utils::dstd::bits::bit_width_u64(a[i]));
            break;
        case Prim::Popcount:
            for (; i + lanes <= n; i += lanes)
                hwy::HWY_NAMESPACE::StoreU(
                    hwy::HWY_NAMESPACE::PopulationCount(
                        hwy::HWY_NAMESPACE::LoadU(d, a + i)),
                    d, out + i);
            for (; i < n; ++i)
                out[i] = static_cast<std::uint64_t>(
                    dftracer::utils::dstd::bits::popcount_u64(a[i]));
            break;
        case Prim::Clz:
            for (; i + lanes <= n; i += lanes)
                hwy::HWY_NAMESPACE::StoreU(
                    hwy::HWY_NAMESPACE::LeadingZeroCount(
                        hwy::HWY_NAMESPACE::LoadU(d, a + i)),
                    d, out + i);
            for (; i < n; ++i)
                out[i] = static_cast<std::uint64_t>(
                    dftracer::utils::dstd::bits::clz_u64(a[i]));
            break;
        case Prim::Ctz:
            for (; i + lanes <= n; i += lanes)
                hwy::HWY_NAMESPACE::StoreU(
                    hwy::HWY_NAMESPACE::TrailingZeroCount(
                        hwy::HWY_NAMESPACE::LoadU(d, a + i)),
                    d, out + i);
            for (; i < n; ++i)
                out[i] = static_cast<std::uint64_t>(
                    dftracer::utils::dstd::bits::ctz_u64(a[i]));
            break;
        case Prim::Mix64: {
            const auto k1 = hwy::HWY_NAMESPACE::Set(
                d, dftracer::utils::dstd::hash::GOLDEN_RATIO);
            const auto m1 = hwy::HWY_NAMESPACE::Set(
                d, dftracer::utils::dstd::hash::SPLITMIX64_MUL1);
            const auto m2 = hwy::HWY_NAMESPACE::Set(
                d, dftracer::utils::dstd::hash::SPLITMIX64_MUL2);
            for (; i + lanes <= n; i += lanes) {
                auto x = hwy::HWY_NAMESPACE::Add(
                    hwy::HWY_NAMESPACE::LoadU(d, a + i), k1);
                x = hwy::HWY_NAMESPACE::Mul(
                    hwy::HWY_NAMESPACE::Xor(
                        x, hwy::HWY_NAMESPACE::ShiftRight<30>(x)),
                    m1);
                x = hwy::HWY_NAMESPACE::Mul(
                    hwy::HWY_NAMESPACE::Xor(
                        x, hwy::HWY_NAMESPACE::ShiftRight<27>(x)),
                    m2);
                x = hwy::HWY_NAMESPACE::Xor(
                    x, hwy::HWY_NAMESPACE::ShiftRight<31>(x));
                hwy::HWY_NAMESPACE::StoreU(x, d, out + i);
            }
            for (; i < n; ++i)
                out[i] = dftracer::utils::dstd::hash::splitmix64(a[i]);
            break;
        }
        default:
            for (; i < n; ++i) out[i] = 0;
    }
}

}  // namespace HWY_NAMESPACE
}  // namespace dftracer::utils::dataframe
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace dftracer::utils::dataframe {

HWY_EXPORT(PrimImpl);

namespace {

dftu_series* prim_col(const dftu_series* a, std::int32_t op) {
    if (a == nullptr || a->encoding != Encoding::Flat) return nullptr;
    if (a->type != TypeId::Int64 && a->type != TypeId::Uint64) return nullptr;

    auto* out = new dftu_series();
    out->type = TypeId::Int64;
    out->encoding = Encoding::Flat;
    out->length = a->length;
    out->null_count = a->null_count;
    out->validity = a->validity;
    out->data =
        new_buffer(static_cast<std::size_t>(a->length) * sizeof(std::int64_t));

    const auto* in = reinterpret_cast<const std::uint64_t*>(a->data.data());
    auto* o = reinterpret_cast<std::int64_t*>(writable(out->data));
    HWY_DYNAMIC_DISPATCH(PrimImpl)(op, in, o,
                                   static_cast<std::size_t>(a->length));
    return out;
}

}  // namespace

Series prim(const Series& a, Prim op) {
    return Series{dftu_series_prim(a.handle(), static_cast<dftu_prim_op>(op))};
}

}  // namespace dftracer::utils::dataframe

dftu_series* dftu_series_prim(const dftu_series* a, dftu_prim_op op) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_prim(flat_a, op));

    return dftracer::utils::dataframe::prim_col(a, op);
}

#endif  // HWY_ONCE
