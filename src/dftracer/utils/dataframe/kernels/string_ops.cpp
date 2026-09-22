#include <dftracer/utils/core/common/hash/fnv1a.h>
#include <dftracer/utils/core/common/hash/hex64.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/substr_simd.h>
#include <dftracer/utils/dataframe/internal/varwidth_offsets.h>
#include <dftracer/utils/dataframe/kernels/string_ops.h>
#include <dftracer/utils/dataframe/parallel.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

// Highway static dispatch (baseline target): only the two
// naturally-vectorizable string ops use it - str_len_bytes (adjacent int32
// offset difference) and the ASCII case folds (byte compare + conditional add).
// Every other op is variable-length per row and is legitimately scalar.
#include <hwy/highway.h>
namespace hn = hwy::HWY_NAMESPACE;

using dftracer::utils::dataframe::substr_find;

namespace dftracer::utils::dataframe {

Series str_eq(const Series& v, std::string_view rhs) {
    return Series{dftu_series_str_eq(v.handle(), rhs.data(),
                                     static_cast<std::int32_t>(rhs.size()))};
}
Series str_contains(const Series& v, std::string_view needle) {
    return Series{dftu_series_str_contains(
        v.handle(), needle.data(), static_cast<std::int32_t>(needle.size()))};
}
Series str_starts_with(const Series& v, std::string_view prefix) {
    return Series{dftu_series_str_starts_with(
        v.handle(), prefix.data(), static_cast<std::int32_t>(prefix.size()))};
}

}  // namespace dftracer::utils::dataframe

namespace {

using dftracer::utils::dataframe::Buffer;
using dftracer::utils::dataframe::buffer_bytes;
using dftracer::utils::dataframe::Encoding;
using dftracer::utils::dataframe::is_wide_offset_type;
using dftracer::utils::dataframe::narrow_varwidth_type;
using dftracer::utils::dataframe::offsets_of;
using dftracer::utils::dataframe::parallel_backend_installed;
using dftracer::utils::dataframe::parallel_for;
using dftracer::utils::dataframe::TypeId;

// String or Binary, at either offset width (narrow_varwidth_type folds the
// Large variants into the same case).
bool is_string_kind(TypeId t) {
    const TypeId n = narrow_varwidth_type(t);
    return n == TypeId::String || n == TypeId::Binary;
}

// Row `i` of a FLAT String/Binary(/Large) column `c`, reading its offsets at
// width `Off` - int32_t for String/Binary, int64_t for LargeString/
// LargeBinary. Callers pick `Off` once (via is_wide_offset_type) and
// instantiate the whole hot loop at that width, never branching per row.
template <class Off>
std::string_view value_at(const dftu_series& c, std::int64_t i) {
    const Off* off = reinterpret_cast<const Off*>(offsets_of<Off>(c)->data());
    const char* data = reinterpret_cast<const char*>(c.data->data());
    return std::string_view(data + off[i],
                            static_cast<std::size_t>(off[i + 1] - off[i]));
}

// Row grain for the parallel FLAT predicate loop: a multiple of 8 so every
// chunk boundary (except the very last) falls on a byte boundary of the
// bit-packed output, giving disjoint bytes per task with no atomics needed.
constexpr std::int64_t STRING_PREDICATE_GRAIN = 1 << 15;

// Apply a string predicate at offset width `Off`, returning a Bool column. On
// a DICTIONARY input (whose dictionary values share `v`'s offset width) the
// predicate is evaluated once per dictionary entry, then codes are mapped.
// Every predicate fans out through the parallel_for seam on the FLAT path
// past one grain: a byte compare over 10M short strings runs at a few GB/s
// on one core, far under what the memory system gives a pool.
template <class Off, class Pred>
dftu_series* string_predicate_w(const dftu_series* v, Pred pred) {
    auto* out = new dftu_series();
    out->type = TypeId::Bool;
    out->encoding = Encoding::Flat;
    out->length = v->length;
    out->null_count = v->null_count;
    out->validity = v->validity;
    std::size_t bytes = buffer_bytes(TypeId::Bool, v->length);
    out->data = Buffer::allocate(bytes);
    std::memset(out->data->data(), 0, bytes);
    std::uint8_t* bits = out->data->data();
    auto set = [&](std::int64_t i) {
        bits[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    };

    if (v->encoding == Encoding::Flat) {
        if (v->length > STRING_PREDICATE_GRAIN &&
            parallel_backend_installed()) {
            parallel_for(v->length, STRING_PREDICATE_GRAIN,
                         [&](std::int64_t b, std::int64_t e) {
                             for (std::int64_t i = b; i < e; ++i)
                                 if (pred(value_at<Off>(*v, i))) set(i);
                         });
        } else {
            for (std::int64_t i = 0; i < v->length; ++i)
                if (pred(value_at<Off>(*v, i))) set(i);
        }
    } else if (v->encoding == Encoding::Dictionary && v->child) {
        const dftu_series& dict = *v->child;
        std::vector<char> hit(static_cast<std::size_t>(dict.length));
        for (std::int64_t k = 0; k < dict.length; ++k)
            hit[static_cast<std::size_t>(k)] =
                pred(value_at<Off>(dict, k)) ? 1 : 0;
        const std::int32_t* codes =
            reinterpret_cast<const std::int32_t*>(v->data->data());
        for (std::int64_t i = 0; i < v->length; ++i)
            if (hit[static_cast<std::size_t>(codes[i])]) set(i);
    } else {
        delete out;
        return nullptr;
    }
    return out;
}

template <class Pred>
dftu_series* string_predicate(const dftu_series* v, Pred pred) {
    if (!is_string_kind(v->type)) return nullptr;
    return is_wide_offset_type(v->type)
               ? string_predicate_w<std::int64_t>(v, pred)
               : string_predicate_w<std::int32_t>(v, pred);
}

// Per-row byte reader over a String/Binary(/Large) column that transparently
// resolves a DICTIONARY input (row i -> dictionary entry codes[i]). Reads its
// offsets at width `Off`. Nullness comes from the top-level validity bitmap
// in both encodings.
template <class Off>
class RowReader {
   public:
    explicit RowReader(const dftu_series* v) : v_(v) {
        if (!is_string_kind(v->type)) return;
        if (v->encoding == Encoding::Flat && offsets_of<Off>(*v) && v->data) {
            off_ = reinterpret_cast<const Off*>(offsets_of<Off>(*v)->data());
            data_ = reinterpret_cast<const char*>(v->data->data());
            ok_ = true;
        } else if (v->encoding == Encoding::Dictionary && v->child &&
                   offsets_of<Off>(*v->child) && v->child->data && v->data) {
            off_ = reinterpret_cast<const Off*>(
                offsets_of<Off>(*v->child)->data());
            data_ = reinterpret_cast<const char*>(v->child->data->data());
            codes_ = reinterpret_cast<const std::int32_t*>(v->data->data());
            ok_ = true;
        }
    }
    bool ok() const { return ok_; }
    bool is_null(std::int64_t i) const {
        if (!v_->validity) return false;
        const std::uint8_t* bm = v_->validity->data();
        return ((bm[i >> 3] >> (i & 7)) & 1) == 0;
    }
    std::string_view at(std::int64_t i) const {
        std::int64_t k = codes_ ? codes_[i] : i;
        return std::string_view(
            data_ + off_[k], static_cast<std::size_t>(off_[k + 1] - off_[k]));
    }

   private:
    const dftu_series* v_;
    const Off* off_ = nullptr;
    const char* data_ = nullptr;
    const std::int32_t* codes_ = nullptr;
    bool ok_ = false;
};

// Int64 output column with the same length/validity as `v` (nulls preserved).
dftu_series* make_i64(const dftu_series* v,
                      const std::vector<std::int64_t>& vals) {
    auto* out = new dftu_series();
    out->type = TypeId::Int64;
    out->encoding = Encoding::Flat;
    out->length = v->length;
    out->null_count = v->null_count;
    out->validity = v->validity;
    std::size_t bytes = buffer_bytes(TypeId::Int64, v->length);
    out->data = Buffer::allocate(bytes);
    if (bytes != 0) std::memcpy(out->data->data(), vals.data(), bytes);
    return out;
}

// UInt64 output column with the same length as `v`. `validity` is a fresh
// Arrow-layout bitmap the caller owns, or NULL to share `v`'s nulls.
dftu_series* make_u64(const dftu_series* v,
                      const std::vector<std::uint64_t>& vals,
                      const std::uint8_t* validity) {
    auto* out = new dftu_series();
    out->type = TypeId::Uint64;
    out->encoding = Encoding::Flat;
    out->length = v->length;
    std::size_t bytes = buffer_bytes(TypeId::Uint64, v->length);
    out->data = Buffer::allocate(bytes);
    if (bytes != 0) std::memcpy(out->data->data(), vals.data(), bytes);
    if (validity) {
        std::size_t vb = static_cast<std::size_t>((v->length + 7) / 8);
        out->validity = Buffer::allocate(vb);
        if (vb != 0) std::memcpy(out->validity->data(), validity, vb);
    } else {
        out->null_count = v->null_count;
        out->validity = v->validity;
    }
    return out;
}

// Flat String output column from one string per row (size == v->length); shares
// v's validity so null rows stay null (their bytes are ignored). Always
// narrow (int32 offsets): a derived per-row transform's total size is its own
// fresh sizing question, not a width the source column forces on it.
dftu_series* make_string(const dftu_series* v,
                         const std::vector<std::string>& parts) {
    auto* out = new dftu_series();
    out->type = TypeId::String;
    out->encoding = Encoding::Flat;
    out->length = v->length;
    out->null_count = v->null_count;
    out->validity = v->validity;
    std::size_t off_bytes =
        static_cast<std::size_t>(v->length + 1) * sizeof(std::int32_t);
    out->offsets = Buffer::allocate(off_bytes);
    std::size_t total = 0;
    for (const std::string& p : parts) total += p.size();
    out->data = Buffer::allocate(total);
    std::int32_t* od = reinterpret_cast<std::int32_t*>(out->offsets->data());
    char* bd =
        total != 0 ? reinterpret_cast<char*>(out->data->data()) : nullptr;
    std::int32_t pos = 0;
    od[0] = 0;
    for (std::int64_t i = 0; i < v->length; ++i) {
        const std::string& p = parts[static_cast<std::size_t>(i)];
        if (!p.empty()) std::memcpy(bd + pos, p.data(), p.size());
        pos += static_cast<std::int32_t>(p.size());
        od[i + 1] = pos;
    }
    return out;
}

// ---- SIMD helpers --------------------------------------------------------

// out[i] = off[i+1] - off[i], widened to int64. Highway over the int32 offsets.
void len_bytes_simd(const std::int32_t* off, std::int64_t* out, std::size_t n) {
    const hn::ScalableTag<std::int64_t> d64;
    const hn::Rebind<std::int32_t, decltype(d64)> d32;
    const std::size_t lanes = hn::Lanes(d64);
    std::size_t i = 0;
    for (; i + lanes <= n; i += lanes) {
        const auto a = hn::LoadU(d32, off + i + 1);
        const auto b = hn::LoadU(d32, off + i);
        hn::StoreU(hn::PromoteTo(d64, hn::Sub(a, b)), d64, out + i);
    }
    for (; i < n; ++i) out[i] = static_cast<std::int64_t>(off[i + 1] - off[i]);
}

// UTF-8 character count of a byte range: every byte that is not a continuation
// byte ((c & 0xC0) != 0x80) starts a character. Highway compares a block and
// folds the mask with CountTrue.
std::int64_t count_char_starts(const std::uint8_t* p, std::size_t len) {
    const hn::ScalableTag<std::uint8_t> d;
    const auto vc0 = hn::Set(d, 0xC0);
    const auto v80 = hn::Set(d, 0x80);
    const std::size_t lanes = hn::Lanes(d);
    std::int64_t count = 0;
    std::size_t i = 0;
    for (; i + lanes <= len; i += lanes) {
        const auto m = hn::Ne(hn::And(hn::LoadU(d, p + i), vc0), v80);
        count += static_cast<std::int64_t>(hn::CountTrue(d, m));
    }
    for (; i < len; ++i)
        if ((p[i] & 0xC0) != 0x80) ++count;
    return count;
}

// ASCII case fold of a byte buffer: `add` is +32 (upper->lower) or -32
// (lower->upper); `lo`/`hi` bound the source case. Non-matching bytes pass
// through unchanged (ASCII-only fold).
void ascii_fold_simd(const std::uint8_t* in, std::uint8_t* out, std::size_t n,
                     std::uint8_t lo, std::uint8_t hi, int add) {
    const hn::ScalableTag<std::uint8_t> d;
    const auto vlo = hn::Set(d, lo);
    const auto vhi = hn::Set(d, hi);
    const auto vadd = hn::Set(d, static_cast<std::uint8_t>(add));
    const std::size_t lanes = hn::Lanes(d);
    std::size_t i = 0;
    for (; i + lanes <= n; i += lanes) {
        const auto v = hn::LoadU(d, in + i);
        const auto m = hn::And(hn::Ge(v, vlo), hn::Le(v, vhi));
        hn::StoreU(hn::IfThenElse(m, hn::Add(v, vadd), v), d, out + i);
    }
    for (; i < n; ++i) {
        const std::uint8_t c = in[i];
        out[i] = (c >= lo && c <= hi) ? static_cast<std::uint8_t>(c + add) : c;
    }
}

// Fold that keeps offsets/validity and rewrites only the (byte-length
// preserving) data buffer. Fast path SIMD for a FLAT input, preserving the
// source's own offset width and buffer (no copy of the offsets); a
// DICTIONARY input falls back through make_string (always narrow) per row.
template <class Off>
dftu_series* ascii_fold_w(const dftu_series* v, std::uint8_t lo,
                          std::uint8_t hi, int add) {
    RowReader<Off> r(v);
    if (!r.ok()) return nullptr;
    if (v->encoding == Encoding::Flat) {
        auto* out = new dftu_series();
        out->type = v->type;
        out->encoding = Encoding::Flat;
        out->length = v->length;
        out->null_count = v->null_count;
        out->validity = v->validity;
        offsets_of<Off>(*out) = offsets_of<Off>(*v);  // byte lengths unchanged
        const std::size_t data_len = v->data ? v->data->size() : 0;
        out->data = Buffer::allocate(data_len);
        if (data_len != 0)
            ascii_fold_simd(v->data->data(), out->data->data(), data_len, lo,
                            hi, add);
        return out;
    }
    std::vector<std::string> parts(static_cast<std::size_t>(v->length));
    for (std::int64_t i = 0; i < v->length; ++i) {
        if (r.is_null(i)) continue;
        std::string s(r.at(i));
        for (char& c : s)
            if (static_cast<std::uint8_t>(c) >= lo &&
                static_cast<std::uint8_t>(c) <= hi)
                c = static_cast<char>(c + add);
        parts[static_cast<std::size_t>(i)] = std::move(s);
    }
    return make_string(v, parts);
}

dftu_series* ascii_fold(const dftu_series* v, std::uint8_t lo, std::uint8_t hi,
                        int add) {
    return is_wide_offset_type(v->type)
               ? ascii_fold_w<std::int64_t>(v, lo, hi, add)
               : ascii_fold_w<std::int32_t>(v, lo, hi, add);
}

// Per-row string transform (scalar) at offset width `Off`; null rows pass
// through as null. Output is always narrow (see make_string).
template <class Off, class Fn>
dftu_series* string_transform_w(const dftu_series* v, Fn fn) {
    RowReader<Off> r(v);
    if (!r.ok()) return nullptr;
    std::vector<std::string> parts(static_cast<std::size_t>(v->length));
    for (std::int64_t i = 0; i < v->length; ++i) {
        if (r.is_null(i)) continue;
        parts[static_cast<std::size_t>(i)] = fn(r.at(i));
    }
    return make_string(v, parts);
}

template <class Fn>
dftu_series* string_transform(const dftu_series* v, Fn fn) {
    return is_wide_offset_type(v->type)
               ? string_transform_w<std::int64_t>(v, fn)
               : string_transform_w<std::int32_t>(v, fn);
}

// Per-row int64 transform (scalar) at offset width `Off`; null rows keep the
// shared validity.
template <class Off, class Fn>
dftu_series* int_transform_w(const dftu_series* v, Fn fn) {
    RowReader<Off> r(v);
    if (!r.ok()) return nullptr;
    std::vector<std::int64_t> vals(static_cast<std::size_t>(v->length), 0);
    for (std::int64_t i = 0; i < v->length; ++i) {
        if (r.is_null(i)) continue;
        vals[static_cast<std::size_t>(i)] = fn(r.at(i));
    }
    return make_i64(v, vals);
}

template <class Fn>
dftu_series* int_transform(const dftu_series* v, Fn fn) {
    return is_wide_offset_type(v->type) ? int_transform_w<std::int64_t>(v, fn)
                                        : int_transform_w<std::int32_t>(v, fn);
}

bool is_ascii_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

std::string_view lstrip_ws(std::string_view s) {
    std::size_t b = 0;
    while (b < s.size() && is_ascii_ws(s[b])) ++b;
    return s.substr(b);
}
std::string_view rstrip_ws(std::string_view s) {
    std::size_t e = s.size();
    while (e > 0 && is_ascii_ws(s[e - 1])) --e;
    return s.substr(0, e);
}

std::string replace_literal(std::string_view s, std::string_view pat,
                            std::string_view repl, bool all) {
    // An empty pattern has no well-defined occurrence; leave the row unchanged
    // (also avoids a zero-advance loop in the all-occurrences case).
    if (pat.empty()) return std::string(s);
    std::string out;
    std::size_t pos = 0;
    while (true) {
        std::size_t hit = s.find(pat, pos);
        if (hit == std::string_view::npos) break;
        out.append(s.substr(pos, hit - pos));
        out.append(repl);
        pos = hit + pat.size();
        if (!all) break;
    }
    out.append(s.substr(pos));
    return out;
}

std::string slice_bytes(std::string_view s, std::int64_t start,
                        std::int64_t length) {
    const std::int64_t len = static_cast<std::int64_t>(s.size());
    std::int64_t begin = start;
    if (begin < 0) begin += len;
    if (begin < 0) begin = 0;
    if (begin > len) begin = len;
    // A negative length means "to the end" (polars slice with no length).
    std::int64_t end = length < 0 ? len : begin + length;
    if (end < begin) end = begin;
    if (end > len) end = len;
    return std::string(s.substr(static_cast<std::size_t>(begin),
                                static_cast<std::size_t>(end - begin)));
}

std::string pad(std::string_view s, std::int64_t width, char fill, bool left) {
    const std::int64_t len = static_cast<std::int64_t>(s.size());
    if (len >= width) return std::string(s);
    std::string fillstr(static_cast<std::size_t>(width - len), fill);
    if (left) return fillstr + std::string(s);
    return std::string(s) + fillstr;
}

std::string zfill(std::string_view s, std::int64_t width) {
    const std::int64_t len = static_cast<std::int64_t>(s.size());
    if (len >= width) return std::string(s);
    const std::size_t pad_n = static_cast<std::size_t>(width - len);
    // A leading sign stays first; zeros are inserted after it (Python zfill).
    if (!s.empty() && (s[0] == '+' || s[0] == '-'))
        return std::string(1, s[0]) + std::string(pad_n, '0') +
               std::string(s.substr(1));
    return std::string(pad_n, '0') + std::string(s);
}

// ---- LIKE / glob matching ------------------------------------------------

// One classified pattern token: a literal byte, `%` (any run), or `_` (any one
// character). `\` escapes the following byte to a literal in the source
// pattern; the classification is done once, then reused for every row.
enum class GlobKind : std::uint8_t { Literal, AnyRun, AnyOne };
struct GlobToken {
    GlobKind kind;
    char ch;  // valid when kind == Literal
};

std::vector<GlobToken> classify_like(std::string_view pattern) {
    std::vector<GlobToken> toks;
    toks.reserve(pattern.size());
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        char c = pattern[i];
        if (c == '\\' && i + 1 < pattern.size()) {
            toks.push_back({GlobKind::Literal, pattern[++i]});
        } else if (c == '%') {
            toks.push_back({GlobKind::AnyRun, 0});
        } else if (c == '_') {
            toks.push_back({GlobKind::AnyOne, 0});
        } else {
            toks.push_back({GlobKind::Literal, c});
        }
    }
    return toks;
}

// Classic two-pointer wildcard match (no backtracking blowup): `%` == `*`
// (matches any run, including empty), `_` == `?` (matches exactly one byte).
bool glob_match(const std::vector<GlobToken>& toks, std::string_view s) {
    std::size_t si = 0, pi = 0;
    std::size_t star = std::string_view::npos;  // last AnyRun token index
    std::size_t star_si = 0;                    // s position when star seen
    const std::size_t n = s.size();
    const std::size_t m = toks.size();
    while (si < n) {
        if (pi < m && toks[pi].kind == GlobKind::AnyOne) {
            ++si;
            ++pi;
        } else if (pi < m && toks[pi].kind == GlobKind::Literal &&
                   toks[pi].ch == s[si]) {
            ++si;
            ++pi;
        } else if (pi < m && toks[pi].kind == GlobKind::AnyRun) {
            star = pi;
            star_si = si;
            ++pi;
        } else if (star != std::string_view::npos) {
            pi = star + 1;
            ++star_si;
            si = star_si;
        } else {
            return false;
        }
    }
    while (pi < m && toks[pi].kind == GlobKind::AnyRun) ++pi;
    return pi == m;
}

}  // namespace

dftu_series* dftu_series_str_eq(const dftu_series* v, const char* rhs,
                                int32_t rhs_len) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_str_eq(flat_v, rhs, rhs_len));

    std::string_view r(rhs, static_cast<std::size_t>(rhs_len));
    return string_predicate(v, [r](std::string_view s) { return s == r; });
}

dftu_series* dftu_series_str_contains(const dftu_series* v, const char* needle,
                                      int32_t needle_len) {
    DFTU_FLAT_OPERAND(v, flat_v,
                      dftu_series_str_contains(flat_v, needle, needle_len));

    std::string_view n(needle, static_cast<std::size_t>(needle_len));
    return string_predicate(v, [n](std::string_view s) {
        return substr_find(s.data(), static_cast<std::int64_t>(s.size()),
                           n.data(), static_cast<std::int64_t>(n.size())) >= 0;
    });
}

dftu_series* dftu_series_str_starts_with(const dftu_series* v,
                                         const char* prefix,
                                         int32_t prefix_len) {
    DFTU_FLAT_OPERAND(v, flat_v,
                      dftu_series_str_starts_with(flat_v, prefix, prefix_len));

    std::string_view p(prefix, static_cast<std::size_t>(prefix_len));
    return string_predicate(
        v, [p](std::string_view s) { return s.starts_with(p); });
}

dftu_series* dftu_series_str_ends_with(const dftu_series* v, const char* suffix,
                                       int32_t suffix_len) {
    DFTU_FLAT_OPERAND(v, flat_v,
                      dftu_series_str_ends_with(flat_v, suffix, suffix_len));

    std::string_view p(suffix, static_cast<std::size_t>(suffix_len));
    return string_predicate(v,
                            [p](std::string_view s) { return s.ends_with(p); });
}

dftu_series* dftu_series_str_matches(const dftu_series* v, const char* pattern,
                                     int32_t pattern_len) {
    DFTU_FLAT_OPERAND(v, flat_v,
                      dftu_series_str_matches(flat_v, pattern, pattern_len));

    std::regex re;
    try {
        re.assign(std::string(pattern, static_cast<std::size_t>(pattern_len)),
                  std::regex::ECMAScript);
    } catch (const std::regex_error&) {
        return nullptr;
    }
    return string_predicate(v, [&re](std::string_view s) {
        return std::regex_match(s.begin(), s.end(), re);
    });
}

namespace {

// The capture `group` of the first `re` match in each row, null where the row
// is null or does not match: a String column with its own validity.
template <class Off>
dftu_series* str_extract_w(const dftu_series* v, const std::regex& re,
                           std::size_t group) {
    RowReader<Off> r(v);
    if (!r.ok()) return nullptr;
    const std::size_t n = static_cast<std::size_t>(v->length);
    std::vector<std::int32_t> offsets(n + 1, 0);
    std::string data;
    std::vector<std::uint8_t> valid((n + 7) / 8, 0);
    bool any_null = false;
    for (std::size_t i = 0; i < n; ++i) {
        bool hit = false;
        if (!r.is_null(static_cast<std::int64_t>(i))) {
            const std::string_view s = r.at(static_cast<std::int64_t>(i));
            std::match_results<std::string_view::const_iterator> m;
            if (std::regex_search(s.begin(), s.end(), m, re) &&
                group < m.size() && m[group].matched) {
                data.append(m[group].first, m[group].second);
                hit = true;
            }
        }
        if (hit)
            valid[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        else
            any_null = true;
        offsets[i + 1] = static_cast<std::int32_t>(data.size());
    }
    return dftu_series_new_string(DFTU_TYPE_STRING, offsets.data(), data.data(),
                                  static_cast<int64_t>(n),
                                  any_null ? valid.data() : nullptr);
}

}  // namespace

dftu_series* dftu_series_str_extract(const dftu_series* v, const char* pattern,
                                     int32_t pattern_len, int64_t group) {
    if (!v || !pattern || group < 0) return nullptr;
    DFTU_FLAT_OPERAND(
        v, flat_v,
        dftu_series_str_extract(flat_v, pattern, pattern_len, group));
    std::regex re;
    try {
        re.assign(std::string(pattern, static_cast<std::size_t>(pattern_len)),
                  std::regex::ECMAScript);
    } catch (const std::regex_error&) {
        return nullptr;
    }
    const auto g = static_cast<std::size_t>(group);
    return is_wide_offset_type(v->type) ? str_extract_w<std::int64_t>(v, re, g)
                                        : str_extract_w<std::int32_t>(v, re, g);
}

dftu_series* dftu_series_str_search(const dftu_series* v, const char* pattern,
                                    int32_t pattern_len) {
    DFTU_FLAT_OPERAND(v, flat_v,
                      dftu_series_str_search(flat_v, pattern, pattern_len));

    std::regex re;
    try {
        re.assign(std::string(pattern, static_cast<std::size_t>(pattern_len)),
                  std::regex::ECMAScript);
    } catch (const std::regex_error&) {
        return nullptr;
    }
    return string_predicate(v, [&re](std::string_view s) {
        return std::regex_search(s.begin(), s.end(), re);
    });
}

dftu_series* dftu_series_str_like(const dftu_series* v, const char* pattern,
                                  int32_t pattern_len) {
    DFTU_FLAT_OPERAND(v, flat_v,
                      dftu_series_str_like(flat_v, pattern, pattern_len));

    std::string_view pat(pattern, static_cast<std::size_t>(pattern_len));
    std::vector<GlobToken> toks = classify_like(pat);

    // Classify once, then dispatch to the cheapest matcher. A run of AnyRun/
    // AnyOne only through the affixes hits equality / affix / substring; any
    // interior wildcard falls to the general two-pointer glob.
    std::size_t n_run = 0, n_one = 0;
    for (const GlobToken& t : toks) {
        if (t.kind == GlobKind::AnyRun) ++n_run;
        if (t.kind == GlobKind::AnyOne) ++n_one;
    }

    auto literal_of = [&](std::size_t begin, std::size_t end) {
        std::string out;
        for (std::size_t i = begin; i < end; ++i) out.push_back(toks[i].ch);
        return out;
    };

    if (n_one == 0 && n_run == 0) {
        std::string lit = literal_of(0, toks.size());
        return string_predicate(v,
                                [lit](std::string_view s) { return s == lit; });
    }
    if (n_one == 0 && n_run == 1) {
        const bool lead =
            !toks.empty() && toks.front().kind == GlobKind::AnyRun;
        const bool trail =
            !toks.empty() && toks.back().kind == GlobKind::AnyRun;
        if (trail && !lead) {  // "text%" -> starts_with
            std::string pre = literal_of(0, toks.size() - 1);
            return string_predicate(
                v, [pre](std::string_view s) { return s.starts_with(pre); });
        }
        if (lead && !trail) {  // "%text" -> ends_with
            std::string suf = literal_of(1, toks.size());
            return string_predicate(
                v, [suf](std::string_view s) { return s.ends_with(suf); });
        }
    }
    if (n_one == 0 && n_run == 2 && toks.size() >= 2 &&
        toks.front().kind == GlobKind::AnyRun &&
        toks.back().kind == GlobKind::AnyRun) {  // "%text%" -> contains
        std::string mid = literal_of(1, toks.size() - 1);
        return string_predicate(v, [mid](std::string_view s) {
            return substr_find(s.data(), static_cast<std::int64_t>(s.size()),
                               mid.data(),
                               static_cast<std::int64_t>(mid.size())) >= 0;
        });
    }
    return string_predicate(v, [toks = std::move(toks)](std::string_view s) {
        return glob_match(toks, s);
    });
}

dftu_series* dftu_series_str_len_bytes(const dftu_series* v) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_str_len_bytes(flat_v));

    // FLAT, narrow offsets: adjacent int32 offset difference, vectorized.
    // Every other case (DICTIONARY, or a wide-offset FLAT column) is scalar;
    // a Large* column's row count is bounded by int32 either way, only its
    // byte offsets are not, so the SIMD int32 path stays narrow-only.
    if (v->encoding == Encoding::Flat && v->offsets) {
        std::vector<std::int64_t> vals(static_cast<std::size_t>(v->length), 0);
        if (v->length > 0)
            len_bytes_simd(
                reinterpret_cast<const std::int32_t*>(v->offsets->data()),
                vals.data(), static_cast<std::size_t>(v->length));
        // Null rows keep 0 length regardless; validity marks them null.
        return make_i64(v, vals);
    }
    return int_transform(v, [](std::string_view s) {
        return static_cast<std::int64_t>(s.size());
    });
}

dftu_series* dftu_series_str_len_chars(const dftu_series* v) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_str_len_chars(flat_v));

    // FLAT, narrow offsets: count non-continuation bytes per row with the
    // vectorized scan. DICTIONARY and wide-offset columns take the scalar
    // per-row path.
    if (v->encoding == Encoding::Flat && v->offsets && v->data) {
        const std::int32_t* off =
            reinterpret_cast<const std::int32_t*>(v->offsets->data());
        const std::uint8_t* data = v->data->data();
        std::vector<std::int64_t> vals(static_cast<std::size_t>(v->length), 0);
        for (std::int64_t i = 0; i < v->length; ++i)
            vals[static_cast<std::size_t>(i)] = count_char_starts(
                data + off[i], static_cast<std::size_t>(off[i + 1] - off[i]));
        return make_i64(v, vals);
    }
    return int_transform(v, [](std::string_view s) {
        std::int64_t count = 0;
        for (unsigned char c : s)
            if ((c & 0xC0) != 0x80) ++count;  // non-continuation byte
        return count;
    });
}

dftu_series* dftu_series_str_find(const dftu_series* v, const char* needle,
                                  int32_t needle_len) {
    DFTU_FLAT_OPERAND(v, flat_v,
                      dftu_series_str_find(flat_v, needle, needle_len));

    std::string_view n(needle, static_cast<std::size_t>(needle_len));
    return int_transform(v, [n](std::string_view s) {
        return substr_find(s.data(), static_cast<std::int64_t>(s.size()),
                           n.data(), static_cast<std::int64_t>(n.size()));
    });
}

namespace {
template <class Off>
dftu_series* fnv1a_w(const dftu_series* v) {
    RowReader<Off> r(v);
    if (!r.ok()) return nullptr;
    std::vector<std::uint64_t> vals(static_cast<std::size_t>(v->length), 0);
    for (std::int64_t i = 0; i < v->length; ++i) {
        if (r.is_null(i)) continue;
        vals[static_cast<std::size_t>(i)] =
            dftracer::utils::hash::fnv1a_hash(r.at(i));
    }
    return make_u64(v, vals, nullptr);
}
}  // namespace

dftu_series* dftu_series_fnv1a(const dftu_series* v) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_fnv1a(flat_v));

    return is_wide_offset_type(v->type) ? fnv1a_w<std::int64_t>(v)
                                        : fnv1a_w<std::int32_t>(v);
}

namespace {
template <class Off>
dftu_series* hex64_parse_w(const dftu_series* v) {
    RowReader<Off> r(v);
    if (!r.ok()) return nullptr;
    std::vector<std::uint64_t> vals(static_cast<std::size_t>(v->length), 0);
    std::size_t bitmap_bytes = static_cast<std::size_t>((v->length + 7) / 8);
    std::vector<std::uint8_t> valid(bitmap_bytes, 0);
    std::int64_t nulls = 0;
    for (std::int64_t i = 0; i < v->length; ++i) {
        std::optional<std::uint64_t> parsed;
        if (!r.is_null(i)) parsed = dftracer::utils::hash::parse_hex64(r.at(i));
        if (!parsed) {
            ++nulls;
            continue;
        }
        vals[static_cast<std::size_t>(i)] = *parsed;
        valid[static_cast<std::size_t>(i >> 3)] |=
            static_cast<std::uint8_t>(1u << (i & 7));
    }
    dftu_series* out = make_u64(v, vals, valid.data());
    out->null_count = nulls;
    return out;
}
}  // namespace

dftu_series* dftu_series_hex64_parse(const dftu_series* v) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_hex64_parse(flat_v));

    return is_wide_offset_type(v->type) ? hex64_parse_w<std::int64_t>(v)
                                        : hex64_parse_w<std::int32_t>(v);
}

dftu_series* dftu_series_hex64_format(const dftu_series* v) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_hex64_format(flat_v));

    using dftracer::utils::hash::format_hex64;
    using dftracer::utils::hash::HEX64_DIGITS;
    if (!v || (v->type != TypeId::Uint64 && v->type != TypeId::Int64)) {
        return nullptr;
    }
    if (v->encoding != Encoding::Flat || !v->data) return nullptr;

    const std::size_t n = static_cast<std::size_t>(v->length);
    const std::size_t total = n * HEX64_DIGITS;
    // Every row is exactly HEX64_DIGITS bytes, so the byte total is known up
    // front; refuse rather than overflow the int32 offsets Arrow uses here.
    if (total > static_cast<std::size_t>(INT32_MAX)) return nullptr;

    const std::uint64_t* src =
        reinterpret_cast<const std::uint64_t*>(v->data->data());
    auto* out = new dftu_series();
    out->type = TypeId::String;
    out->encoding = Encoding::Flat;
    out->length = v->length;
    out->null_count = v->null_count;
    out->validity = v->validity;
    out->offsets = Buffer::allocate((n + 1) * sizeof(std::int32_t));
    out->data = Buffer::allocate(total);
    std::int32_t* od = reinterpret_cast<std::int32_t*>(out->offsets->data());
    char* bd =
        total != 0 ? reinterpret_cast<char*>(out->data->data()) : nullptr;
    for (std::size_t i = 0; i < n; ++i) {
        od[i] = static_cast<std::int32_t>(i * HEX64_DIGITS);
        format_hex64(src[i], bd + i * HEX64_DIGITS);
    }
    od[n] = static_cast<std::int32_t>(total);
    return out;
}

dftu_series* dftu_series_to_lowercase(const dftu_series* v) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_to_lowercase(flat_v));

    return ascii_fold(v, 'A', 'Z', 32);
}

dftu_series* dftu_series_to_uppercase(const dftu_series* v) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_to_uppercase(flat_v));

    return ascii_fold(v, 'a', 'z', -32);
}

dftu_series* dftu_series_str_strip(const dftu_series* v) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_str_strip(flat_v));

    return string_transform(v, [](std::string_view s) {
        return std::string(rstrip_ws(lstrip_ws(s)));
    });
}
dftu_series* dftu_series_str_lstrip(const dftu_series* v) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_str_lstrip(flat_v));

    return string_transform(
        v, [](std::string_view s) { return std::string(lstrip_ws(s)); });
}
dftu_series* dftu_series_str_rstrip(const dftu_series* v) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_str_rstrip(flat_v));

    return string_transform(
        v, [](std::string_view s) { return std::string(rstrip_ws(s)); });
}

dftu_series* dftu_series_str_replace(const dftu_series* v, const char* pat,
                                     int32_t pat_len, const char* repl,
                                     int32_t repl_len) {
    DFTU_FLAT_OPERAND(
        v, flat_v,
        dftu_series_str_replace(flat_v, pat, pat_len, repl, repl_len));

    std::string_view p(pat, static_cast<std::size_t>(pat_len));
    std::string_view r(repl, static_cast<std::size_t>(repl_len));
    return string_transform(v, [p, r](std::string_view s) {
        return replace_literal(s, p, r, false);
    });
}
dftu_series* dftu_series_str_replace_all(const dftu_series* v, const char* pat,
                                         int32_t pat_len, const char* repl,
                                         int32_t repl_len) {
    DFTU_FLAT_OPERAND(
        v, flat_v,
        dftu_series_str_replace_all(flat_v, pat, pat_len, repl, repl_len));

    std::string_view p(pat, static_cast<std::size_t>(pat_len));
    std::string_view r(repl, static_cast<std::size_t>(repl_len));
    return string_transform(v, [p, r](std::string_view s) {
        return replace_literal(s, p, r, true);
    });
}

dftu_series* dftu_series_str_slice(const dftu_series* v, int64_t start,
                                   int64_t length) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_str_slice(flat_v, start, length));

    return string_transform(v, [start, length](std::string_view s) {
        return slice_bytes(s, start, length);
    });
}

dftu_series* dftu_series_str_pad_start(const dftu_series* v, int64_t width,
                                       char fill) {
    DFTU_FLAT_OPERAND(v, flat_v,
                      dftu_series_str_pad_start(flat_v, width, fill));

    return string_transform(v, [width, fill](std::string_view s) {
        return pad(s, width, fill, true);
    });
}
dftu_series* dftu_series_str_pad_end(const dftu_series* v, int64_t width,
                                     char fill) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_str_pad_end(flat_v, width, fill));

    return string_transform(v, [width, fill](std::string_view s) {
        return pad(s, width, fill, false);
    });
}
dftu_series* dftu_series_str_zfill(const dftu_series* v, int64_t width) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_str_zfill(flat_v, width));

    return string_transform(
        v, [width](std::string_view s) { return zfill(s, width); });
}

namespace {
// Splits every row of `v` on `s_sep`, producing a List<String> (always
// narrow, both levels: the per-row part count and total byte count are the
// source column's own data, already known to fit an int32 row count).
// A List<String> column over `v`'s rows (always narrow, both levels): row i
// holds the parts `fn(row)` yields, a null input row a null list.
template <class Off, class Fn>
dftu_series* list_transform_w(const dftu_series* v, Fn fn) {
    RowReader<Off> r(v);
    if (!r.ok()) return nullptr;

    std::vector<std::string> all_parts;
    std::vector<std::int32_t> list_off(static_cast<std::size_t>(v->length + 1),
                                       0);
    for (std::int64_t i = 0; i < v->length; ++i) {
        if (!r.is_null(i)) fn(r.at(i), all_parts);
        list_off[static_cast<std::size_t>(i + 1)] =
            static_cast<std::int32_t>(all_parts.size());
    }

    auto* child = new dftu_series();
    child->type = TypeId::String;
    child->encoding = Encoding::Flat;
    child->length = static_cast<std::int64_t>(all_parts.size());
    std::size_t coff_bytes =
        static_cast<std::size_t>(all_parts.size() + 1) * sizeof(std::int32_t);
    child->offsets = Buffer::allocate(coff_bytes);
    std::size_t ctotal = 0;
    for (const std::string& p : all_parts) ctotal += p.size();
    child->data = Buffer::allocate(ctotal);
    std::int32_t* cod = reinterpret_cast<std::int32_t*>(child->offsets->data());
    char* cbd =
        ctotal != 0 ? reinterpret_cast<char*>(child->data->data()) : nullptr;
    std::int32_t cpos = 0;
    cod[0] = 0;
    for (std::size_t k = 0; k < all_parts.size(); ++k) {
        const std::string& p = all_parts[k];
        if (!p.empty()) std::memcpy(cbd + cpos, p.data(), p.size());
        cpos += static_cast<std::int32_t>(p.size());
        cod[k + 1] = cpos;
    }

    auto* out = new dftu_series();
    out->type = TypeId::List;
    out->encoding = Encoding::Flat;
    out->length = v->length;
    out->null_count = v->null_count;
    out->validity = v->validity;  // a null input row stays a null list
    std::size_t loff_bytes =
        static_cast<std::size_t>(v->length + 1) * sizeof(std::int32_t);
    out->offsets = Buffer::allocate(loff_bytes);
    std::memcpy(out->offsets->data(), list_off.data(), loff_bytes);
    out->child = std::shared_ptr<dftu_series>(child);
    return out;
}

template <class Fn>
dftu_series* list_transform(const dftu_series* v, Fn fn) {
    if (!v || !is_string_kind(v->type)) return nullptr;
    return is_wide_offset_type(v->type) ? list_transform_w<std::int64_t>(v, fn)
                                        : list_transform_w<std::int32_t>(v, fn);
}

void split_row(std::string_view row, std::string_view s_sep,
               std::vector<std::string>& all_parts) {
    if (s_sep.empty()) {
        // No well-defined split point: one part, the whole string.
        all_parts.emplace_back(row);
        return;
    }
    std::size_t pos = 0;
    while (true) {
        std::size_t hit = row.find(s_sep, pos);
        if (hit == std::string_view::npos) {
            all_parts.emplace_back(row.substr(pos));
            break;
        }
        all_parts.emplace_back(row.substr(pos, hit - pos));
        pos = hit + s_sep.size();
    }
}
}  // namespace

dftu_series* dftu_series_str_split(const dftu_series* v, const char* sep,
                                   int32_t sep_len) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_str_split(flat_v, sep, sep_len));

    std::string_view s_sep(sep, static_cast<std::size_t>(sep_len));
    return list_transform(
        v, [s_sep](std::string_view row, std::vector<std::string>& parts) {
            split_row(row, s_sep, parts);
        });
}

dftu_series* dftu_series_list_len(const dftu_series* v) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_list_len(flat_v));

    if (!v || v->type != TypeId::List || v->encoding != Encoding::Flat)
        return nullptr;
    const std::int32_t* off = dftu_series_offsets(v);
    if (!off) return nullptr;
    const std::size_t n = static_cast<std::size_t>(v->length);
    std::vector<std::int64_t> len(n);
    for (std::size_t i = 0; i < n; ++i) len[i] = off[i + 1] - off[i];
    auto* out = new dftu_series();
    out->type = TypeId::Int64;
    out->encoding = Encoding::Flat;
    out->length = v->length;
    out->null_count = v->null_count;
    out->validity = v->validity;
    out->data = Buffer::allocate(n * sizeof(std::int64_t));
    if (n != 0)
        std::memcpy(out->data->data(), len.data(), n * sizeof(std::int64_t));
    return out;
}

dftu_series* dftu_series_list_get(const dftu_series* v, int64_t index) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_list_get(flat_v, index));

    if (!v || v->type != TypeId::List || v->encoding != Encoding::Flat)
        return nullptr;
    const std::int32_t* off = dftu_series_offsets(v);
    if (!off || dftu_series_num_children(v) != 1) return nullptr;
    const std::size_t n = static_cast<std::size_t>(v->length);
    // A gather of the element at `index` (from the end when negative) per
    // row; -1 marks a null row or one whose list is too short.
    std::vector<std::int64_t> idx(n, -1);
    for (std::size_t i = 0; i < n; ++i) {
        if (v->validity && !((v->validity->data()[i >> 3] >> (i & 7)) & 1))
            continue;
        const std::int64_t len = off[i + 1] - off[i];
        const std::int64_t k = index < 0 ? len + index : index;
        if (k >= 0 && k < len) idx[i] = off[i] + k;
    }
    dftu_series* child = dftu_series_child(v, 0);
    if (!child) return nullptr;
    dftu_series* out =
        dftu_series_take(child, idx.data(), static_cast<int64_t>(n));
    dftu_series_free(child);
    return out;
}

// The pandas `.str` batch: ASCII case forms, character-class predicates,
// counting and searching from the right, prefix / suffix removal, repeat,
// centering, elementwise concatenation, regex findall, partition, and a join
// over a List<String>.
namespace {

bool ascii_alpha(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
bool ascii_digit(unsigned char c) { return c >= '0' && c <= '9'; }
bool ascii_lower(unsigned char c) { return c >= 'a' && c <= 'z'; }
bool ascii_upper(unsigned char c) { return c >= 'A' && c <= 'Z'; }
bool ascii_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

std::string capitalize(std::string_view s) {
    std::string out(s);
    bool first = true;
    for (char& c : out) {
        const auto u = static_cast<unsigned char>(c);
        if (first) {
            if (ascii_lower(u)) c = static_cast<char>(u - 32);
            first = false;
        } else if (ascii_upper(u)) {
            c = static_cast<char>(u + 32);
        }
    }
    return out;
}

std::string title(std::string_view s) {
    std::string out(s);
    bool start = true;
    for (char& c : out) {
        const auto u = static_cast<unsigned char>(c);
        if (ascii_alpha(u)) {
            if (start && ascii_lower(u)) c = static_cast<char>(u - 32);
            if (!start && ascii_upper(u)) c = static_cast<char>(u + 32);
            start = false;
        } else {
            start = true;
        }
    }
    return out;
}

std::string swapcase(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        const auto u = static_cast<unsigned char>(c);
        if (ascii_lower(u))
            c = static_cast<char>(u - 32);
        else if (ascii_upper(u))
            c = static_cast<char>(u + 32);
    }
    return out;
}

// Python's str.istitle: at least one cased character, uppercase only at the
// start of a run of letters, lowercase only after one.
bool is_title(std::string_view s) {
    bool cased = false;
    bool prev_cased = false;
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (ascii_upper(u)) {
            if (prev_cased) return false;
            prev_cased = true;
            cased = true;
        } else if (ascii_lower(u)) {
            if (!prev_cased) return false;
            prev_cased = true;
            cased = true;
        } else {
            prev_cased = false;
        }
    }
    return cased;
}

template <class Pred>
bool all_of_nonempty(std::string_view s, Pred pred) {
    if (s.empty()) return false;
    for (char c : s)
        if (!pred(static_cast<unsigned char>(c))) return false;
    return true;
}

bool str_class(std::string_view s, dftu_str_class cls) {
    switch (cls) {
        case DFTU_STR_ALNUM:
            return all_of_nonempty(s, [](unsigned char c) {
                return ascii_alpha(c) || ascii_digit(c);
            });
        case DFTU_STR_ALPHA:
            return all_of_nonempty(s, ascii_alpha);
        case DFTU_STR_DIGIT:
        case DFTU_STR_DECIMAL:
        case DFTU_STR_NUMERIC:
            return all_of_nonempty(s, ascii_digit);
        case DFTU_STR_SPACE:
            return all_of_nonempty(s, ascii_space);
        case DFTU_STR_LOWER: {
            bool cased = false;
            for (char c : s) {
                const auto u = static_cast<unsigned char>(c);
                if (ascii_upper(u)) return false;
                if (ascii_lower(u)) cased = true;
            }
            return cased;
        }
        case DFTU_STR_UPPER: {
            bool cased = false;
            for (char c : s) {
                const auto u = static_cast<unsigned char>(c);
                if (ascii_lower(u)) return false;
                if (ascii_upper(u)) cased = true;
            }
            return cased;
        }
        case DFTU_STR_TITLE:
            return is_title(s);
    }
    return false;
}

std::int64_t count_literal(std::string_view s, std::string_view pat) {
    if (pat.empty()) return static_cast<std::int64_t>(s.size()) + 1;
    std::int64_t n = 0;
    std::size_t pos = 0;
    while (true) {
        const std::size_t hit = s.find(pat, pos);
        if (hit == std::string_view::npos) break;
        ++n;
        pos = hit + pat.size();
    }
    return n;
}

// Concatenate two String columns row by row; a null on either side is null.
template <class OffA, class OffB>
dftu_series* str_cat_w(const dftu_series* a, const dftu_series* b) {
    RowReader<OffA> ra(a);
    RowReader<OffB> rb(b);
    if (!ra.ok() || !rb.ok()) return nullptr;
    std::vector<std::string> parts(static_cast<std::size_t>(a->length));
    std::shared_ptr<Buffer> validity =
        Buffer::allocate(buffer_bytes(TypeId::Bool, a->length));
    std::memset(validity->data(), 0, validity->size());
    std::int64_t nulls = 0;
    for (std::int64_t i = 0; i < a->length; ++i) {
        if (ra.is_null(i) || rb.is_null(i)) {
            ++nulls;
            continue;
        }
        validity->data()[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        std::string s(ra.at(i));
        s.append(rb.at(i));
        parts[static_cast<std::size_t>(i)] = std::move(s);
    }
    dftu_series* out = make_string(a, parts);
    out->validity = nulls ? validity : nullptr;
    out->null_count = nulls;
    return out;
}

// Join each row's list of strings with `sep`; a null list is null.
dftu_series* list_join_impl(const dftu_series* v, std::string_view sep) {
    if (!v || v->type != TypeId::List || v->encoding != Encoding::Flat)
        return nullptr;
    const std::int32_t* off = dftu_series_offsets(v);
    if (!off || !v->child || !is_string_kind(v->child->type)) return nullptr;
    const dftu_series& child = *v->child;
    const bool wide = is_wide_offset_type(child.type);
    std::vector<std::string> parts(static_cast<std::size_t>(v->length));
    for (std::int64_t i = 0; i < v->length; ++i) {
        if (v->validity && !((v->validity->data()[i >> 3] >> (i & 7)) & 1))
            continue;
        std::string s;
        for (std::int32_t k = off[i]; k < off[i + 1]; ++k) {
            if (k != off[i]) s.append(sep);
            s.append(wide ? value_at<std::int64_t>(child, k)
                          : value_at<std::int32_t>(child, k));
        }
        parts[static_cast<std::size_t>(i)] = std::move(s);
    }
    auto* out = new dftu_series();
    out->type = TypeId::String;
    out->encoding = Encoding::Flat;
    out->length = v->length;
    out->null_count = v->null_count;
    out->validity = v->validity;
    std::size_t total = 0;
    for (const std::string& p : parts) total += p.size();
    out->offsets = Buffer::allocate(static_cast<std::size_t>(v->length + 1) *
                                    sizeof(std::int32_t));
    out->data = Buffer::allocate(total);
    std::int32_t* od = reinterpret_cast<std::int32_t*>(out->offsets->data());
    char* bd = total ? reinterpret_cast<char*>(out->data->data()) : nullptr;
    std::int32_t pos = 0;
    od[0] = 0;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (!parts[i].empty())
            std::memcpy(bd + pos, parts[i].data(), parts[i].size());
        pos += static_cast<std::int32_t>(parts[i].size());
        od[i + 1] = pos;
    }
    return out;
}

}  // namespace

dftu_series* dftu_series_str_case(const dftu_series* v, int32_t op) {
    if (!v || !is_string_kind(v->type)) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_str_case(flat_v, op));
    switch (static_cast<dftu_str_case>(op)) {
        case DFTU_STR_CAPITALIZE:
            return string_transform(v, capitalize);
        case DFTU_STR_TITLE_CASE:
            return string_transform(v, title);
        case DFTU_STR_SWAPCASE:
            return string_transform(v, swapcase);
    }
    return nullptr;
}

dftu_series* dftu_series_str_is(const dftu_series* v, int32_t cls) {
    if (!v) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_str_is(flat_v, cls));
    if (cls < DFTU_STR_ALNUM || cls > DFTU_STR_TITLE) return nullptr;
    const auto c = static_cast<dftu_str_class>(cls);
    return string_predicate(
        v, [c](std::string_view s) { return str_class(s, c); });
}

dftu_series* dftu_series_str_count(const dftu_series* v, const char* pat,
                                   int32_t pat_len) {
    if (!v || !is_string_kind(v->type)) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_str_count(flat_v, pat, pat_len));
    std::string_view p(pat, static_cast<std::size_t>(pat_len));
    return int_transform(
        v, [p](std::string_view s) { return count_literal(s, p); });
}

dftu_series* dftu_series_str_rfind(const dftu_series* v, const char* needle,
                                   int32_t needle_len) {
    if (!v || !is_string_kind(v->type)) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v,
                      dftu_series_str_rfind(flat_v, needle, needle_len));
    std::string_view n(needle, static_cast<std::size_t>(needle_len));
    return int_transform(v, [n](std::string_view s) {
        const std::size_t hit = s.rfind(n);
        return hit == std::string_view::npos ? std::int64_t{-1}
                                             : static_cast<std::int64_t>(hit);
    });
}

dftu_series* dftu_series_str_remove_prefix(const dftu_series* v,
                                           const char* prefix, int32_t len) {
    if (!v || !is_string_kind(v->type)) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v,
                      dftu_series_str_remove_prefix(flat_v, prefix, len));
    std::string_view p(prefix, static_cast<std::size_t>(len));
    return string_transform(v, [p](std::string_view s) {
        if (!p.empty() && s.substr(0, p.size()) == p) s.remove_prefix(p.size());
        return std::string(s);
    });
}

dftu_series* dftu_series_str_remove_suffix(const dftu_series* v,
                                           const char* suffix, int32_t len) {
    if (!v || !is_string_kind(v->type)) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v,
                      dftu_series_str_remove_suffix(flat_v, suffix, len));
    std::string_view p(suffix, static_cast<std::size_t>(len));
    return string_transform(v, [p](std::string_view s) {
        if (!p.empty() && s.size() >= p.size() &&
            s.substr(s.size() - p.size()) == p)
            s.remove_suffix(p.size());
        return std::string(s);
    });
}

dftu_series* dftu_series_str_repeat(const dftu_series* v, int64_t n) {
    if (!v || !is_string_kind(v->type) || n < 0) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_str_repeat(flat_v, n));
    return string_transform(v, [n](std::string_view s) {
        std::string out;
        out.reserve(s.size() * static_cast<std::size_t>(n));
        for (std::int64_t k = 0; k < n; ++k) out.append(s);
        return out;
    });
}

dftu_series* dftu_series_str_center(const dftu_series* v, int64_t width,
                                    char fill) {
    if (!v || !is_string_kind(v->type)) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_str_center(flat_v, width, fill));
    return string_transform(v, [width, fill](std::string_view s) {
        const std::int64_t len = static_cast<std::int64_t>(s.size());
        if (len >= width) return std::string(s);
        // CPython's placement: an odd gap puts its extra fill character on
        // the left when the width is odd, else on the right.
        const std::int64_t gap = width - len;
        const std::int64_t left = gap / 2 + (gap & width & 1);
        std::string out(static_cast<std::size_t>(left), fill);
        out.append(s);
        out.append(static_cast<std::size_t>(gap - left), fill);
        return out;
    });
}

dftu_series* dftu_series_str_cat(const dftu_series* a, const dftu_series* b) {
    if (!a || !b) return nullptr;
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_str_cat(flat_a, b));
    DFTU_FLAT_OPERAND(b, flat_b, dftu_series_str_cat(a, flat_b));
    if (!is_string_kind(a->type) || !is_string_kind(b->type) ||
        a->length != b->length)
        return nullptr;
    const bool wa = is_wide_offset_type(a->type);
    const bool wb = is_wide_offset_type(b->type);
    if (wa && wb) return str_cat_w<std::int64_t, std::int64_t>(a, b);
    if (wa) return str_cat_w<std::int64_t, std::int32_t>(a, b);
    if (wb) return str_cat_w<std::int32_t, std::int64_t>(a, b);
    return str_cat_w<std::int32_t, std::int32_t>(a, b);
}

dftu_series* dftu_series_str_findall(const dftu_series* v, const char* pattern,
                                     int32_t pattern_len) {
    if (!v || !is_string_kind(v->type)) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v,
                      dftu_series_str_findall(flat_v, pattern, pattern_len));
    std::regex re;
    try {
        re = std::regex(
            std::string(pattern, static_cast<std::size_t>(pattern_len)));
    } catch (const std::regex_error&) {
        return nullptr;
    }
    return list_transform(
        v, [&re](std::string_view row, std::vector<std::string>& parts) {
            const std::string text(row);
            for (auto it = std::sregex_iterator(text.begin(), text.end(), re);
                 it != std::sregex_iterator(); ++it)
                parts.emplace_back(it->str());
        });
}

dftu_series* dftu_series_str_partition(const dftu_series* v, const char* sep,
                                       int32_t sep_len, int32_t from_right) {
    if (!v || !is_string_kind(v->type) || sep_len <= 0) return nullptr;
    DFTU_FLAT_OPERAND(
        v, flat_v, dftu_series_str_partition(flat_v, sep, sep_len, from_right));
    std::string_view s_sep(sep, static_cast<std::size_t>(sep_len));
    const bool right = from_right != 0;
    return list_transform(v, [s_sep, right](std::string_view row,
                                            std::vector<std::string>& parts) {
        const std::size_t hit = right ? row.rfind(s_sep) : row.find(s_sep);
        if (hit == std::string_view::npos) {
            // Python: (row, "", "") for partition, ("", "", row) for
            // rpartition.
            parts.emplace_back(right ? std::string_view{} : row);
            parts.emplace_back();
            parts.emplace_back(right ? row : std::string_view{});
            return;
        }
        parts.emplace_back(row.substr(0, hit));
        parts.emplace_back(s_sep);
        parts.emplace_back(row.substr(hit + s_sep.size()));
    });
}

dftu_series* dftu_series_list_join(const dftu_series* v, const char* sep,
                                   int32_t sep_len) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_list_join(flat_v, sep, sep_len));

    return list_join_impl(
        v, std::string_view(sep, static_cast<std::size_t>(sep_len)));
}
