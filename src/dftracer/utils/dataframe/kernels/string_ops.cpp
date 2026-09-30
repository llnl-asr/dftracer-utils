#include <dftracer/utils/core/common/hash/fnv1a.h>
#include <dftracer/utils/core/common/hash/hex64.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/string_simd.h>
#include <dftracer/utils/dataframe/internal/varwidth_offsets.h>
#include <dftracer/utils/dataframe/kernels/string_ops.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/duql/pattern_engine.h>
#include <dftracer/utils/duql/substr_simd.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <vector>

// Highway static dispatch (baseline target): only the two
// naturally-vectorizable string ops use it - str_len_bytes (adjacent int32
// offset difference) and the ASCII case folds (byte compare + conditional add).
// Every other op is variable-length per row and is legitimately scalar.
#include <hwy/highway.h>
namespace hn = hwy::HWY_NAMESPACE;

namespace duql = dftracer::utils::duql;
using duql::detail::substr_find;

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
    } else if (v->encoding == Encoding::Dictionary && v->child()) {
        const dftu_series& dict = *v->child();
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
        } else if (v->encoding == Encoding::Dictionary && v->child() &&
                   offsets_of<Off>(*v->child()) && v->child()->data &&
                   v->data) {
            off_ = reinterpret_cast<const Off*>(
                offsets_of<Off>(*v->child())->data());
            data_ = reinterpret_cast<const char*>(v->child()->data->data());
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

// The strings whose regex match reached its work limit; their rows turn null.
class LimitedRows {
   public:
    void add(std::string_view s) {
        std::lock_guard<std::mutex> lock(mu_);
        rows_.emplace(s);
    }

    // `out` with every row of `v` whose string is listed made null.
    dftu_series* apply(dftu_series* out, const dftu_series* v) const {
        if (!out || rows_.empty()) return out;
        dftu_series* ok = string_predicate(v, [this](std::string_view s) {
            return !rows_.contains(std::string(s));
        });
        if (!ok) return out;
        const std::size_t bytes = buffer_bytes(TypeId::Bool, v->length);
        auto valid = Buffer::allocate(bytes);
        for (std::size_t k = 0; k < bytes; ++k) {
            const std::uint8_t have =
                v->validity ? v->validity->data()[k] : std::uint8_t{0xFF};
            valid->data()[k] =
                static_cast<std::uint8_t>(ok->data->data()[k] & have);
        }
        std::int64_t nulls = 0;
        for (std::int64_t i = 0; i < v->length; ++i)
            if (!((valid->data()[i >> 3] >> (i & 7)) & 1)) ++nulls;
        delete ok;
        out->validity = std::move(valid);
        out->null_count = nulls;
        return out;
    }

   private:
    std::mutex mu_;
    std::unordered_set<std::string> rows_;
};

// Bool mask of `p` over `v`; a row whose match reached the work limit is null.
dftu_series* pattern_predicate(const dftu_series* v,
                               const duql::CompiledPattern& p) {
    // A literal pattern runs inline, with no call per row.
    if (const auto t = duql::literal_test(p); t && !t->icase) {
        const std::string_view lit = t->text;
        switch (t->op) {
            case duql::LiteralTest::Op::EQUALS:
                return string_predicate(
                    v, [lit](std::string_view s) { return s == lit; });
            case duql::LiteralTest::Op::STARTS_WITH:
                return string_predicate(v, [lit](std::string_view s) {
                    return s.starts_with(lit);
                });
            case duql::LiteralTest::Op::ENDS_WITH:
                return string_predicate(
                    v, [lit](std::string_view s) { return s.ends_with(lit); });
            case duql::LiteralTest::Op::CONTAINS:
                return string_predicate(v, [lit](std::string_view s) {
                    return substr_find(
                               s.data(), static_cast<std::int64_t>(s.size()),
                               lit.data(),
                               static_cast<std::int64_t>(lit.size())) >= 0;
                });
        }
    }
    // Case-sensitive globs run inline too; SEGMENTS search long strings.
    if (!p.icase && p.kind == duql::detail::Kind::GLOB)
        return string_predicate(v, [toks = p.glob](std::string_view s) {
            return duql::detail::glob_match_t<false, true>(toks, s);
        });
    if (!p.icase && p.kind == duql::detail::Kind::SEGMENTS)
        return string_predicate(v, [&p, toks = p.glob](std::string_view s) {
            return s.size() < duql::detail::SIMD_MIN_HAY
                       ? duql::detail::glob_match_t<false, false>(toks, s)
                       : duql::detail::segments_match(p, s);
        });
    LimitedRows limited;
    dftu_series* out = string_predicate(v, [&](std::string_view s) {
        const auto r = duql::match(p, s);
        if (r == duql::MatchResult::LIMIT) limited.add(s);
        return r == duql::MatchResult::YES;
    });
    return limited.apply(out, v);
}

}  // namespace

namespace dftracer::utils::dataframe {

Series str_pattern(const Series& v, const duql::CompiledPattern& p) {
    if (!v.valid()) return {};
    if (v.encoding() != Encoding::Flat && v.encoding() != Encoding::Dictionary)
        return str_pattern(v.materialize(), p);
    return Series{pattern_predicate(v.handle(), p)};
}

}  // namespace dftracer::utils::dataframe

namespace {

template <class Off>
dftu_series* regex_replace_w(const dftu_series* v,
                             const duql::CompiledPattern& re,
                             const duql::Substitution& sub) {
    RowReader<Off> r(v);
    if (!r.ok()) return nullptr;
    const std::size_t n = static_cast<std::size_t>(v->length);
    std::vector<std::int32_t> offsets(n + 1, 0);
    std::string data;
    std::string buf;
    std::vector<std::uint8_t> valid((n + 7) / 8, 0);
    bool any_null = false;
    for (std::size_t i = 0; i < n; ++i) {
        bool hit = false;
        if (!r.is_null(static_cast<std::int64_t>(i))) {
            const std::string_view s = r.at(static_cast<std::int64_t>(i));
            if (duql::regex_replace(re, sub, s, buf) !=
                duql::MatchResult::LIMIT) {
                data.append(buf);
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

dftu_series* regex_replace_flat(const dftu_series* v,
                                const duql::CompiledPattern& re,
                                const duql::Substitution& sub) {
    return is_wide_offset_type(v->type)
               ? regex_replace_w<std::int64_t>(v, re, sub)
               : regex_replace_w<std::int32_t>(v, re, sub);
}

}  // namespace

namespace dftracer::utils::dataframe {

Series str_regex_replace(const Series& v, const duql::CompiledPattern& p,
                         const duql::Substitution& sub) {
    if (!v.valid()) return {};
    const Series flat = v.materialize();
    return Series{regex_replace_flat(flat.handle(), p, sub)};
}

}  // namespace dftracer::utils::dataframe

namespace {

duql::PatternPtr compiled(duql::PatternResult r) {
    return r ? std::move(*r) : nullptr;
}

std::string_view pattern_text(const char* pattern, int32_t len) {
    return {pattern, static_cast<std::size_t>(len)};
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

    const auto p = compiled(duql::compile_regex(
        pattern_text(pattern, pattern_len), false, /*whole=*/true));
    if (!p) return nullptr;
    return pattern_predicate(v, *p);
}

namespace {

// The capture `group` of the first `re` match in each row, null where the row
// is null or does not match: a String column with its own validity.
template <class Off>
dftu_series* str_extract_w(const dftu_series* v,
                           const duql::CompiledPattern& re, std::size_t group) {
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
            std::string_view m;
            if (duql::extract(re, s, group, m) == duql::MatchResult::YES) {
                data.append(m);
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
    const auto re = compiled(
        duql::compile_regex(pattern_text(pattern, pattern_len), false));
    if (!re) return nullptr;
    const auto g = static_cast<std::size_t>(group);
    if (g > duql::capture_count(*re)) return nullptr;
    return is_wide_offset_type(v->type)
               ? str_extract_w<std::int64_t>(v, *re, g)
               : str_extract_w<std::int32_t>(v, *re, g);
}

dftu_series* dftu_series_str_regex_replace(const dftu_series* v,
                                           const char* pattern,
                                           int32_t pattern_len, const char* to,
                                           int32_t to_len) {
    if (!v || !pattern || !to) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v,
                      dftu_series_str_regex_replace(flat_v, pattern,
                                                    pattern_len, to, to_len));
    const auto re = compiled(
        duql::compile_regex(pattern_text(pattern, pattern_len), false));
    if (!re) return nullptr;
    auto sub = duql::compile_substitution(
        *re, std::string_view(to, static_cast<std::size_t>(to_len)));
    if (!sub) return nullptr;
    return regex_replace_flat(v, *re, *sub);
}

dftu_series* dftu_series_str_search(const dftu_series* v, const char* pattern,
                                    int32_t pattern_len) {
    DFTU_FLAT_OPERAND(v, flat_v,
                      dftu_series_str_search(flat_v, pattern, pattern_len));

    const auto p = compiled(
        duql::compile_regex(pattern_text(pattern, pattern_len), false));
    if (!p) return nullptr;
    return pattern_predicate(v, *p);
}

dftu_series* dftu_series_str_like(const dftu_series* v, const char* pattern,
                                  int32_t pattern_len) {
    DFTU_FLAT_OPERAND(v, flat_v,
                      dftu_series_str_like(flat_v, pattern, pattern_len));

    const auto p = compiled(
        duql::compile_like(pattern_text(pattern, pattern_len), false, '\\'));
    if (!p) return nullptr;
    return pattern_predicate(v, *p);
}

dftu_series* dftu_series_str_len_bytes(const dftu_series* v) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_str_len_bytes(flat_v));

    // FLAT, narrow offsets: adjacent int32 offset difference, vectorized.
    // Every other case (DICTIONARY, or a wide-offset FLAT column) is scalar;
    // a Large* column's row count is bounded by int32 either way, only its
    // byte offsets are not, so the SIMD int32 path stays narrow-only.
    if (v->encoding == Encoding::Flat && v->offsets && !v->wide_offsets()) {
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
    if (v->encoding == Encoding::Flat && v->offsets && !v->wide_offsets() &&
        v->data) {
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
    out->set_child(std::shared_ptr<dftu_series>(child));
    return out;
}

template <class Fn>
dftu_series* list_transform(const dftu_series* v, Fn fn) {
    if (!v || !is_string_kind(v->type)) return nullptr;
    return is_wide_offset_type(v->type) ? list_transform_w<std::int64_t>(v, fn)
                                        : list_transform_w<std::int32_t>(v, fn);
}

}  // namespace

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

// The pandas `.str` batch. Every op below works on the column's buffers
// directly (no per-row std::string): the SIMD primitives of strsimd scan the
// whole data buffer, the builders size the output in one pass and fill it in a
// second.
namespace {

namespace simd = dftracer::utils::dataframe::strsimd;

// A FLAT String/Binary column read at offset width `Off`. Offsets are
// absolute positions in `data`; a column cut from a larger one starts past 0.
template <class Off>
struct FlatStr {
    const Off* off = nullptr;
    const std::uint8_t* data = nullptr;
    const std::uint8_t* validity = nullptr;
    std::int64_t n = 0;
    bool ok = false;

    explicit FlatStr(const dftu_series* v) {
        if (!v || v->encoding != Encoding::Flat || !is_string_kind(v->type) ||
            is_wide_offset_type(v->type) != std::is_same_v<Off, std::int64_t>)
            return;
        const auto& ob = offsets_of<Off>(*v);
        if (!ob) return;
        off = reinterpret_cast<const Off*>(ob->data());
        data = v->data ? v->data->data() : nullptr;
        validity = v->validity ? v->validity->data() : nullptr;
        n = v->length;
        ok = true;
    }
    bool is_null(std::int64_t i) const {
        return validity != nullptr && ((validity[i >> 3] >> (i & 7)) & 1) == 0;
    }
    std::int64_t lo() const {
        return n != 0 ? static_cast<std::int64_t>(off[0]) : 0;
    }
    std::int64_t hi() const {
        return n != 0 ? static_cast<std::int64_t>(off[n]) : 0;
    }
    std::int64_t len(std::int64_t i) const {
        return static_cast<std::int64_t>(off[i + 1]) - off[i];
    }
    const std::uint8_t* row(std::int64_t i) const { return data + off[i]; }
};

template <class Fn>
dftu_series* with_flat(const dftu_series* v, Fn&& fn) {
    if (!v || !is_string_kind(v->type) || v->encoding != Encoding::Flat)
        return nullptr;
    if (is_wide_offset_type(v->type)) {
        FlatStr<std::int64_t> f(v);
        return f.ok ? fn(f) : nullptr;
    }
    FlatStr<std::int32_t> f(v);
    return f.ok ? fn(f) : nullptr;
}

void share_nulls(dftu_series* out, const dftu_series* v) {
    if (!out) return;
    out->validity = v->validity;
    out->null_count = v->null_count;
}

void copy_bytes(std::uint8_t* dst, const std::uint8_t* src, std::int64_t n) {
    if (n > 0) std::memcpy(dst, src, static_cast<std::size_t>(n));
}

constexpr std::int64_t BUILD_GRAIN = 1 << 14;

// A narrow String column of `n` rows: row i is `len_of(i)` bytes that
// `fill_of(i, dst)` writes (a null row is empty and never asked). Sizes the
// output in one pass and fills it in a second, across the pool for a large
// column. nullptr when the bytes pass what an int32 offset reaches.
template <class IsNull, class LenFn, class FillFn>
dftu_series* build_flat_string(std::int64_t n, IsNull is_null, LenFn len_of,
                               FillFn fill_of) {
    auto offb = Buffer::allocate(static_cast<std::size_t>(n + 1) *
                                 sizeof(std::int32_t));
    auto* od = reinterpret_cast<std::int32_t*>(offb->data());
    std::int64_t total = 0;
    od[0] = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        if (!is_null(i)) {
            const std::int64_t l = len_of(i);
            if (l < 0 || __builtin_add_overflow(total, l, &total) ||
                total > std::numeric_limits<std::int32_t>::max())
                return nullptr;
        }
        od[i + 1] = static_cast<std::int32_t>(total);
    }
    auto datab = Buffer::allocate(static_cast<std::size_t>(total));
    std::uint8_t* d = datab->data();
    auto body = [&](std::int64_t b, std::int64_t e) {
        for (std::int64_t i = b; i < e; ++i)
            if (!is_null(i)) fill_of(i, d + od[i]);
    };
    if (n > BUILD_GRAIN && parallel_backend_installed())
        parallel_for(n, BUILD_GRAIN, body);
    else
        body(0, n);
    auto* out = new dftu_series();
    out->type = TypeId::String;
    out->encoding = Encoding::Flat;
    out->length = n;
    out->offsets = std::move(offb);
    out->data = std::move(datab);
    return out;
}

// One string op over a single column: the output shares its nulls.
template <class LenFn, class FillFn>
dftu_series* map_strings(const dftu_series* v, LenFn len_of, FillFn fill_of) {
    dftu_series* out = with_flat(v, [&](const auto& f) {
        return build_flat_string(
            f.n, [&](std::int64_t i) { return f.is_null(i); },
            [&](std::int64_t i) { return len_of(f, i); },
            [&](std::int64_t i, std::uint8_t* dst) { fill_of(f, i, dst); });
    });
    share_nulls(out, v);
    return out;
}

// The bit arrays of one kind over p[0, n), across the pool for a large buffer.
struct Bits {
    std::vector<std::uint64_t> bad;
    std::vector<std::uint64_t> aux;
};

Bits compute_bits(simd::BitKind kind, const std::uint8_t* p, std::size_t n,
                  std::uint8_t eq, bool want_aux) {
    const std::size_t words = (n + 63) / 64;
    Bits b;
    b.bad.assign(words + 1, 0);
    if (want_aux) b.aux.assign(words + 1, 0);
    std::uint64_t* bp = b.bad.data();
    std::uint64_t* ap = want_aux ? b.aux.data() : nullptr;
    constexpr std::int64_t CHUNK = 1 << 14;  // words: 1 MiB of bytes
    if (static_cast<std::int64_t>(words) > CHUNK &&
        parallel_backend_installed()) {
        parallel_for(static_cast<std::int64_t>(words), CHUNK,
                     [&](std::int64_t b0, std::int64_t e0) {
                         simd::class_bits(
                             kind, p, n, static_cast<std::size_t>(b0),
                             static_cast<std::size_t>(e0), eq, bp, ap);
                     });
    } else if (words != 0) {
        simd::class_bits(kind, p, n, 0, words, eq, bp, ap);
    }
    return b;
}

bool lower_byte(std::uint8_t c) { return c >= 'a' && c <= 'z'; }

// Narrow String out of a List<String> shape: row i of the list holds
// `list_off[i + 1] - list_off[i]` strings.
dftu_series* make_str_list(const dftu_series* v,
                           const std::vector<std::int32_t>& list_off,
                           std::int64_t parts, std::shared_ptr<Buffer> coff,
                           std::shared_ptr<Buffer> cdata) {
    auto* child = new dftu_series();
    child->type = TypeId::String;
    child->encoding = Encoding::Flat;
    child->length = parts;
    child->offsets = std::move(coff);
    child->data = std::move(cdata);
    auto* out = new dftu_series();
    out->type = TypeId::List;
    out->encoding = Encoding::Flat;
    out->length = v->length;
    out->null_count = v->null_count;
    out->validity = v->validity;  // a null input row stays a null list
    const std::size_t loff_bytes = list_off.size() * sizeof(std::int32_t);
    out->offsets = Buffer::allocate(loff_bytes);
    std::memcpy(out->offsets->data(), list_off.data(), loff_bytes);
    out->set_child(std::shared_ptr<dftu_series>(child));
    return out;
}

// The first (last, when `from_right`) place in row [s, e) of p where `sep`
// occurs, or NPOS: candidate bits of sep's first byte, each verified.
std::size_t find_sep(const std::uint64_t* cand, const std::uint8_t* p,
                     std::size_t s, std::size_t e, std::string_view sep,
                     bool from_right) {
    const std::size_t sl = sep.size();
    if (e - s < sl) return simd::NPOS;
    const std::size_t limit = e - sl + 1;  // one past the last start
    auto verify = [&](std::size_t q) {
        return sl == 1 || std::memcmp(p + q + 1, sep.data() + 1, sl - 1) == 0;
    };
    if (!from_right) {
        std::size_t pos = s;
        while (pos < limit) {
            const std::size_t q = simd::bits_next(cand, pos, limit);
            if (q >= limit) return simd::NPOS;
            if (verify(q)) return q;
            pos = q + 1;
        }
        return simd::NPOS;
    }
    std::size_t lim = limit;
    while (lim > s) {
        const std::size_t q = simd::bits_prev(cand, s, lim);
        if (q == simd::NPOS) return simd::NPOS;
        if (verify(q)) return q;
        lim = q;
    }
    return simd::NPOS;
}

dftu_series* case_impl(const dftu_series* v, simd::CaseKind kind,
                       bool capitalize) {
    if (!v || !is_string_kind(v->type)) return nullptr;
    const bool wide = is_wide_offset_type(v->type);
    auto run = [&](const auto& f) -> dftu_series* {
        auto* out = new dftu_series();
        out->type = wide ? TypeId::LargeString : TypeId::String;
        out->encoding = Encoding::Flat;
        out->length = v->length;
        out->null_count = v->null_count;
        out->validity = v->validity;
        using Off = std::remove_const_t<std::remove_pointer_t<decltype(f.off)>>;
        offsets_of<Off>(*out) = offsets_of<Off>(*v);  // byte lengths unchanged
        const std::size_t data_len = v->data ? v->data->size() : 0;
        out->data = Buffer::allocate(data_len);
        if (data_len == 0) return out;
        const std::uint8_t* in = v->data->data();
        std::uint8_t* o = out->data->data();
        simd::case_map(capitalize ? simd::CaseKind::Lower : kind, in, o,
                       data_len);
        if (capitalize || kind == simd::CaseKind::Title) {
            // A row's first byte has no previous byte: it is upper-cased when
            // lower, whatever sat before it in the buffer.
            for (std::int64_t i = 0; i < f.n; ++i) {
                if (f.len(i) <= 0) continue;
                const std::int64_t s = f.off[i];
                o[s] = lower_byte(in[s]) ? static_cast<std::uint8_t>(in[s] - 32)
                                         : in[s];
            }
        }
        return out;
    };
    if (wide) {
        FlatStr<std::int64_t> f(v);
        return f.ok ? run(f) : nullptr;
    }
    FlatStr<std::int32_t> f(v);
    return f.ok ? run(f) : nullptr;
}

dftu_series* is_impl(const dftu_series* v, int cls) {
    simd::BitKind kind = simd::BitKind::Alnum;
    bool want_aux = false;
    switch (cls) {
        case DFTU_STR_ALNUM:
            kind = simd::BitKind::Alnum;
            break;
        case DFTU_STR_ALPHA:
            kind = simd::BitKind::Alpha;
            break;
        case DFTU_STR_DIGIT:
        case DFTU_STR_DECIMAL:
        case DFTU_STR_NUMERIC:
            kind = simd::BitKind::Digit;
            break;
        case DFTU_STR_SPACE:
            kind = simd::BitKind::Space;
            break;
        case DFTU_STR_LOWER:
            kind = simd::BitKind::Lower;
            want_aux = true;
            break;
        case DFTU_STR_UPPER:
            kind = simd::BitKind::Upper;
            want_aux = true;
            break;
        case DFTU_STR_TITLE:
            kind = simd::BitKind::Title;
            want_aux = true;
            break;
        default:
            return nullptr;
    }
    const bool title = kind == simd::BitKind::Title;
    return with_flat(v, [&](const auto& f) -> dftu_series* {
        const std::int64_t lo = f.lo();
        const std::size_t nbytes = static_cast<std::size_t>(f.hi() - lo);
        const std::uint8_t* p = f.data != nullptr ? f.data + lo : nullptr;
        // Short strings: one vector pass over the whole buffer, then a bit
        // range per row. Long strings: scan each row and stop at its first
        // bad vector (no bit arrays at all).
        const std::int64_t avg =
            f.n > 0 ? static_cast<std::int64_t>(nbytes) / f.n : 0;
        const simd::PredStrategy strat = simd::pred_strategy();
        const bool by_rows = strat == simd::PredStrategy::Rows ||
                             (strat == simd::PredStrategy::Auto &&
                              avg >= simd::PRED_ROWS_MIN_AVG);
        Bits bits;
        if (!by_rows) bits = compute_bits(kind, p, nbytes, 0, want_aux);
        const std::uint64_t* bad = bits.bad.data();
        const std::uint64_t* aux =
            want_aux && !by_rows ? bits.aux.data() : nullptr;

        auto* out = new dftu_series();
        out->type = TypeId::Bool;
        out->encoding = Encoding::Flat;
        out->length = f.n;
        out->null_count = v->null_count;
        out->validity = v->validity;
        const std::size_t bytes = buffer_bytes(TypeId::Bool, f.n);
        out->data = Buffer::allocate(bytes);
        std::memset(out->data->data(), 0, bytes);
        std::uint8_t* res = out->data->data();
        auto rows = [&](std::int64_t b, std::int64_t e) {
            if (by_rows) {
                simd::class_rows(kind, f.data, f.off, lo, b, e, res);
                return;
            }
            for (std::int64_t i = b; i < e; ++i) {
                const std::size_t s = static_cast<std::size_t>(f.off[i] - lo);
                const std::size_t t =
                    static_cast<std::size_t>(f.off[i + 1] - lo);
                if (s >= t) continue;  // an empty string is in no class
                bool yes;
                if (title) {
                    // The first byte has no previous byte, so it breaks the
                    // rule when lower; the rest read the bit array.
                    yes = !lower_byte(p[s]) && !simd::bits_any(bad, s + 1, t) &&
                          simd::bits_any(aux, s, t);
                } else {
                    yes = !simd::bits_any(bad, s, t) &&
                          (aux == nullptr || simd::bits_any(aux, s, t));
                }
                if (yes)
                    res[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
            }
        };
        constexpr std::int64_t ROW_GRAIN = 1 << 15;  // a multiple of 8
        if (f.n > ROW_GRAIN && parallel_backend_installed())
            parallel_for(f.n, ROW_GRAIN, rows);
        else
            rows(0, f.n);
        return out;
    });
}

// Pad / center: `mode` 0 pads the start, 1 the end, 2 centers.
dftu_series* pad_impl(const dftu_series* v, std::int64_t width, char fill,
                      int mode) {
    return map_strings(
        v,
        [width](const auto& f, std::int64_t i) {
            return std::max<std::int64_t>(f.len(i), width);
        },
        [width, fill, mode](const auto& f, std::int64_t i, std::uint8_t* dst) {
            const std::int64_t len = f.len(i);
            if (len >= width) {
                copy_bytes(dst, f.row(i), len);
                return;
            }
            const std::int64_t gap = width - len;
            // CPython's center puts an odd gap's extra fill on the left when
            // the width is odd, else on the right.
            const std::int64_t left = mode == 0   ? gap
                                      : mode == 1 ? 0
                                                  : gap / 2 + (gap & width & 1);
            std::memset(dst, fill, static_cast<std::size_t>(left));
            copy_bytes(dst + left, f.row(i), len);
            std::memset(dst + left + len, fill,
                        static_cast<std::size_t>(gap - left));
        });
}

dftu_series* zfill_impl(const dftu_series* v, std::int64_t width) {
    return map_strings(
        v,
        [width](const auto& f, std::int64_t i) {
            return std::max<std::int64_t>(f.len(i), width);
        },
        [width](const auto& f, std::int64_t i, std::uint8_t* dst) {
            const std::int64_t len = f.len(i);
            const std::uint8_t* r = f.row(i);
            if (len >= width) {
                copy_bytes(dst, r, len);
                return;
            }
            const std::int64_t gap = width - len;
            // A leading sign stays first; the zeros go after it.
            const bool sign = len > 0 && (r[0] == '+' || r[0] == '-');
            std::int64_t at = 0;
            if (sign) dst[at++] = r[0];
            std::memset(dst + at, '0', static_cast<std::size_t>(gap));
            at += gap;
            copy_bytes(dst + at, r + (sign ? 1 : 0), len - (sign ? 1 : 0));
        });
}

dftu_series* remove_affix_impl(const dftu_series* v, std::string_view a,
                               bool prefix) {
    const auto plen = static_cast<std::int64_t>(a.size());
    auto cut = [a, plen, prefix](const auto& f,
                                 std::int64_t i) -> std::int64_t {
        const std::int64_t len = f.len(i);
        if (plen == 0 || len < plen) return 0;
        const std::uint8_t* at = f.row(i) + (prefix ? 0 : len - plen);
        return std::memcmp(at, a.data(), static_cast<std::size_t>(plen)) == 0
                   ? plen
                   : 0;
    };
    return map_strings(
        v,
        [&cut](const auto& f, std::int64_t i) { return f.len(i) - cut(f, i); },
        [&cut, prefix](const auto& f, std::int64_t i, std::uint8_t* dst) {
            const std::int64_t c = cut(f, i);
            copy_bytes(dst, f.row(i) + (prefix ? c : 0), f.len(i) - c);
        });
}

dftu_series* repeat_impl(const dftu_series* v, std::int64_t n) {
    return map_strings(
        v,
        [n](const auto& f, std::int64_t i) {
            std::int64_t total;
            if (__builtin_mul_overflow(f.len(i), n, &total))
                return std::numeric_limits<std::int64_t>::max();
            return total;
        },
        [n](const auto& f, std::int64_t i, std::uint8_t* dst) {
            const std::int64_t len = f.len(i);
            const std::int64_t total = len * n;
            if (total == 0) return;
            copy_bytes(dst, f.row(i), len);
            // Double the filled prefix until the row is full.
            std::int64_t filled = len;
            while (filled < total) {
                const std::int64_t c = std::min(filled, total - filled);
                std::memcpy(dst + filled, dst, static_cast<std::size_t>(c));
                filled += c;
            }
        });
}

dftu_series* slice_impl(const dftu_series* v, std::int64_t start,
                        std::int64_t length) {
    // The byte range of a row: a negative start counts from the end, a
    // negative length runs to the end (polars slice with no length).
    auto range = [start, length](std::int64_t len) {
        std::int64_t b = start < 0 ? start + len : start;
        b = std::clamp<std::int64_t>(b, 0, len);
        std::int64_t e = length < 0 ? len : b + length;
        e = std::clamp<std::int64_t>(e, b, len);
        return std::pair<std::int64_t, std::int64_t>(b, e);
    };
    return map_strings(
        v,
        [&range](const auto& f, std::int64_t i) {
            const auto [b, e] = range(f.len(i));
            return e - b;
        },
        [&range](const auto& f, std::int64_t i, std::uint8_t* dst) {
            const auto [b, e] = range(f.len(i));
            copy_bytes(dst, f.row(i) + b, e - b);
        });
}

template <class OffA, class OffB>
dftu_series* cat_impl(const dftu_series* a, const dftu_series* b) {
    FlatStr<OffA> fa(a);
    FlatStr<OffB> fb(b);
    if (!fa.ok || !fb.ok) return nullptr;
    dftu_series* out = build_flat_string(
        fa.n, [&](std::int64_t i) { return fa.is_null(i) || fb.is_null(i); },
        [&](std::int64_t i) { return fa.len(i) + fb.len(i); },
        [&](std::int64_t i, std::uint8_t* dst) {
            copy_bytes(dst, fa.row(i), fa.len(i));
            copy_bytes(dst + fa.len(i), fb.row(i), fb.len(i));
        });
    if (!out) return nullptr;
    if (fa.validity == nullptr && fb.validity == nullptr) return out;
    // A null on either side is null: the validity is the two ANDed.
    const std::size_t vbytes = buffer_bytes(TypeId::Bool, fa.n);
    auto valid = Buffer::allocate(vbytes);
    std::int64_t valid_rows = 0;
    for (std::size_t k = 0; k < vbytes; ++k) {
        std::uint8_t w =
            static_cast<std::uint8_t>((fa.validity ? fa.validity[k] : 0xFF) &
                                      (fb.validity ? fb.validity[k] : 0xFF));
        const std::int64_t rows_here =
            std::min<std::int64_t>(8, fa.n - static_cast<std::int64_t>(k) * 8);
        if (rows_here < 8)
            w &= static_cast<std::uint8_t>((1u << rows_here) - 1);
        valid->data()[k] = w;
        valid_rows += std::popcount(static_cast<unsigned>(w));
    }
    out->null_count = fa.n - valid_rows;
    out->validity = out->null_count != 0 ? std::move(valid) : nullptr;
    return out;
}

template <class ChildOff>
dftu_series* join_impl(const dftu_series* v, const dftu_series& child,
                       std::string_view sep) {
    FlatStr<ChildOff> c(&child);
    if (!c.ok) return nullptr;
    const std::int32_t* off = dftu_series_offsets(v);
    const auto sl = static_cast<std::int64_t>(sep.size());
    auto is_null = [&](std::int64_t i) {
        return v->validity && !((v->validity->data()[i >> 3] >> (i & 7)) & 1);
    };
    dftu_series* out = build_flat_string(
        v->length, is_null,
        [&](std::int64_t i) {
            const std::int64_t cnt = off[i + 1] - off[i];
            const std::int64_t bytes =
                static_cast<std::int64_t>(c.off[off[i + 1]]) - c.off[off[i]];
            return bytes + (cnt > 0 ? (cnt - 1) * sl : 0);
        },
        [&](std::int64_t i, std::uint8_t* dst) {
            const std::int64_t cnt = off[i + 1] - off[i];
            if (cnt == 0) return;
            if (sl == 0 || cnt == 1) {
                // The elements are back to back: one copy.
                copy_bytes(dst, c.data + c.off[off[i]],
                           static_cast<std::int64_t>(c.off[off[i + 1]]) -
                               c.off[off[i]]);
                return;
            }
            for (std::int32_t k = off[i]; k < off[i + 1]; ++k) {
                if (k != off[i]) {
                    std::memcpy(dst, sep.data(), static_cast<std::size_t>(sl));
                    dst += sl;
                }
                copy_bytes(dst, c.row(k), c.len(k));
                dst += c.len(k);
            }
        });
    share_nulls(out, v);
    return out;
}

template <class F>
dftu_series* split_impl(const dftu_series* v, const F& f,
                        std::string_view sep) {
    const std::int64_t n = f.n;
    const std::int64_t lo = f.lo();
    const std::size_t nbytes = static_cast<std::size_t>(f.hi() - lo);
    const std::uint8_t* p = f.data != nullptr ? f.data + lo : nullptr;
    const std::size_t sl = sep.size();
    Bits cand;
    if (sl != 0)
        cand = compute_bits(simd::BitKind::Eq, p, nbytes,
                            static_cast<std::uint8_t>(sep[0]), false);

    // Pass 1: where the separator occurs in each row, the part counts and
    // the bytes the parts hold.
    std::vector<std::size_t> hits;
    std::vector<std::int32_t> list_off(static_cast<std::size_t>(n + 1), 0);
    std::int64_t parts = 0, bytes = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        if (!f.is_null(i)) {
            const std::size_t s = static_cast<std::size_t>(f.off[i] - lo);
            const std::size_t e = static_cast<std::size_t>(f.off[i + 1] - lo);
            const std::size_t before = hits.size();
            std::size_t pos = s;
            while (sl != 0 && pos < e) {
                const std::size_t q =
                    find_sep(cand.bad.data(), p, pos, e, sep, false);
                if (q == simd::NPOS) break;
                hits.push_back(q);
                pos = q + sl;
            }
            const std::int64_t h =
                static_cast<std::int64_t>(hits.size() - before);
            parts += h + 1;
            bytes += static_cast<std::int64_t>(e - s) -
                     h * static_cast<std::int64_t>(sl);
            if (parts > std::numeric_limits<std::int32_t>::max())
                return nullptr;
        }
        list_off[static_cast<std::size_t>(i + 1)] =
            static_cast<std::int32_t>(parts);
    }
    if (bytes > std::numeric_limits<std::int32_t>::max()) return nullptr;

    // Pass 2: copy the parts.
    auto coff = Buffer::allocate(static_cast<std::size_t>(parts + 1) *
                                 sizeof(std::int32_t));
    auto cdata = Buffer::allocate(static_cast<std::size_t>(bytes));
    auto* co = reinterpret_cast<std::int32_t*>(coff->data());
    std::uint8_t* cd = cdata->data();
    std::int64_t at = 0, k = 0;
    co[0] = 0;
    std::size_t h = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        if (f.is_null(i)) continue;
        const std::size_t e = static_cast<std::size_t>(f.off[i + 1] - lo);
        std::size_t start = static_cast<std::size_t>(f.off[i] - lo);
        auto emit = [&](std::size_t from, std::size_t to) {
            copy_bytes(cd + at, p + from, static_cast<std::int64_t>(to - from));
            at += static_cast<std::int64_t>(to - from);
            co[++k] = static_cast<std::int32_t>(at);
        };
        while (h < hits.size() && hits[h] < e) {
            emit(start, hits[h]);
            start = hits[h] + sl;
            ++h;
        }
        emit(start, e);
    }
    return make_str_list(v, list_off, parts, std::move(coff), std::move(cdata));
}

template <class F>
dftu_series* partition_impl(const dftu_series* v, const F& f,
                            std::string_view sep, bool right) {
    const std::int64_t n = f.n;
    const std::int64_t lo = f.lo();
    const std::size_t nbytes = static_cast<std::size_t>(f.hi() - lo);
    const std::uint8_t* p = f.data != nullptr ? f.data + lo : nullptr;
    const std::size_t sl = sep.size();
    const Bits cand = compute_bits(simd::BitKind::Eq, p, nbytes,
                                   static_cast<std::uint8_t>(sep[0]), false);
    std::vector<std::size_t> at(static_cast<std::size_t>(n), simd::NPOS);
    std::vector<std::int32_t> list_off(static_cast<std::size_t>(n + 1), 0);
    std::int64_t parts = 0, bytes = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        if (!f.is_null(i)) {
            const std::size_t s = static_cast<std::size_t>(f.off[i] - lo);
            const std::size_t e = static_cast<std::size_t>(f.off[i + 1] - lo);
            at[static_cast<std::size_t>(i)] =
                find_sep(cand.bad.data(), p, s, e, sep, right);
            parts += 3;
            bytes += static_cast<std::int64_t>(e - s);
            if (parts > std::numeric_limits<std::int32_t>::max())
                return nullptr;
        }
        list_off[static_cast<std::size_t>(i + 1)] =
            static_cast<std::int32_t>(parts);
    }
    if (bytes > std::numeric_limits<std::int32_t>::max()) return nullptr;
    auto coff = Buffer::allocate(static_cast<std::size_t>(parts + 1) *
                                 sizeof(std::int32_t));
    auto cdata = Buffer::allocate(static_cast<std::size_t>(bytes));
    auto* co = reinterpret_cast<std::int32_t*>(coff->data());
    std::uint8_t* cd = cdata->data();
    std::int64_t w = 0, k = 0;
    co[0] = 0;
    auto put = [&](const std::uint8_t* src, std::int64_t len) {
        copy_bytes(cd + w, src, len);
        w += len;
        co[++k] = static_cast<std::int32_t>(w);
    };
    for (std::int64_t i = 0; i < n; ++i) {
        if (f.is_null(i)) continue;
        const std::size_t s = static_cast<std::size_t>(f.off[i] - lo);
        const std::size_t e = static_cast<std::size_t>(f.off[i + 1] - lo);
        const std::size_t q = at[static_cast<std::size_t>(i)];
        if (q == simd::NPOS) {
            // Python: (row, "", "") for partition, ("", "", row) for
            // rpartition.
            if (!right) {
                put(p + s, static_cast<std::int64_t>(e - s));
                put(p, 0);
                put(p, 0);
            } else {
                put(p, 0);
                put(p, 0);
                put(p + s, static_cast<std::int64_t>(e - s));
            }
            continue;
        }
        put(p + s, static_cast<std::int64_t>(q - s));
        put(p + q, static_cast<std::int64_t>(sl));
        put(p + q + sl, static_cast<std::int64_t>(e - q - sl));
    }
    return make_str_list(v, list_off, parts, std::move(coff), std::move(cdata));
}

template <class F>
dftu_series* rfind_impl(const dftu_series* v, const F& f,
                        std::string_view needle) {
    const std::int64_t lo = f.lo();
    const std::size_t nbytes = static_cast<std::size_t>(f.hi() - lo);
    const std::uint8_t* p = f.data != nullptr ? f.data + lo : nullptr;
    Bits cand;
    if (!needle.empty())
        cand = compute_bits(simd::BitKind::Eq, p, nbytes,
                            static_cast<std::uint8_t>(needle[0]), false);
    auto* out = new dftu_series();
    out->type = TypeId::Int64;
    out->encoding = Encoding::Flat;
    out->length = f.n;
    out->null_count = v->null_count;
    out->validity = v->validity;
    out->data = Buffer::allocate(buffer_bytes(TypeId::Int64, f.n));
    auto* r = reinterpret_cast<std::int64_t*>(out->data->data());
    for (std::int64_t i = 0; i < f.n; ++i) {
        if (f.is_null(i)) {
            r[i] = 0;
            continue;
        }
        const std::size_t s = static_cast<std::size_t>(f.off[i] - lo);
        const std::size_t e = static_cast<std::size_t>(f.off[i + 1] - lo);
        if (needle.empty()) {
            r[i] = static_cast<std::int64_t>(e - s);
            continue;
        }
        const std::size_t q = find_sep(cand.bad.data(), p, s, e, needle, true);
        r[i] = q == simd::NPOS ? -1 : static_cast<std::int64_t>(q - s);
    }
    return out;
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

}  // namespace

dftu_series* dftu_series_str_slice(const dftu_series* v, int64_t start,
                                   int64_t length) {
    if (!v || !is_string_kind(v->type)) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_str_slice(flat_v, start, length));
    return slice_impl(v, start, length);
}

dftu_series* dftu_series_str_pad_start(const dftu_series* v, int64_t width,
                                       char fill) {
    if (!v || !is_string_kind(v->type)) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v,
                      dftu_series_str_pad_start(flat_v, width, fill));
    return pad_impl(v, width, fill, 0);
}
dftu_series* dftu_series_str_pad_end(const dftu_series* v, int64_t width,
                                     char fill) {
    if (!v || !is_string_kind(v->type)) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_str_pad_end(flat_v, width, fill));
    return pad_impl(v, width, fill, 1);
}
dftu_series* dftu_series_str_center(const dftu_series* v, int64_t width,
                                    char fill) {
    if (!v || !is_string_kind(v->type)) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_str_center(flat_v, width, fill));
    return pad_impl(v, width, fill, 2);
}
dftu_series* dftu_series_str_zfill(const dftu_series* v, int64_t width) {
    if (!v || !is_string_kind(v->type)) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_str_zfill(flat_v, width));
    return zfill_impl(v, width);
}

dftu_series* dftu_series_str_split(const dftu_series* v, const char* sep,
                                   int32_t sep_len) {
    if (!v || !is_string_kind(v->type)) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_str_split(flat_v, sep, sep_len));
    // An empty separator is no split point: one part, the whole row.
    const std::string_view s_sep(sep, static_cast<std::size_t>(sep_len));
    return with_flat(v, [&](const auto& f) { return split_impl(v, f, s_sep); });
}

dftu_series* dftu_series_str_partition(const dftu_series* v, const char* sep,
                                       int32_t sep_len, int32_t from_right) {
    if (!v || !is_string_kind(v->type) || sep_len <= 0) return nullptr;
    DFTU_FLAT_OPERAND(
        v, flat_v, dftu_series_str_partition(flat_v, sep, sep_len, from_right));
    const std::string_view s_sep(sep, static_cast<std::size_t>(sep_len));
    return with_flat(v, [&](const auto& f) {
        return partition_impl(v, f, s_sep, from_right != 0);
    });
}

dftu_series* dftu_series_str_rfind(const dftu_series* v, const char* needle,
                                   int32_t needle_len) {
    if (!v || !is_string_kind(v->type)) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v,
                      dftu_series_str_rfind(flat_v, needle, needle_len));
    const std::string_view n(needle, static_cast<std::size_t>(needle_len));
    return with_flat(v, [&](const auto& f) { return rfind_impl(v, f, n); });
}

dftu_series* dftu_series_str_case(const dftu_series* v, int32_t op) {
    if (!v || !is_string_kind(v->type)) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_str_case(flat_v, op));
    switch (static_cast<dftu_str_case>(op)) {
        case DFTU_STR_CAPITALIZE:
            return case_impl(v, simd::CaseKind::Lower, true);
        case DFTU_STR_TITLE_CASE:
            return case_impl(v, simd::CaseKind::Title, false);
        case DFTU_STR_SWAPCASE:
            return case_impl(v, simd::CaseKind::Swapcase, false);
    }
    return nullptr;
}

dftu_series* dftu_series_str_is(const dftu_series* v, int32_t cls) {
    if (!v) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_str_is(flat_v, cls));
    return is_impl(v, cls);
}

dftu_series* dftu_series_str_count(const dftu_series* v, const char* pat,
                                   int32_t pat_len) {
    if (!v || !is_string_kind(v->type)) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_str_count(flat_v, pat, pat_len));
    std::string_view p(pat, static_cast<std::size_t>(pat_len));
    return int_transform(
        v, [p](std::string_view s) { return count_literal(s, p); });
}

dftu_series* dftu_series_str_remove_prefix(const dftu_series* v,
                                           const char* prefix, int32_t len) {
    if (!v || !is_string_kind(v->type)) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v,
                      dftu_series_str_remove_prefix(flat_v, prefix, len));
    return remove_affix_impl(
        v, std::string_view(prefix, static_cast<std::size_t>(len)), true);
}

dftu_series* dftu_series_str_remove_suffix(const dftu_series* v,
                                           const char* suffix, int32_t len) {
    if (!v || !is_string_kind(v->type)) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v,
                      dftu_series_str_remove_suffix(flat_v, suffix, len));
    return remove_affix_impl(
        v, std::string_view(suffix, static_cast<std::size_t>(len)), false);
}

dftu_series* dftu_series_str_repeat(const dftu_series* v, int64_t n) {
    if (!v || !is_string_kind(v->type) || n < 0) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_str_repeat(flat_v, n));
    return repeat_impl(v, n);
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
    if (wa && wb) return cat_impl<std::int64_t, std::int64_t>(a, b);
    if (wa) return cat_impl<std::int64_t, std::int32_t>(a, b);
    if (wb) return cat_impl<std::int32_t, std::int64_t>(a, b);
    return cat_impl<std::int32_t, std::int32_t>(a, b);
}

dftu_series* dftu_series_list_join(const dftu_series* v, const char* sep,
                                   int32_t sep_len) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_list_join(flat_v, sep, sep_len));
    if (!v || v->type != TypeId::List || v->encoding != Encoding::Flat)
        return nullptr;
    if (!dftu_series_offsets(v) || !v->child() ||
        !is_string_kind(v->child()->type))
        return nullptr;
    const dftu_series& child = *v->child();
    const std::string_view s(sep, static_cast<std::size_t>(sep_len));
    return is_wide_offset_type(child.type)
               ? join_impl<std::int64_t>(v, child, s)
               : join_impl<std::int32_t>(v, child, s);
}

dftu_series* dftu_series_str_findall(const dftu_series* v, const char* pattern,
                                     int32_t pattern_len) {
    if (!v || !is_string_kind(v->type)) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v,
                      dftu_series_str_findall(flat_v, pattern, pattern_len));
    const auto re = compiled(
        duql::compile_regex(pattern_text(pattern, pattern_len), false));
    if (!re) return nullptr;
    LimitedRows limited;
    dftu_series* out = list_transform(
        v, [&](std::string_view row, std::vector<std::string>& parts) {
            const auto r = duql::findall(
                *re, row, [&](std::string_view m) { parts.emplace_back(m); });
            if (r == duql::MatchResult::LIMIT) limited.add(row);
        });
    return limited.apply(out, v);
}
