// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/dataframe/internal/string_simd.h>
#include <dftracer/utils/dstd/ascii.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "dftracer/utils/dataframe/kernels/string_simd.cpp"
#include <hwy/foreach_target.h>  // must precede highway.h
#include <hwy/highway.h>

HWY_BEFORE_NAMESPACE();
namespace dftracer::utils::dataframe {
namespace HWY_NAMESPACE {

using hn_u8 = std::uint8_t;

// 64 bytes a block, whatever the vector width: a block's mask is one 64-bit
// word, so the bit arrays do not depend on the target.
using Tag = hwy::HWY_NAMESPACE::CappedTag<hn_u8, 64>;

// v in [lo, hi], unsigned: the wrapped difference is small exactly then.
template <class D, class V>
HWY_INLINE auto in_range(D d, V v, int lo, int hi) {
    return hwy::HWY_NAMESPACE::Le(
        hwy::HWY_NAMESPACE::Sub(
            v, hwy::HWY_NAMESPACE::Set(d, static_cast<hn_u8>(lo))),
        hwy::HWY_NAMESPACE::Set(d, static_cast<hn_u8>(hi - lo)));
}

// A..Z or a..z: or-ing 0x20 folds the two ranges onto a..z, and no other byte
// lands there.
template <class D, class V>
HWY_INLINE auto is_alpha(D d, V v) {
    return in_range(
        d, hwy::HWY_NAMESPACE::Or(v, hwy::HWY_NAMESPACE::Set(d, hn_u8{0x20})),
        'a', 'z');
}
template <class D, class V>
HWY_INLINE auto is_space(D d, V v) {
    return hwy::HWY_NAMESPACE::Or(
        hwy::HWY_NAMESPACE::Eq(v, hwy::HWY_NAMESPACE::Set(d, hn_u8{' '})),
        in_range(d, v, 9, 13));
}

template <int K, class D, class V>
HWY_INLINE auto bad_mask(D d, V v, V pv, V veq) {
    using strsimd::BitKind;
    if constexpr (K == static_cast<int>(BitKind::Alnum))
        return hwy::HWY_NAMESPACE::Not(
            hwy::HWY_NAMESPACE::Or(is_alpha(d, v), in_range(d, v, '0', '9')));
    else if constexpr (K == static_cast<int>(BitKind::Alpha))
        return hwy::HWY_NAMESPACE::Not(is_alpha(d, v));
    else if constexpr (K == static_cast<int>(BitKind::Digit))
        return hwy::HWY_NAMESPACE::Not(in_range(d, v, '0', '9'));
    else if constexpr (K == static_cast<int>(BitKind::Space))
        return hwy::HWY_NAMESPACE::Not(is_space(d, v));
    else if constexpr (K == static_cast<int>(BitKind::Lower))
        return in_range(d, v, 'A', 'Z');
    else if constexpr (K == static_cast<int>(BitKind::Upper))
        return in_range(d, v, 'a', 'z');
    else if constexpr (K == static_cast<int>(BitKind::Title)) {
        const auto prev_cased = is_alpha(d, pv);
        return hwy::HWY_NAMESPACE::Or(
            hwy::HWY_NAMESPACE::And(in_range(d, v, 'A', 'Z'), prev_cased),
            hwy::HWY_NAMESPACE::AndNot(prev_cased, in_range(d, v, 'a', 'z')));
    } else
        return hwy::HWY_NAMESPACE::Eq(v, veq);
}

template <int K, class D, class V>
HWY_INLINE auto aux_mask(D d, V v) {
    using strsimd::BitKind;
    if constexpr (K == static_cast<int>(BitKind::Lower))
        return in_range(d, v, 'a', 'z');
    else if constexpr (K == static_cast<int>(BitKind::Upper))
        return in_range(d, v, 'A', 'Z');
    else
        return is_alpha(d, v);  // Title (the other kinds never ask)
}

// The two 64-bit words of one block: `cur` is the 64 bytes, `prev` the 64
// bytes shifted back by one (prev[c] precedes cur[c]).
template <int K>
HWY_INLINE void block_words(const hn_u8* cur, const hn_u8* prev, hn_u8 eq,
                            std::uint64_t* bad, std::uint64_t* aux) {
    using strsimd::BitKind;
    constexpr bool has_aux = K == static_cast<int>(BitKind::Lower) ||
                             K == static_cast<int>(BitKind::Upper) ||
                             K == static_cast<int>(BitKind::Title);
    constexpr bool has_prev = K == static_cast<int>(BitKind::Title);
    const Tag d;
    const std::size_t lanes = hwy::HWY_NAMESPACE::Lanes(d);
    const auto veq = hwy::HWY_NAMESPACE::Set(d, eq);
    const std::uint64_t lane_mask =
        lanes >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << lanes) - 1);
    std::uint64_t b = 0, a = 0;
    for (std::size_t c = 0; c < 64; c += lanes) {
        const auto v = hwy::HWY_NAMESPACE::LoadU(d, cur + c);
        const auto pv = has_prev ? hwy::HWY_NAMESPACE::LoadU(d, prev + c) : v;
        std::uint64_t cb = 0;
        hwy::HWY_NAMESPACE::StoreMaskBits(d, bad_mask<K>(d, v, pv, veq),
                                          reinterpret_cast<std::uint8_t*>(&cb));
        b |= (cb & lane_mask) << c;
        if constexpr (has_aux) {
            std::uint64_t ca = 0;
            hwy::HWY_NAMESPACE::StoreMaskBits(
                d, aux_mask<K>(d, v), reinterpret_cast<std::uint8_t*>(&ca));
            a |= (ca & lane_mask) << c;
        }
    }
    *bad = b;
    *aux = a;
}

template <int K>
void bits_blocks(const hn_u8* p, std::size_t n, std::size_t w0, std::size_t w1,
                 hn_u8 eq, std::uint64_t* bad, std::uint64_t* aux) {
    for (std::size_t w = w0; w < w1; ++w) {
        const std::size_t base = w * 64;
        if (base >= n) {
            bad[w] = 0;
            if (aux) aux[w] = 0;
            continue;
        }
        std::uint64_t b, a;
        if (base != 0 && base + 64 <= n) {
            block_words<K>(p + base, p + base - 1, eq, &b, &a);
        } else {
            // The first block has no byte before it and the last one is
            // short: stage them with a zero (non-letter) byte in front and
            // zeros after, and mask the padding off the result.
            hn_u8 tmp[65] = {};
            const std::size_t valid = std::min<std::size_t>(64, n - base);
            tmp[0] = base != 0 ? p[base - 1] : hn_u8{0};
            std::memcpy(tmp + 1, p + base, valid);
            block_words<K>(tmp + 1, tmp, eq, &b, &a);
            const std::uint64_t keep = valid >= 64
                                           ? ~std::uint64_t{0}
                                           : ((std::uint64_t{1} << valid) - 1);
            b &= keep;
            a &= keep;
        }
        bad[w] = b;
        if (aux) aux[w] = a;
    }
}

void ClassBits(int kind, const hn_u8* p, std::size_t n, std::size_t w0,
               std::size_t w1, hn_u8 eq, std::uint64_t* bad,
               std::uint64_t* aux) {
    using strsimd::BitKind;
    switch (static_cast<BitKind>(kind)) {
        case BitKind::Alnum:
            bits_blocks<0>(p, n, w0, w1, eq, bad, aux);
            break;
        case BitKind::Alpha:
            bits_blocks<1>(p, n, w0, w1, eq, bad, aux);
            break;
        case BitKind::Digit:
            bits_blocks<2>(p, n, w0, w1, eq, bad, aux);
            break;
        case BitKind::Space:
            bits_blocks<3>(p, n, w0, w1, eq, bad, aux);
            break;
        case BitKind::Lower:
            bits_blocks<4>(p, n, w0, w1, eq, bad, aux);
            break;
        case BitKind::Upper:
            bits_blocks<5>(p, n, w0, w1, eq, bad, aux);
            break;
        case BitKind::Title:
            bits_blocks<6>(p, n, w0, w1, eq, bad, aux);
            break;
        case BitKind::Eq:
            bits_blocks<7>(p, n, w0, w1, eq, bad, aux);
            break;
    }
}

// Case maps: a letter's case is bit 5, so each flips it with an xor under a
// mask. Title also reads the previous byte (an unaligned load one back).
template <int K>
void case_blocks(const hn_u8* in, hn_u8* out, std::size_t n) {
    using strsimd::CaseKind;
    const Tag d;
    const std::size_t lanes = hwy::HWY_NAMESPACE::Lanes(d);
    const auto flip = hwy::HWY_NAMESPACE::Set(d, hn_u8{0x20});
    std::size_t i = 0;
    if constexpr (K == static_cast<int>(CaseKind::Title)) {
        if (n >= lanes) {
            // The first vector has no previous byte: stage a zero before it.
            hn_u8 tmp[65] = {};
            std::memcpy(tmp + 1, in, lanes - 1);
            const auto v = hwy::HWY_NAMESPACE::LoadU(d, in);
            const auto pv = hwy::HWY_NAMESPACE::LoadU(d, tmp);
            const auto prev_cased = is_alpha(d, pv);
            const auto up =
                hwy::HWY_NAMESPACE::And(in_range(d, v, 'A', 'Z'), prev_cased);
            const auto lo = hwy::HWY_NAMESPACE::AndNot(
                prev_cased, in_range(d, v, 'a', 'z'));
            hwy::HWY_NAMESPACE::StoreU(hwy::HWY_NAMESPACE::IfThenElse(
                                           hwy::HWY_NAMESPACE::Or(up, lo),
                                           hwy::HWY_NAMESPACE::Xor(v, flip), v),
                                       d, out);
            i = lanes;
        }
        for (; i + lanes <= n; i += lanes) {
            const auto v = hwy::HWY_NAMESPACE::LoadU(d, in + i);
            const auto pv = hwy::HWY_NAMESPACE::LoadU(d, in + i - 1);
            const auto prev_cased = is_alpha(d, pv);
            const auto up =
                hwy::HWY_NAMESPACE::And(in_range(d, v, 'A', 'Z'), prev_cased);
            const auto lo = hwy::HWY_NAMESPACE::AndNot(
                prev_cased, in_range(d, v, 'a', 'z'));
            hwy::HWY_NAMESPACE::StoreU(hwy::HWY_NAMESPACE::IfThenElse(
                                           hwy::HWY_NAMESPACE::Or(up, lo),
                                           hwy::HWY_NAMESPACE::Xor(v, flip), v),
                                       d, out + i);
        }
        for (; i < n; ++i) {
            const hn_u8 c = in[i];
            const hn_u8 p = i != 0 ? in[i - 1] : hn_u8{0};
            const bool prev_cased = dstd::is_ascii_alpha(static_cast<char>(p));
            const bool upper = dstd::is_ascii_upper(static_cast<char>(c));
            const bool lower = dstd::is_ascii_lower(static_cast<char>(c));
            out[i] = ((upper && prev_cased) || (lower && !prev_cased))
                         ? static_cast<hn_u8>(c ^ 0x20)
                         : c;
        }
    } else {
        for (; i + lanes <= n; i += lanes) {
            const auto v = hwy::HWY_NAMESPACE::LoadU(d, in + i);
            const auto m = K == static_cast<int>(CaseKind::Swapcase)
                               ? is_alpha(d, v)
                               : in_range(d, v, 'A', 'Z');
            hwy::HWY_NAMESPACE::StoreU(
                hwy::HWY_NAMESPACE::IfThenElse(
                    m, hwy::HWY_NAMESPACE::Xor(v, flip), v),
                d, out + i);
        }
        for (; i < n; ++i) {
            const hn_u8 c = in[i];
            const bool hit = K == static_cast<int>(CaseKind::Swapcase)
                                 ? dstd::is_ascii_alpha(static_cast<char>(c))
                                 : dstd::is_ascii_upper(static_cast<char>(c));
            out[i] = hit ? static_cast<hn_u8>(c ^ 0x20) : c;
        }
    }
}

void CaseMap(int kind, const hn_u8* in, hn_u8* out, std::size_t n) {
    using strsimd::CaseKind;
    switch (static_cast<CaseKind>(kind)) {
        case CaseKind::Swapcase:
            case_blocks<0>(in, out, n);
            break;
        case CaseKind::Title:
            case_blocks<1>(in, out, n);
            break;
        case CaseKind::Lower:
            case_blocks<2>(in, out, n);
            break;
    }
}

// ---- Row-wise predicates for long strings --------------------------------
// One string at a time, vector by vector, stopping at the first vector that
// holds a bad byte. The whole-buffer pass above reads every byte of the
// column; this one reads only what it needs, and needs no bit arrays.

template <int K>
HWY_INLINE bool sb_bad(hn_u8 c, hn_u8 prev) {
    using strsimd::BitKind;
    if constexpr (K == static_cast<int>(BitKind::Alnum))
        return !(dstd::is_ascii_alpha(static_cast<char>(c)) ||
                 dstd::is_ascii_digit(static_cast<char>(c)));
    else if constexpr (K == static_cast<int>(BitKind::Alpha))
        return !dstd::is_ascii_alpha(static_cast<char>(c));
    else if constexpr (K == static_cast<int>(BitKind::Digit))
        return !dstd::is_ascii_digit(static_cast<char>(c));
    else if constexpr (K == static_cast<int>(BitKind::Space))
        return !dstd::is_ascii_space(static_cast<char>(c));
    else if constexpr (K == static_cast<int>(BitKind::Lower))
        return dstd::is_ascii_upper(static_cast<char>(c));
    else if constexpr (K == static_cast<int>(BitKind::Upper))
        return dstd::is_ascii_lower(static_cast<char>(c));
    else if constexpr (K == static_cast<int>(BitKind::Title))
        return (dstd::is_ascii_upper(static_cast<char>(c)) &&
                dstd::is_ascii_alpha(static_cast<char>(prev))) ||
               (dstd::is_ascii_lower(static_cast<char>(c)) &&
                !dstd::is_ascii_alpha(static_cast<char>(prev)));
    else
        return false;
}

template <int K>
HWY_INLINE bool sb_aux(hn_u8 c) {
    using strsimd::BitKind;
    if constexpr (K == static_cast<int>(BitKind::Lower))
        return dstd::is_ascii_lower(static_cast<char>(c));
    else if constexpr (K == static_cast<int>(BitKind::Upper))
        return dstd::is_ascii_upper(static_cast<char>(c));
    else
        return dstd::is_ascii_alpha(static_cast<char>(c));  // Title
}

// Rows [r0, r1) of a flat string column whose bytes start at data + lo: sets
// bit i of `res` when row i is non-empty, has no bad byte and (for the cased
// kinds) holds at least one cased byte.
template <int K, class Off>
void rows_range(const hn_u8* data, const Off* off, std::int64_t lo,
                std::int64_t r0, std::int64_t r1, hn_u8* res) {
    using strsimd::BitKind;
    constexpr bool is_title = K == static_cast<int>(BitKind::Title);
    constexpr bool has_aux = K == static_cast<int>(BitKind::Lower) ||
                             K == static_cast<int>(BitKind::Upper) || is_title;
    const Tag d;
    const std::size_t lanes = hwy::HWY_NAMESPACE::Lanes(d);
    const auto veq = hwy::HWY_NAMESPACE::Set(d, hn_u8{0});
    const hn_u8* p = data + lo;
    for (std::int64_t i = r0; i < r1; ++i) {
        const std::size_t s = static_cast<std::size_t>(off[i] - lo);
        const std::size_t t = static_cast<std::size_t>(off[i + 1] - lo);
        if (s >= t) continue;  // an empty string is in no class
        bool good = true;
        bool cased = false;
        std::size_t pos = s;
        if constexpr (is_title) {
            // The first byte has no previous byte: it breaks the rule when
            // lower.
            if (dstd::is_ascii_lower(static_cast<char>(p[s]))) continue;
            cased = dstd::is_ascii_alpha(static_cast<char>(p[s]));
            pos = s + 1;
        }
        std::size_t j = pos;
        auto one = [&](std::size_t at) {
            const auto v = hwy::HWY_NAMESPACE::LoadU(d, p + at);
            const auto pv =
                is_title ? hwy::HWY_NAMESPACE::LoadU(d, p + at - 1) : v;
            if (!hwy::HWY_NAMESPACE::AllFalse(d, bad_mask<K>(d, v, pv, veq))) {
                good = false;
                return;
            }
            if constexpr (has_aux)
                if (!cased &&
                    !hwy::HWY_NAMESPACE::AllFalse(d, aux_mask<K>(d, v)))
                    cased = true;
        };
        // The first vector alone (a bad byte usually shows early), then four
        // per test: one reduction per 4 blocks keeps an all-good string
        // streaming.
        if (j + lanes <= t) {
            one(j);
            j += lanes;
        }
        for (; good && j + 4 * lanes <= t; j += 4 * lanes) {
            const auto v0 = hwy::HWY_NAMESPACE::LoadU(d, p + j);
            const auto v1 = hwy::HWY_NAMESPACE::LoadU(d, p + j + lanes);
            const auto v2 = hwy::HWY_NAMESPACE::LoadU(d, p + j + 2 * lanes);
            const auto v3 = hwy::HWY_NAMESPACE::LoadU(d, p + j + 3 * lanes);
            auto pv = [&](std::size_t k, hwy::HWY_NAMESPACE::VFromD<Tag> v) {
                return is_title
                           ? hwy::HWY_NAMESPACE::LoadU(d, p + j + k * lanes - 1)
                           : v;
            };
            const auto m = hwy::HWY_NAMESPACE::Or(
                hwy::HWY_NAMESPACE::Or(bad_mask<K>(d, v0, pv(0, v0), veq),
                                       bad_mask<K>(d, v1, pv(1, v1), veq)),
                hwy::HWY_NAMESPACE::Or(bad_mask<K>(d, v2, pv(2, v2), veq),
                                       bad_mask<K>(d, v3, pv(3, v3), veq)));
            if (!hwy::HWY_NAMESPACE::AllFalse(d, m)) {
                good = false;
                break;
            }
            if constexpr (has_aux)
                if (!cased &&
                    !hwy::HWY_NAMESPACE::AllFalse(
                        d, hwy::HWY_NAMESPACE::Or(
                               hwy::HWY_NAMESPACE::Or(aux_mask<K>(d, v0),
                                                      aux_mask<K>(d, v1)),
                               hwy::HWY_NAMESPACE::Or(aux_mask<K>(d, v2),
                                                      aux_mask<K>(d, v3)))))
                    cased = true;
        }
        for (; good && j + lanes <= t; j += lanes) one(j);
        if (good && j < t) {
            if (t - pos >= lanes) {
                // One more vector ending at the last byte; the bytes it shares
                // with the previous one are checked twice, which changes
                // nothing since each byte is judged on its own (and its own
                // previous byte).
                const std::size_t k = t - lanes;
                const auto v = hwy::HWY_NAMESPACE::LoadU(d, p + k);
                const auto pv =
                    is_title ? hwy::HWY_NAMESPACE::LoadU(d, p + k - 1) : v;
                if (!hwy::HWY_NAMESPACE::AllFalse(d,
                                                  bad_mask<K>(d, v, pv, veq))) {
                    good = false;
                } else if constexpr (has_aux) {
                    if (!cased &&
                        !hwy::HWY_NAMESPACE::AllFalse(d, aux_mask<K>(d, v)))
                        cased = true;
                }
            } else {
                for (; j < t; ++j) {
                    const hn_u8 prev =
                        (is_title && j > s) ? p[j - 1] : hn_u8{0};
                    if (sb_bad<K>(p[j], prev)) {
                        good = false;
                        break;
                    }
                    if constexpr (has_aux)
                        if (sb_aux<K>(p[j])) cased = true;
                }
            }
        }
        if (good && (!has_aux || cased))
            res[i >> 3] |= static_cast<hn_u8>(1u << (i & 7));
    }
}

template <class Off>
void class_rows_dispatch(int kind, const hn_u8* data, const Off* off,
                         std::int64_t lo, std::int64_t r0, std::int64_t r1,
                         hn_u8* res) {
    using strsimd::BitKind;
    switch (static_cast<BitKind>(kind)) {
        case BitKind::Alnum:
            rows_range<0>(data, off, lo, r0, r1, res);
            break;
        case BitKind::Alpha:
            rows_range<1>(data, off, lo, r0, r1, res);
            break;
        case BitKind::Digit:
            rows_range<2>(data, off, lo, r0, r1, res);
            break;
        case BitKind::Space:
            rows_range<3>(data, off, lo, r0, r1, res);
            break;
        case BitKind::Lower:
            rows_range<4>(data, off, lo, r0, r1, res);
            break;
        case BitKind::Upper:
            rows_range<5>(data, off, lo, r0, r1, res);
            break;
        case BitKind::Title:
            rows_range<6>(data, off, lo, r0, r1, res);
            break;
        case BitKind::Eq:
            break;  // not a row predicate
    }
}

void ClassRows32(int kind, const hn_u8* data, const std::int32_t* off,
                 std::int64_t lo, std::int64_t r0, std::int64_t r1,
                 hn_u8* res) {
    class_rows_dispatch(kind, data, off, lo, r0, r1, res);
}
void ClassRows64(int kind, const hn_u8* data, const std::int64_t* off,
                 std::int64_t lo, std::int64_t r0, std::int64_t r1,
                 hn_u8* res) {
    class_rows_dispatch(kind, data, off, lo, r0, r1, res);
}

void LenBytes32(const std::int32_t* off, std::int64_t* out, std::size_t n) {
    const hwy::HWY_NAMESPACE::ScalableTag<std::int64_t> d64;
    const hwy::HWY_NAMESPACE::Rebind<std::int32_t, decltype(d64)> d32;
    const std::size_t lanes = hwy::HWY_NAMESPACE::Lanes(d64);
    std::size_t i = 0;
    for (; i + lanes <= n; i += lanes) {
        const auto a = hwy::HWY_NAMESPACE::LoadU(d32, off + i + 1);
        const auto b = hwy::HWY_NAMESPACE::LoadU(d32, off + i);
        hwy::HWY_NAMESPACE::StoreU(
            hwy::HWY_NAMESPACE::PromoteTo(d64, hwy::HWY_NAMESPACE::Sub(a, b)),
            d64, out + i);
    }
    for (; i < n; ++i) out[i] = static_cast<std::int64_t>(off[i + 1] - off[i]);
}

// Rows lie end to end in the buffer, so a row shorter than a vector still
// takes one vector load while the load stays before `end`, with the bytes past
// the row masked off. Only the rows at the end of the buffer count by byte.
void CharCounts32(const hn_u8* data, const std::int32_t* off, std::int64_t n,
                  std::int64_t* out) {
    const hwy::HWY_NAMESPACE::ScalableTag<hn_u8> d;
    const auto vc0 = hwy::HWY_NAMESPACE::Set(d, hn_u8{0xC0});
    const auto v80 = hwy::HWY_NAMESPACE::Set(d, hn_u8{0x80});
    const std::size_t lanes = hwy::HWY_NAMESPACE::Lanes(d);
    const hn_u8* end = data + off[n];
    const auto starts = [&](const hn_u8* at) {
        return hwy::HWY_NAMESPACE::Ne(
            hwy::HWY_NAMESPACE::And(hwy::HWY_NAMESPACE::LoadU(d, at), vc0),
            v80);
    };
    for (std::int64_t r = 0; r < n; ++r) {
        const hn_u8* p = data + off[r];
        const std::size_t len = static_cast<std::size_t>(off[r + 1] - off[r]);
        std::int64_t count = 0;
        std::size_t i = 0;
        for (; i + lanes <= len; i += lanes) {
            count += static_cast<std::int64_t>(
                hwy::HWY_NAMESPACE::CountTrue(d, starts(p + i)));
        }
        if (i < len) {
            if (p + i + lanes <= end) {
                const auto m = hwy::HWY_NAMESPACE::And(
                    starts(p + i), hwy::HWY_NAMESPACE::FirstN(d, len - i));
                count += static_cast<std::int64_t>(
                    hwy::HWY_NAMESPACE::CountTrue(d, m));
            } else {
                for (; i < len; ++i)
                    if ((p[i] & 0xC0) != 0x80) ++count;
            }
        }
        out[r] = count;
    }
}

void AsciiFold(const hn_u8* in, hn_u8* out, std::size_t n, hn_u8 lo, hn_u8 hi,
               int add) {
    const hwy::HWY_NAMESPACE::ScalableTag<hn_u8> d;
    const auto vlo = hwy::HWY_NAMESPACE::Set(d, lo);
    const auto vhi = hwy::HWY_NAMESPACE::Set(d, hi);
    const auto vadd = hwy::HWY_NAMESPACE::Set(d, static_cast<hn_u8>(add));
    const std::size_t lanes = hwy::HWY_NAMESPACE::Lanes(d);
    std::size_t i = 0;
    for (; i + lanes <= n; i += lanes) {
        const auto v = hwy::HWY_NAMESPACE::LoadU(d, in + i);
        const auto m = hwy::HWY_NAMESPACE::And(hwy::HWY_NAMESPACE::Ge(v, vlo),
                                               hwy::HWY_NAMESPACE::Le(v, vhi));
        hwy::HWY_NAMESPACE::StoreU(hwy::HWY_NAMESPACE::IfThenElse(
                                       m, hwy::HWY_NAMESPACE::Add(v, vadd), v),
                                   d, out + i);
    }
    for (; i < n; ++i) {
        const hn_u8 c = in[i];
        out[i] = (c >= lo && c <= hi) ? static_cast<hn_u8>(c + add) : c;
    }
}

const char* TargetNameFn() { return hwy::TargetName(HWY_TARGET); }

}  // namespace HWY_NAMESPACE
}  // namespace dftracer::utils::dataframe
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace dftracer::utils::dataframe {

HWY_EXPORT(ClassBits);
HWY_EXPORT(CaseMap);
HWY_EXPORT(ClassRows32);
HWY_EXPORT(ClassRows64);
HWY_EXPORT(TargetNameFn);
HWY_EXPORT(LenBytes32);
HWY_EXPORT(CharCounts32);
HWY_EXPORT(AsciiFold);

namespace strsimd {
namespace {

// The byte-at-a-time reference of class_bits: one bit per byte, the same
// definitions the vector kernels use.
void class_bits_scalar(BitKind kind, const std::uint8_t* p, std::size_t n,
                       std::size_t w0, std::size_t w1, std::uint8_t eq,
                       std::uint64_t* bad, std::uint64_t* aux) {
    for (std::size_t w = w0; w < w1; ++w) {
        std::uint64_t b = 0, a = 0;
        const std::size_t base = w * 64;
        for (std::size_t c = 0; c < 64 && base + c < n; ++c) {
            const std::size_t i = base + c;
            const std::uint8_t x = p[i];
            bool is_bad = false, is_aux = false;
            switch (kind) {
                case BitKind::Alnum:
                    is_bad = !(dstd::is_ascii_alpha(static_cast<char>(x)) ||
                               dstd::is_ascii_digit(static_cast<char>(x)));
                    break;
                case BitKind::Alpha:
                    is_bad = !dstd::is_ascii_alpha(static_cast<char>(x));
                    break;
                case BitKind::Digit:
                    is_bad = !dstd::is_ascii_digit(static_cast<char>(x));
                    break;
                case BitKind::Space:
                    is_bad = !dstd::is_ascii_space(static_cast<char>(x));
                    break;
                case BitKind::Lower:
                    is_bad = dstd::is_ascii_upper(static_cast<char>(x));
                    is_aux = dstd::is_ascii_lower(static_cast<char>(x));
                    break;
                case BitKind::Upper:
                    is_bad = dstd::is_ascii_lower(static_cast<char>(x));
                    is_aux = dstd::is_ascii_upper(static_cast<char>(x));
                    break;
                case BitKind::Title: {
                    const bool prev_cased =
                        i != 0 &&
                        dstd::is_ascii_alpha(static_cast<char>(p[i - 1]));
                    is_bad = (dstd::is_ascii_upper(static_cast<char>(x)) &&
                              prev_cased) ||
                             (dstd::is_ascii_lower(static_cast<char>(x)) &&
                              !prev_cased);
                    is_aux = dstd::is_ascii_alpha(static_cast<char>(x));
                    break;
                }
                case BitKind::Eq:
                    is_bad = x == eq;
                    break;
            }
            b |= std::uint64_t{is_bad} << c;
            a |= std::uint64_t{is_aux} << c;
        }
        bad[w] = b;
        if (aux) aux[w] = a;
    }
}

void case_map_scalar(CaseKind kind, const std::uint8_t* in, std::uint8_t* out,
                     std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t c = in[i];
        bool flip = false;
        switch (kind) {
            case CaseKind::Swapcase:
                flip = dstd::is_ascii_alpha(static_cast<char>(c));
                break;
            case CaseKind::Lower:
                flip = dstd::is_ascii_upper(static_cast<char>(c));
                break;
            case CaseKind::Title: {
                const bool prev_cased =
                    i != 0 &&
                    dstd::is_ascii_alpha(static_cast<char>(in[i - 1]));
                flip =
                    (dstd::is_ascii_upper(static_cast<char>(c)) &&
                     prev_cased) ||
                    (dstd::is_ascii_lower(static_cast<char>(c)) && !prev_cased);
                break;
            }
        }
        out[i] = flip ? static_cast<std::uint8_t>(c ^ 0x20) : c;
    }
}

std::atomic<int> g_force_scalar{-1};

}  // namespace

bool scalar_forced() {
    int v = g_force_scalar.load(std::memory_order_relaxed);
    if (v < 0) {
        v = common::Env::get("DFTRACER_UTILS_STRING_SCALAR") ? 1 : 0;
        g_force_scalar.store(v, std::memory_order_relaxed);
    }
    return v == 1;
}

void set_force_scalar(bool on) {
    g_force_scalar.store(on ? 1 : 0, std::memory_order_relaxed);
}

void class_bits(BitKind kind, const std::uint8_t* p, std::size_t n,
                std::size_t w0, std::size_t w1, std::uint8_t eq,
                std::uint64_t* bad, std::uint64_t* aux) {
    if (scalar_forced())
        class_bits_scalar(kind, p, n, w0, w1, eq, bad, aux);
    else
        HWY_DYNAMIC_DISPATCH(ClassBits)
    (static_cast<int>(kind), p, n, w0, w1, eq, bad, aux);
}

void case_map(CaseKind kind, const std::uint8_t* in, std::uint8_t* out,
              std::size_t n) {
    if (scalar_forced())
        case_map_scalar(kind, in, out, n);
    else
        HWY_DYNAMIC_DISPATCH(CaseMap)(static_cast<int>(kind), in, out, n);
}

namespace {

std::atomic<int> g_pred_strategy{-1};

// The byte-at-a-time reference of class_rows: the same judgement per byte as
// class_bits_scalar, one row at a time.
template <class Off>
void class_rows_scalar(BitKind kind, const std::uint8_t* data, const Off* off,
                       std::int64_t lo, std::int64_t r0, std::int64_t r1,
                       std::uint8_t* res) {
    const std::uint8_t* p = data + lo;
    for (std::int64_t i = r0; i < r1; ++i) {
        const std::size_t s = static_cast<std::size_t>(off[i] - lo);
        const std::size_t t = static_cast<std::size_t>(off[i + 1] - lo);
        if (s >= t) continue;
        bool bad_any = false, aux_any = false;
        for (std::size_t j = s; j < t; ++j) {
            const std::uint8_t x = p[j];
            switch (kind) {
                case BitKind::Alnum:
                    bad_any |= !(dstd::is_ascii_alpha(static_cast<char>(x)) ||
                                 dstd::is_ascii_digit(static_cast<char>(x)));
                    break;
                case BitKind::Alpha:
                    bad_any |= !dstd::is_ascii_alpha(static_cast<char>(x));
                    break;
                case BitKind::Digit:
                    bad_any |= !dstd::is_ascii_digit(static_cast<char>(x));
                    break;
                case BitKind::Space:
                    bad_any |= !dstd::is_ascii_space(static_cast<char>(x));
                    break;
                case BitKind::Lower:
                    bad_any |= dstd::is_ascii_upper(static_cast<char>(x));
                    aux_any |= dstd::is_ascii_lower(static_cast<char>(x));
                    break;
                case BitKind::Upper:
                    bad_any |= dstd::is_ascii_lower(static_cast<char>(x));
                    aux_any |= dstd::is_ascii_upper(static_cast<char>(x));
                    break;
                case BitKind::Title: {
                    const bool prev_cased =
                        j != s &&
                        dstd::is_ascii_alpha(static_cast<char>(p[j - 1]));
                    bad_any |= (dstd::is_ascii_upper(static_cast<char>(x)) &&
                                prev_cased) ||
                               (dstd::is_ascii_lower(static_cast<char>(x)) &&
                                !prev_cased);
                    aux_any |= dstd::is_ascii_alpha(static_cast<char>(x));
                    break;
                }
                case BitKind::Eq:
                    break;
            }
        }
        const bool has_aux = kind == BitKind::Lower || kind == BitKind::Upper ||
                             kind == BitKind::Title;
        if (!bad_any && (!has_aux || aux_any))
            res[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    }
}

}  // namespace

PredStrategy pred_strategy() {
    int v = g_pred_strategy.load(std::memory_order_relaxed);
    if (v < 0) {
        const auto e = common::Env::get("DFTRACER_UTILS_STRING_PREDICATE");
        v = !e ? 0 : *e == "bits" ? 1 : *e == "rows" ? 2 : 0;
        g_pred_strategy.store(v, std::memory_order_relaxed);
    }
    return static_cast<PredStrategy>(v);
}

void set_pred_strategy(PredStrategy s) {
    g_pred_strategy.store(static_cast<int>(s), std::memory_order_relaxed);
}

void class_rows(BitKind kind, const std::uint8_t* data, const std::int32_t* off,
                std::int64_t lo, std::int64_t r0, std::int64_t r1,
                std::uint8_t* res) {
    if (scalar_forced())
        class_rows_scalar(kind, data, off, lo, r0, r1, res);
    else
        HWY_DYNAMIC_DISPATCH(ClassRows32)
    (static_cast<int>(kind), data, off, lo, r0, r1, res);
}

void class_rows(BitKind kind, const std::uint8_t* data, const std::int64_t* off,
                std::int64_t lo, std::int64_t r0, std::int64_t r1,
                std::uint8_t* res) {
    if (scalar_forced())
        class_rows_scalar(kind, data, off, lo, r0, r1, res);
    else
        HWY_DYNAMIC_DISPATCH(ClassRows64)
    (static_cast<int>(kind), data, off, lo, r0, r1, res);
}

const char* target_name() { return HWY_DYNAMIC_DISPATCH(TargetNameFn)(); }

void len_bytes(const std::int32_t* off, std::int64_t* out, std::size_t n) {
    HWY_DYNAMIC_DISPATCH(LenBytes32)(off, out, n);
}

void char_counts(const std::uint8_t* data, const std::int32_t* off,
                 std::int64_t n, std::int64_t* out) {
    // An empty column may have no offsets at all, as from an empty Arrow array.
    if (n <= 0) return;
    HWY_DYNAMIC_DISPATCH(CharCounts32)(data, off, n, out);
}

void ascii_fold(const std::uint8_t* in, std::uint8_t* out, std::size_t n,
                std::uint8_t lo, std::uint8_t hi, int add) {
    HWY_DYNAMIC_DISPATCH(AsciiFold)(in, out, n, lo, hi, add);
}

}  // namespace strsimd
}  // namespace dftracer::utils::dataframe
#endif  // HWY_ONCE
