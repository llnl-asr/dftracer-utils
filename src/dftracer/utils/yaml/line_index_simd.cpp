// SPDX-License-Identifier: MIT
#include <dftracer/utils/yaml/line_index.h>

#include <cstddef>
#include <cstdint>

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "dftracer/utils/yaml/line_index_simd.cpp"
#include <hwy/foreach_target.h>  // must precede highway.h
#include <hwy/highway.h>
#include <hwy/targets.h>

HWY_BEFORE_NAMESPACE();
namespace dftracer::utils::yaml::detail {
namespace HWY_NAMESPACE {

using Tag = hwy::HWY_NAMESPACE::CappedTag<std::uint8_t, 64>;

inline std::uint64_t Bits(Tag d, hwy::HWY_NAMESPACE::Mask<Tag> m) {
#if HWY_MAX_BYTES <= 64
    return hwy::HWY_NAMESPACE::BitsFromMask(d, m);
#else
    std::uint8_t buf[16] = {};
    hwy::HWY_NAMESPACE::StoreMaskBits(d, m, buf);
    std::uint64_t bits = 0;
    for (std::size_t i = 0; i < (hwy::HWY_NAMESPACE::Lanes(d) + 7) / 8; ++i)
        bits |= static_cast<std::uint64_t>(buf[i]) << (8 * i);
    return bits;
#endif
}

std::size_t CountNewlines(const char* text, std::size_t size) {
    const Tag d;
    const std::size_t n = hwy::HWY_NAMESPACE::Lanes(d);
    const auto lf = hwy::HWY_NAMESPACE::Set(d, '\n');
    const auto* p = reinterpret_cast<const std::uint8_t*>(text);
    std::size_t total = 0;
    std::size_t i = 0;
    for (; i + n <= size; i += n)
        total += hwy::HWY_NAMESPACE::CountTrue(
            d, hwy::HWY_NAMESPACE::Eq(hwy::HWY_NAMESPACE::LoadU(d, p + i), lf));
    for (; i < size; ++i) total += p[i] == '\n' ? 1u : 0u;
    return total;
}

// The nibble tables of the bytes that set a line apart from a plain one:
// `# ' " & * ! ? | > % @ ` [ ] { } ,` and 0x80 and above. A byte is one when
// the entries of its low and its high nibble share a bit. The method is the
// byte classification of simdjson (Apache-2.0 OR MIT,
// https://github.com/simdjson/simdjson; Langdale and Lemire, "Parsing
// Gigabytes of JSON per Second", https://arxiv.org/abs/1902.08318).
alignas(16) constexpr std::uint8_t SPECIAL_LO[16] = {
    0x84, 0x81, 0x81, 0x81, 0x80, 0x81, 0x81, 0x81,
    0x80, 0x80, 0x81, 0x88, 0x91, 0x88, 0x82, 0x82};
alignas(16) constexpr std::uint8_t SPECIAL_HI[16] = {
    0x00, 0x00, 0x01, 0x02, 0x04, 0x08, 0x04, 0x18,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};

// A 64-bit mask for each byte class of each 64-byte block, built from vector
// compares: the structure of the first stage of simdjson (as above). The
// special, control and high masks are rare, so a block makes each only when
// one of its bytes is of that class.
void LineMasks(const char* text, std::size_t block_count,
               const MaskArrays& out) {
    const Tag d;
    const std::size_t n = hwy::HWY_NAMESPACE::Lanes(d);
    const auto lf = hwy::HWY_NAMESPACE::Set(d, '\n');
    const auto cr = hwy::HWY_NAMESPACE::Set(d, '\r');
    const auto space = hwy::HWY_NAMESPACE::Set(d, ' ');
    const auto colon = hwy::HWY_NAMESPACE::Set(d, ':');
    const auto del = hwy::HWY_NAMESPACE::Set(d, 0x7F);
    const auto before_del = hwy::HWY_NAMESPACE::Set(d, 0x7E);
    const auto low_nibble = hwy::HWY_NAMESPACE::Set(d, 0x0F);
    const auto lo_table = hwy::HWY_NAMESPACE::LoadDup128(d, SPECIAL_LO);
    const auto hi_table = hwy::HWY_NAMESPACE::LoadDup128(d, SPECIAL_HI);
    // Not 0 for a byte that sets a line apart from a plain one.
    const auto special_class = [&](auto v) {
        return hwy::HWY_NAMESPACE::And(
            hwy::HWY_NAMESPACE::TableLookupBytes(
                lo_table, hwy::HWY_NAMESPACE::And(v, low_nibble)),
            hwy::HWY_NAMESPACE::TableLookupBytes(
                hi_table, hwy::HWY_NAMESPACE::ShiftRight<4>(v)));
    };
    const auto* p = reinterpret_cast<const std::uint8_t*>(text);
    for (std::size_t b = 0; b < block_count; ++b) {
        const std::uint8_t* q = p + b * 64;
        std::uint64_t nlm = 0;
        std::uint64_t spm = 0;
        std::uint64_t colm = 0;
        auto specials = hwy::HWY_NAMESPACE::Zero(d);
        auto rare = hwy::HWY_NAMESPACE::Zero(d);
        for (std::size_t k = 0; k < 64; k += n) {
            const auto v = hwy::HWY_NAMESPACE::LoadU(d, q + k);
            const auto is_lf = hwy::HWY_NAMESPACE::Eq(v, lf);
            nlm |= Bits(d, is_lf) << k;
            spm |= Bits(d, hwy::HWY_NAMESPACE::Eq(v, space)) << k;
            colm |= Bits(d, hwy::HWY_NAMESPACE::Eq(v, colon)) << k;
            specials = hwy::HWY_NAMESPACE::Or(specials, special_class(v));
            // Below ' ' but not LF, or DEL and above.
            rare = hwy::HWY_NAMESPACE::Or(
                rare, hwy::HWY_NAMESPACE::VecFromMask(
                          d, hwy::HWY_NAMESPACE::Or(
                                 hwy::HWY_NAMESPACE::AndNot(
                                     is_lf, hwy::HWY_NAMESPACE::Lt(v, space)),
                                 hwy::HWY_NAMESPACE::Gt(v, before_del))));
        }
        std::uint64_t spec = 0;
        if (!hwy::HWY_NAMESPACE::AllTrue(
                d, hwy::HWY_NAMESPACE::Eq(
                       specials, hwy::HWY_NAMESPACE::Zero(d)))) [[unlikely]] {
            for (std::size_t k = 0; k < 64; k += n) {
                const auto v = hwy::HWY_NAMESPACE::LoadU(d, q + k);
                spec |=
                    Bits(d, hwy::HWY_NAMESPACE::Ne(special_class(v),
                                                   hwy::HWY_NAMESPACE::Zero(d)))
                    << k;
            }
        }
        const bool has_rare = !hwy::HWY_NAMESPACE::AllFalse(
            d, hwy::HWY_NAMESPACE::MaskFromVec(rare));
        std::uint64_t crm = 0;
        std::uint64_t ctl = 0;
        std::uint64_t hi = 0;
        if (has_rare) [[unlikely]] {
            for (std::size_t k = 0; k < 64; k += n) {
                const auto v = hwy::HWY_NAMESPACE::LoadU(d, q + k);
                crm |= Bits(d, hwy::HWY_NAMESPACE::Eq(v, cr)) << k;
                ctl |= Bits(d, hwy::HWY_NAMESPACE::Or(
                                   hwy::HWY_NAMESPACE::Lt(v, space),
                                   hwy::HWY_NAMESPACE::Eq(v, del)))
                       << k;
                hi |= Bits(d, hwy::HWY_NAMESPACE::Gt(v, del)) << k;
            }
        }
        const std::uint64_t ends = spm | nlm | crm;
        const std::uint64_t next_ends =
            (ends >> 1) | (static_cast<std::uint64_t>(
                               q[64] == ' ' || q[64] == '\n' || q[64] == '\r')
                           << 63);
        const std::uint64_t next_is_lf =
            (nlm >> 1) | (static_cast<std::uint64_t>(q[64] == '\n') << 63);
        out.nl[b] = nlm;
        out.bad[b] = (ctl & ~nlm & ~crm) | (crm & ~next_is_lf);
        out.high[b] = hi;
        out.space[b] = spm;
        out.sep[b] = colm & next_ends;
        out.special[b] = spec;
    }
}

}  // namespace HWY_NAMESPACE
}  // namespace dftracer::utils::yaml::detail
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace dftracer::utils::yaml::detail {

HWY_EXPORT(CountNewlines);
HWY_EXPORT(LineMasks);

std::size_t count_newlines(const char* text, std::size_t size) {
    return HWY_DYNAMIC_DISPATCH(CountNewlines)(text, size);
}

void line_masks(const char* text, std::size_t block_count,
                const MaskArrays& out) {
    HWY_DYNAMIC_DISPATCH(LineMasks)(text, block_count, out);
}

void set_scalar_line_masks_for_test(bool scalar) {
    hwy::SetSupportedTargetsForTest(scalar ? HWY_SCALAR : 0);
}

}  // namespace dftracer::utils::yaml::detail
#endif
