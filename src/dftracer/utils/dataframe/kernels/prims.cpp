// Unary numeric primitives (ilog2/mix64/popcount/...) as a hand-written Highway
// dataframe kernel over an Int64/Uint64 column, producing an Int64 column. The
// scalar tail reuses the core bit/hash primitives so the SIMD body and the tail
// stay in agreement with the JIT/plugin path (plugins/prims.h mirrors them).

#include <dftracer/utils/core/common/bits.h>
#include <dftracer/utils/core/common/hash/constants.h>
#include <dftracer/utils/core/common/hash/splitmix64.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/buffer.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/kernels/prims.h>
#include <dftracer/utils/dataframe/types.h>

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
namespace hn = hwy::HWY_NAMESPACE;

void PrimImpl(std::int32_t op, const std::uint64_t* a, std::int64_t* out_i64,
              std::size_t n) {
    auto* out = reinterpret_cast<std::uint64_t*>(out_i64);
    const hn::ScalableTag<std::uint64_t> d;
    const std::size_t lanes = hn::Lanes(d);
    const auto zero = hn::Zero(d);
    std::size_t i = 0;

    switch (static_cast<Prim>(op)) {
        case Prim::Ilog2:
            for (; i + lanes <= n; i += lanes) {
                const auto x = hn::LoadU(d, a + i);
                auto r = hn::Sub(hn::Set(d, 63), hn::LeadingZeroCount(x));
                r = hn::IfThenElseZero(hn::Ne(x, zero), r);
                hn::StoreU(r, d, out + i);
            }
            for (; i < n; ++i)
                out[i] = static_cast<std::uint64_t>(
                    dftracer::utils::bits::ilog2_u64(a[i]));
            break;
        case Prim::BitWidth:
            for (; i + lanes <= n; i += lanes) {
                const auto x = hn::LoadU(d, a + i);
                auto r = hn::Sub(hn::Set(d, 64), hn::LeadingZeroCount(x));
                r = hn::IfThenElseZero(hn::Ne(x, zero), r);
                hn::StoreU(r, d, out + i);
            }
            for (; i < n; ++i)
                out[i] = static_cast<std::uint64_t>(
                    dftracer::utils::bits::bit_width_u64(a[i]));
            break;
        case Prim::Popcount:
            for (; i + lanes <= n; i += lanes)
                hn::StoreU(hn::PopulationCount(hn::LoadU(d, a + i)), d,
                           out + i);
            for (; i < n; ++i)
                out[i] = static_cast<std::uint64_t>(
                    dftracer::utils::bits::popcount_u64(a[i]));
            break;
        case Prim::Clz:
            for (; i + lanes <= n; i += lanes)
                hn::StoreU(hn::LeadingZeroCount(hn::LoadU(d, a + i)), d,
                           out + i);
            for (; i < n; ++i)
                out[i] = static_cast<std::uint64_t>(
                    dftracer::utils::bits::clz_u64(a[i]));
            break;
        case Prim::Ctz:
            for (; i + lanes <= n; i += lanes)
                hn::StoreU(hn::TrailingZeroCount(hn::LoadU(d, a + i)), d,
                           out + i);
            for (; i < n; ++i)
                out[i] = static_cast<std::uint64_t>(
                    dftracer::utils::bits::ctz_u64(a[i]));
            break;
        case Prim::Mix64: {
            const auto k1 = hn::Set(d, dftracer::utils::hash::GOLDEN_RATIO);
            const auto m1 = hn::Set(d, dftracer::utils::hash::SPLITMIX64_MUL1);
            const auto m2 = hn::Set(d, dftracer::utils::hash::SPLITMIX64_MUL2);
            for (; i + lanes <= n; i += lanes) {
                auto x = hn::Add(hn::LoadU(d, a + i), k1);
                x = hn::Mul(hn::Xor(x, hn::ShiftRight<30>(x)), m1);
                x = hn::Mul(hn::Xor(x, hn::ShiftRight<27>(x)), m2);
                x = hn::Xor(x, hn::ShiftRight<31>(x));
                hn::StoreU(x, d, out + i);
            }
            for (; i < n; ++i) out[i] = dftracer::utils::hash::splitmix64(a[i]);
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
    out->data = Buffer::allocate(static_cast<std::size_t>(a->length) *
                                 sizeof(std::int64_t));

    const auto* in = reinterpret_cast<const std::uint64_t*>(a->data->data());
    auto* o = reinterpret_cast<std::int64_t*>(out->data->data());
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
