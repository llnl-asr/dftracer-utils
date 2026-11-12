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

alignas(16) constexpr std::uint8_t LANE_BITS[16] = {
    1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};

// The 64 bits of the masks of four 16-byte vectors, lane i of `m3` at bit
// 48 + i, from rounds of pairwise sums of weighted lanes: the movemask of the
// NEON first stage of simdjson (Apache-2.0 OR MIT,
// https://github.com/simdjson/simdjson). Add of ConcatEven and ConcatOdd is a
// pairwise add, one instruction on NEON.
template <class D, class M>
std::uint64_t Bits64(D d, M m0, M m1, M m2, M m3) {
    const auto weights = hwy::HWY_NAMESPACE::LoadDup128(d, LANE_BITS);
    const auto pair = [&](auto lo, auto hi) {
        return hwy::HWY_NAMESPACE::Add(
            hwy::HWY_NAMESPACE::ConcatEven(d, hi, lo),
            hwy::HWY_NAMESPACE::ConcatOdd(d, hi, lo));
    };
    const auto s0 = pair(hwy::HWY_NAMESPACE::And(
                             hwy::HWY_NAMESPACE::VecFromMask(d, m0), weights),
                         hwy::HWY_NAMESPACE::And(
                             hwy::HWY_NAMESPACE::VecFromMask(d, m1), weights));
    const auto s1 = pair(hwy::HWY_NAMESPACE::And(
                             hwy::HWY_NAMESPACE::VecFromMask(d, m2), weights),
                         hwy::HWY_NAMESPACE::And(
                             hwy::HWY_NAMESPACE::VecFromMask(d, m3), weights));
    const auto s2 = pair(s0, s1);
    const auto s3 = pair(s2, s2);
    return hwy::HWY_NAMESPACE::GetLane(hwy::HWY_NAMESPACE::BitCast(
        hwy::HWY_NAMESPACE::Repartition<std::uint64_t, D>(), s3));
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

// The nibble tables of the classes of a byte: the bytes that set a line apart
// from a plain one (`# ' " & * ! ? | > % @ ` [ ] { } ,`, SPECIAL_BITS), the
// control bytes other than LF (0x20 and 0x40) and 0x80 and above (0x80, which
// is also special). A byte is in a class when the entries of its low and its
// high nibble share its bit. The method is the byte classification of simdjson
// (Apache-2.0 OR MIT, https://github.com/simdjson/simdjson; Langdale and
// Lemire, "Parsing Gigabytes of JSON per Second",
// https://arxiv.org/abs/1902.08318).
alignas(16) constexpr std::uint8_t CLASS_LO[16] = {
    0xE4, 0xE1, 0xE1, 0xE1, 0xE0, 0xE1, 0xE1, 0xE1,
    0xE0, 0xE0, 0xC1, 0xE8, 0xF1, 0xE8, 0xE2, 0xE2};
alignas(16) constexpr std::uint8_t CLASS_HI[16] = {
    0x20, 0x40, 0x01, 0x02, 0x04, 0x08, 0x04, 0x18,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
constexpr std::uint8_t SPECIAL_BITS = 0x9F;
constexpr std::uint8_t RARE_BITS = 0xE0;

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
    const auto low_nibble = hwy::HWY_NAMESPACE::Set(d, 0x0F);
    const auto lo_table = hwy::HWY_NAMESPACE::LoadDup128(d, CLASS_LO);
    const auto hi_table = hwy::HWY_NAMESPACE::LoadDup128(d, CLASS_HI);
    // The class bits of each byte.
    const auto byte_class = [&](auto v) {
        return hwy::HWY_NAMESPACE::And(
            hwy::HWY_NAMESPACE::TableLookupBytes(
                lo_table, hwy::HWY_NAMESPACE::And(v, low_nibble)),
            hwy::HWY_NAMESPACE::TableLookupBytes(
                hi_table, hwy::HWY_NAMESPACE::ShiftRight<4>(v)));
    };
    const auto any_set = [&](auto x) {
        return !hwy::HWY_NAMESPACE::AllTrue(
            d, hwy::HWY_NAMESPACE::Eq(x, hwy::HWY_NAMESPACE::Zero(d)));
    };
    const auto* p = reinterpret_cast<const std::uint8_t*>(text);
    for (std::size_t b = 0; b < block_count; ++b) {
        const std::uint8_t* q = p + b * 64;
        std::uint64_t nlm = 0;
        std::uint64_t spm = 0;
        std::uint64_t colm = 0;
        auto classes = hwy::HWY_NAMESPACE::Zero(d);
        auto dels = hwy::HWY_NAMESPACE::Zero(d);
        if constexpr (hwy::HWY_NAMESPACE::MaxLanes(Tag()) == 16 &&
                      HWY_TARGET != HWY_SCALAR) {
            const auto v0 = hwy::HWY_NAMESPACE::LoadU(d, q);
            const auto v1 = hwy::HWY_NAMESPACE::LoadU(d, q + 16);
            const auto v2 = hwy::HWY_NAMESPACE::LoadU(d, q + 32);
            const auto v3 = hwy::HWY_NAMESPACE::LoadU(d, q + 48);
            nlm = Bits64(d, hwy::HWY_NAMESPACE::Eq(v0, lf),
                         hwy::HWY_NAMESPACE::Eq(v1, lf),
                         hwy::HWY_NAMESPACE::Eq(v2, lf),
                         hwy::HWY_NAMESPACE::Eq(v3, lf));
            spm = Bits64(d, hwy::HWY_NAMESPACE::Eq(v0, space),
                         hwy::HWY_NAMESPACE::Eq(v1, space),
                         hwy::HWY_NAMESPACE::Eq(v2, space),
                         hwy::HWY_NAMESPACE::Eq(v3, space));
            colm = Bits64(d, hwy::HWY_NAMESPACE::Eq(v0, colon),
                          hwy::HWY_NAMESPACE::Eq(v1, colon),
                          hwy::HWY_NAMESPACE::Eq(v2, colon),
                          hwy::HWY_NAMESPACE::Eq(v3, colon));
        }
        for (std::size_t k = 0; k < 64; k += n) {
            const auto v = hwy::HWY_NAMESPACE::LoadU(d, q + k);
            const auto is_lf = hwy::HWY_NAMESPACE::Eq(v, lf);
            if constexpr (hwy::HWY_NAMESPACE::MaxLanes(Tag()) != 16 ||
                          HWY_TARGET == HWY_SCALAR) {
                nlm |= Bits(d, is_lf) << k;
                spm |= Bits(d, hwy::HWY_NAMESPACE::Eq(v, space)) << k;
                colm |= Bits(d, hwy::HWY_NAMESPACE::Eq(v, colon)) << k;
            }
            classes = hwy::HWY_NAMESPACE::Or(classes, byte_class(v));
            dels = hwy::HWY_NAMESPACE::Or(
                dels, hwy::HWY_NAMESPACE::VecFromMask(
                          d, hwy::HWY_NAMESPACE::Eq(v, del)));
        }
        const auto special_bits = hwy::HWY_NAMESPACE::Set(d, SPECIAL_BITS);
        std::uint64_t spec = 0;
        if (any_set(hwy::HWY_NAMESPACE::And(classes, special_bits)))
            [[unlikely]] {
            for (std::size_t k = 0; k < 64; k += n) {
                const auto v = hwy::HWY_NAMESPACE::LoadU(d, q + k);
                spec |= Bits(d, hwy::HWY_NAMESPACE::Ne(
                                    hwy::HWY_NAMESPACE::And(byte_class(v),
                                                            special_bits),
                                    hwy::HWY_NAMESPACE::Zero(d)))
                        << k;
            }
        }
        const bool has_rare = any_set(hwy::HWY_NAMESPACE::Or(
            hwy::HWY_NAMESPACE::And(classes,
                                    hwy::HWY_NAMESPACE::Set(d, RARE_BITS)),
            dels));
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
