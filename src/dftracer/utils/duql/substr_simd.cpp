#include <dftracer/utils/duql/substr_simd.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "dftracer/utils/duql/substr_simd.cpp"
#include <hwy/foreach_target.h>  // must precede highway.h
#include <hwy/highway.h>

HWY_BEFORE_NAMESPACE();
namespace dftracer::utils::duql::detail {
namespace HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// Needles at or below this length take the SIMD first-byte scan; longer ones
// fall back to a scalar search (the per-candidate compare dominates, so the
// vector filter stops paying off). The trailing tail shorter than a vector is
// likewise scalar.
constexpr std::int64_t SUBSTR_SIMD_MAX_NEEDLE = 64;

inline std::uint8_t fold(std::uint8_t c) {
    return c >= 'A' && c <= 'Z' ? static_cast<std::uint8_t>(c | 0x20) : c;
}

// Whether the `m` bytes at `h` equal `needle`, ASCII letters folded when
// `icase` (the needle is already lowercase then).
template <bool ICASE>
bool same(const std::uint8_t* h, const char* needle, std::int64_t m) {
    if constexpr (!ICASE) {
        return std::memcmp(h, needle, static_cast<std::size_t>(m)) == 0;
    } else {
        for (std::int64_t k = 0; k < m; ++k)
            if (fold(h[k]) != static_cast<std::uint8_t>(needle[k]))
                return false;
        return true;
    }
}

// First index in [0, hay_len - needle_len] whose needle_len bytes match, or
// -1. Callers handle the empty needle and needle-longer-than-haystack cases
// before dispatch.
template <bool ICASE>
std::int64_t Find(const char* hay, std::int64_t hay_len, const char* needle,
                  std::int64_t needle_len) {
    const std::uint8_t* hp = reinterpret_cast<const std::uint8_t*>(hay);
    const std::int64_t last = hay_len - needle_len;  // inclusive last start
    const std::int64_t m = needle_len;
    const auto head = static_cast<std::uint8_t>(needle[0]);
    auto scalar_from = [&](std::int64_t i) -> std::int64_t {
        for (; i <= last; ++i)
            if ((ICASE ? fold(hp[i]) : hp[i]) == head &&
                same<ICASE>(hp + i, needle, m))
                return i;
        return -1;
    };
    if (m > SUBSTR_SIMD_MAX_NEEDLE) return scalar_from(0);
    const hn::ScalableTag<std::uint8_t> d;
    const std::size_t lanes = hn::Lanes(d);
    const auto first = hn::Set(d, head);
    const auto upper_a = hn::Set(d, static_cast<std::uint8_t>('A'));
    const auto letters = hn::Set(d, static_cast<std::uint8_t>(26));
    const auto bit = hn::Set(d, static_cast<std::uint8_t>(0x20));
    std::int64_t i = 0;
    // The candidate offsets in a block are read from a 64-bit mask accumulator;
    // skip the vector loop when a target's u8 vector exceeds 64 lanes (some
    // scalable ISAs), leaving the scalar tail to cover every position.
    const bool vec_ok = lanes <= 64;
    // Load `lanes` bytes at a time; require i + lanes <= last + 1 so the load
    // stays in bounds (last + 1 <= hay_len for m >= 1) and every candidate has
    // a full needle after it.
    for (; vec_ok && i + static_cast<std::int64_t>(lanes) <= last + 1;
         i += static_cast<std::int64_t>(lanes)) {
        auto v = hn::LoadU(d, hp + i);
        if constexpr (ICASE) {
            // An uppercase ASCII letter is (v - 'A') < 26 unsigned.
            const auto upper = hn::Lt(hn::Sub(v, upper_a), letters);
            v = hn::IfThenElse(upper, hn::Or(v, bit), v);
        }
        const auto eq = hn::Eq(v, first);
        alignas(8) std::uint8_t mbytes[8] = {0};
        hn::StoreMaskBits(d, eq, mbytes);
        std::uint64_t bits;
        std::memcpy(&bits, mbytes, sizeof(bits));
        while (bits != 0) {
            const std::int64_t cand =
                i + static_cast<std::int64_t>(
                        hwy::Num0BitsBelowLS1Bit_Nonzero64(bits));
            if (same<ICASE>(hp + cand, needle, m)) return cand;
            bits &= bits - 1;
        }
    }
    return scalar_from(i);
}

std::int64_t SubstrFind(const char* hay, std::int64_t hay_len,
                        const char* needle, std::int64_t needle_len) {
    return Find<false>(hay, hay_len, needle, needle_len);
}

std::int64_t SubstrFindIcase(const char* hay, std::int64_t hay_len,
                             const char* needle, std::int64_t needle_len) {
    return Find<true>(hay, hay_len, needle, needle_len);
}

}  // namespace HWY_NAMESPACE
}  // namespace dftracer::utils::duql::detail
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace dftracer::utils::duql::detail {

HWY_EXPORT(SubstrFind);
HWY_EXPORT(SubstrFindIcase);

std::int64_t substr_find(const char* hay, std::int64_t hay_len,
                         const char* needle, std::int64_t needle_len) {
    if (needle_len <= 0)
        return 0;  // empty needle matches at 0 (find semantics)
    if (needle_len > hay_len) return -1;
    return HWY_DYNAMIC_DISPATCH(SubstrFind)(hay, hay_len, needle, needle_len);
}

std::int64_t substr_find_icase(const char* hay, std::int64_t hay_len,
                               const char* needle, std::int64_t needle_len) {
    if (needle_len <= 0) return 0;
    if (needle_len > hay_len) return -1;
    return HWY_DYNAMIC_DISPATCH(SubstrFindIcase)(hay, hay_len, needle,
                                                 needle_len);
}

}  // namespace dftracer::utils::duql::detail
#endif  // HWY_ONCE
