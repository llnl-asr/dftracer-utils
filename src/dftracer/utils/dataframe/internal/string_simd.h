#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_STRING_SIMD_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_STRING_SIMD_H

#include <bit>
#include <cstddef>
#include <cstdint>

// SIMD primitives of the string kernels. They run over a whole data buffer
// (the bytes of every row end to end), not row by row, so a short string
// gains as much as a long one: a vector pass writes one bit per byte, and
// the per-row work is popcount / first-set / last-set over a bit range.
namespace dftracer::utils::dataframe::strsimd {

/// What a bit array of class_bits holds. `bad` is the first array, `aux` the
/// second (null where a kind has none).
///   Alnum, Alpha, Digit, Space: bad = the byte is not in the class
///   Lower: bad = upper, aux = lower      Upper: bad = lower, aux = upper
///   Title: bad = breaks Python istitle given the previous byte, aux = letter
///   Eq:    bad = the byte equals `eq`
enum class BitKind : int {
    Alnum = 0,
    Alpha = 1,
    Digit = 2,
    Space = 3,
    Lower = 4,
    Upper = 5,
    Title = 6,
    Eq = 7
};

/// The length-preserving ASCII case maps of a byte buffer. Title reads the
/// previous byte (the first byte has none): a caller fixes each row's first
/// byte itself.
enum class CaseKind : int { Swapcase = 0, Title = 1, Lower = 2 };

/// Bits of bytes [w0 * 64, min(n, w1 * 64)) of `p` into word w0.. of `bad` /
/// `aux`; a bit past `n` is zero. Chunks of words are independent, so a caller
/// can run them on several threads.
void class_bits(BitKind kind, const std::uint8_t* p, std::size_t n,
                std::size_t w0, std::size_t w1, std::uint8_t eq,
                std::uint64_t* bad, std::uint64_t* aux);

/// Which scan the string predicates (isalpha, isdigit, ...) use. Bits: one
/// vector pass over the whole data buffer writes a bit per byte (class_bits);
/// each row then reads its bit range, so short strings gain as much as long
/// ones. Rows: each string is scanned vector by vector and stops at its first
/// bad vector (class_rows), which wins when strings are long and a bad byte
/// comes early. Auto picks per column from the mean string length
/// (PRED_ROWS_MIN_AVG). Read once from DFTRACER_UTILS_STRING_PREDICATE
/// ("bits" or "rows"; anything else is Auto); set_pred_strategy overrides it.
/// A test and benchmark hook: both scans give the same result.
enum class PredStrategy : int { Auto = 0, Bits = 1, Rows = 2 };
PredStrategy pred_strategy();
void set_pred_strategy(PredStrategy s);

/// The mean string length (bytes per row) from which Auto scans row by row.
/// Measured (change string-expression-speed): at a mean of 8 and 16 bytes the
/// bit mask is about 2x faster, from 32 up the row scan is 1.2x to 4x faster,
/// on random and on all-good strings alike.
constexpr std::int64_t PRED_ROWS_MIN_AVG = 24;

/// Rows [r0, r1) of a flat String (int32 offsets) or LargeString (int64
/// offsets) column whose bytes start at data + lo: sets bit i of `res` when row
/// i is non-empty, holds no bad byte and, for Lower, Upper and Title, holds a
/// cased byte. `kind` is any BitKind but Eq. A caller splits rows into ranges
/// that start on a multiple of 8 so ranges write distinct bytes of `res`.
void class_rows(BitKind kind, const std::uint8_t* data, const std::int32_t* off,
                std::int64_t lo, std::int64_t r0, std::int64_t r1,
                std::uint8_t* res);
void class_rows(BitKind kind, const std::uint8_t* data, const std::int64_t* off,
                std::int64_t lo, std::int64_t r0, std::int64_t r1,
                std::uint8_t* res);

/// out[i] = the case map of in[i] for i in [0, n); `in` and `out` may alias.
void case_map(CaseKind kind, const std::uint8_t* in, std::uint8_t* out,
              std::size_t n);

/// Name of the Highway target the dispatcher chose (for the benchmark).
const char* target_name();

/// Run the byte-at-a-time reference instead of the vector kernels. Read once
/// from the environment variable DFTRACER_UTILS_STRING_SCALAR (any value);
/// set_force_scalar overrides it. A test and benchmark hook, never a fallback.
bool scalar_forced();
void set_force_scalar(bool on);

constexpr std::size_t NPOS = ~std::size_t{0};

/// Any bit set in [s, e)?
inline bool bits_any(const std::uint64_t* b, std::size_t s, std::size_t e) {
    if (s >= e) return false;
    const std::size_t ws = s >> 6, we = (e - 1) >> 6;
    const std::uint64_t first = ~std::uint64_t{0} << (s & 63);
    const std::uint64_t last = ~std::uint64_t{0} >> (63 - ((e - 1) & 63));
    if (ws == we) return (b[ws] & first & last) != 0;
    if (b[ws] & first) return true;
    for (std::size_t w = ws + 1; w < we; ++w)
        if (b[w]) return true;
    return (b[we] & last) != 0;
}

/// The first set bit in [s, e), or `e` when there is none.
inline std::size_t bits_next(const std::uint64_t* b, std::size_t s,
                             std::size_t e) {
    if (s >= e) return e;
    std::size_t w = s >> 6;
    const std::size_t we = (e - 1) >> 6;
    std::uint64_t word = b[w] & (~std::uint64_t{0} << (s & 63));
    for (;;) {
        if (word) {
            const std::size_t i =
                (w << 6) + static_cast<std::size_t>(std::countr_zero(word));
            return i < e ? i : e;
        }
        if (++w > we) return e;
        word = b[w];
    }
}

/// The last set bit in [s, e), or NPOS when there is none.
inline std::size_t bits_prev(const std::uint64_t* b, std::size_t s,
                             std::size_t e) {
    if (s >= e) return NPOS;
    std::size_t w = (e - 1) >> 6;
    const std::size_t ws = s >> 6;
    std::uint64_t word = b[w] & (~std::uint64_t{0} >> (63 - ((e - 1) & 63)));
    for (;;) {
        if (word) {
            const std::size_t i =
                (w << 6) + 63 -
                static_cast<std::size_t>(std::countl_zero(word));
            return i >= s ? i : NPOS;
        }
        if (w <= ws) return NPOS;
        word = b[--w];
    }
}

}  // namespace dftracer::utils::dataframe::strsimd

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_STRING_SIMD_H
