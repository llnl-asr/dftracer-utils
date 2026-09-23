#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/kernels/logical.h>

#include <cstddef>
#include <cstdint>

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "dftracer/utils/dataframe/kernels/logical.cpp"
#include <hwy/foreach_target.h>  // must precede highway.h
#include <hwy/highway.h>

HWY_BEFORE_NAMESPACE();
namespace dftracer::utils::dataframe {
namespace HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// Bitwise AND/OR/NOT over the packed bool bitmap, one u8 vector at a time; the
// trailing bytes are handled scalar. Output bytes are identical to the scalar
// byte loop, so tail-bit handling stays with the caller (NOT still masks).
void AndBytes(const std::uint8_t* a, const std::uint8_t* b, std::uint8_t* o,
              std::size_t n) {
    const hn::ScalableTag<std::uint8_t> d;
    const std::size_t lanes = hn::Lanes(d);
    std::size_t i = 0;
    for (; i + lanes <= n; i += lanes)
        hn::StoreU(hn::And(hn::LoadU(d, a + i), hn::LoadU(d, b + i)), d, o + i);
    for (; i < n; ++i) o[i] = static_cast<std::uint8_t>(a[i] & b[i]);
}

void OrBytes(const std::uint8_t* a, const std::uint8_t* b, std::uint8_t* o,
             std::size_t n) {
    const hn::ScalableTag<std::uint8_t> d;
    const std::size_t lanes = hn::Lanes(d);
    std::size_t i = 0;
    for (; i + lanes <= n; i += lanes)
        hn::StoreU(hn::Or(hn::LoadU(d, a + i), hn::LoadU(d, b + i)), d, o + i);
    for (; i < n; ++i) o[i] = static_cast<std::uint8_t>(a[i] | b[i]);
}

void NotBytes(const std::uint8_t* a, std::uint8_t* o, std::size_t n) {
    const hn::ScalableTag<std::uint8_t> d;
    const std::size_t lanes = hn::Lanes(d);
    std::size_t i = 0;
    for (; i + lanes <= n; i += lanes)
        hn::StoreU(hn::Not(hn::LoadU(d, a + i)), d, o + i);
    for (; i < n; ++i) o[i] = static_cast<std::uint8_t>(~a[i]);
}

}  // namespace HWY_NAMESPACE
}  // namespace dftracer::utils::dataframe
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace dftracer::utils::dataframe {

HWY_EXPORT(AndBytes);
HWY_EXPORT(OrBytes);
HWY_EXPORT(NotBytes);

Series logical_and(const Series& a, const Series& b) {
    return Series{
        dftu_series_logical(a.handle(), b.handle(), DFTU_LOGICAL_AND)};
}
Series logical_or(const Series& a, const Series& b) {
    return Series{dftu_series_logical(a.handle(), b.handle(), DFTU_LOGICAL_OR)};
}
Series logical_not(const Series& a) {
    return Series{dftu_series_logical_not(a.handle())};
}

}  // namespace dftracer::utils::dataframe

namespace {

// Clear bits beyond `length` in the final byte so unused high bits stay 0.
void mask_tail(std::uint8_t* bits, std::int64_t length) {
    std::int64_t rem = length & 7;
    if (rem != 0)
        bits[length >> 3] &=
            static_cast<std::uint8_t>((1u << static_cast<unsigned>(rem)) - 1);
}

}  // namespace

dftu_series* dftu_series_logical(const dftu_series* a, const dftu_series* b,
                                 dftu_logical_op op) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_logical(flat_a, b, op));
    DFTU_FLAT_OPERAND(b, flat_b, dftu_series_logical(a, flat_b, op));

    using namespace dftracer::utils::dataframe;
    using dftracer::utils::dataframe::Buffer;
    using dftracer::utils::dataframe::buffer_bytes;
    using dftracer::utils::dataframe::Encoding;
    using dftracer::utils::dataframe::LogicalOp;
    using dftracer::utils::dataframe::TypeId;
    if (a->encoding != Encoding::Flat || b->encoding != Encoding::Flat)
        return nullptr;
    if (a->type != TypeId::Bool || b->type != TypeId::Bool) return nullptr;
    if (a->length != b->length) return nullptr;
    if (op < static_cast<int32_t>(LogicalOp::And) ||
        op > static_cast<int32_t>(LogicalOp::Or))
        return nullptr;

    auto* out = new dftu_series();
    out->type = TypeId::Bool;
    out->encoding = Encoding::Flat;
    out->length = a->length;
    std::size_t bytes = buffer_bytes(TypeId::Bool, a->length);
    out->data = Buffer::allocate(bytes);

    const std::uint8_t* pa = a->data->data();
    const std::uint8_t* pb = b->data->data();
    std::uint8_t* po = out->data->data();
    switch (static_cast<LogicalOp>(op)) {
        case LogicalOp::And:
            HWY_DYNAMIC_DISPATCH(AndBytes)(pa, pb, po, bytes);
            break;
        case LogicalOp::Or:
            HWY_DYNAMIC_DISPATCH(OrBytes)(pa, pb, po, bytes);
            break;
    }
    return out;
}

dftu_series* dftu_series_logical_not(const dftu_series* a) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_logical_not(flat_a));

    using namespace dftracer::utils::dataframe;
    using dftracer::utils::dataframe::Buffer;
    using dftracer::utils::dataframe::buffer_bytes;
    using dftracer::utils::dataframe::Encoding;
    using dftracer::utils::dataframe::TypeId;
    if (a->encoding != Encoding::Flat || a->type != TypeId::Bool)
        return nullptr;

    auto* out = new dftu_series();
    out->type = TypeId::Bool;
    out->encoding = Encoding::Flat;
    out->length = a->length;
    std::size_t bytes = buffer_bytes(TypeId::Bool, a->length);
    out->data = Buffer::allocate(bytes);

    const std::uint8_t* pa = a->data->data();
    std::uint8_t* po = out->data->data();
    HWY_DYNAMIC_DISPATCH(NotBytes)(pa, po, bytes);
    mask_tail(po, a->length);
    return out;
}

#endif  // HWY_ONCE
