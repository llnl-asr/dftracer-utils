#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_STRING_READER_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_STRING_READER_H

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/dataframe/buffer.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/dataframe/types.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <string_view>
#include <vector>

namespace dftracer::utils::dataframe {

inline bool is_string_column(const dftu_series& v) {
    const TypeId n = narrow_varwidth_type(v.type);
    return n == TypeId::String || n == TypeId::Binary;
}

/// Row `i` of a String/Binary(/Large) column in any encoding, read in place.
/// Returns false for a null row (own bitmap, a Selection outer-fill index, or
/// a null entry below a Dictionary or Selection). `out` points into the
/// column's buffers.
inline bool str_at(const dftu_series* c, std::int64_t i,
                   std::string_view& out) {
    for (;;) {
        if (c->validity && !((c->validity->data()[i >> 3] >> (i & 7)) & 1))
            return false;
        switch (c->encoding) {
            case Encoding::Flat: {
                const char* d =
                    c->data ? reinterpret_cast<const char*>(c->data->data())
                            : nullptr;
                if (!c->offsets) return false;
                if (c->wide_offsets()) {
                    const auto* off = reinterpret_cast<const std::int64_t*>(
                        c->offsets->data());
                    out = std::string_view(
                        d + off[i],
                        static_cast<std::size_t>(off[i + 1] - off[i]));
                } else {
                    const auto* off = reinterpret_cast<const std::int32_t*>(
                        c->offsets->data());
                    out = std::string_view(
                        d + off[i],
                        static_cast<std::size_t>(off[i + 1] - off[i]));
                }
                return true;
            }
            case Encoding::View: {
                const std::uint8_t* view = c->data->data() + i * 16;
                std::int32_t size;
                std::memcpy(&size, view, sizeof(size));
                if (size <= 12) {
                    out = std::string_view(
                        reinterpret_cast<const char*>(view + 4),
                        static_cast<std::size_t>(size));
                    return true;
                }
                std::int32_t index, offset;
                std::memcpy(&index, view + 8, sizeof(index));
                std::memcpy(&offset, view + 12, sizeof(offset));
                out = std::string_view(
                    reinterpret_cast<const char*>(
                        (*c->blobs)[static_cast<std::size_t>(index)]->data() +
                        offset),
                    static_cast<std::size_t>(size));
                return true;
            }
            case Encoding::Dictionary:
                i = reinterpret_cast<const std::int32_t*>(c->data->data())[i];
                c = c->child().get();
                break;
            case Encoding::Selection: {
                const std::int64_t idx =
                    reinterpret_cast<const std::int64_t*>(c->data->data())[i];
                if (idx < 0) return false;
                i = idx;
                c = c->child().get();
                break;
            }
            case Encoding::Chunked: {
                std::int64_t local = 0;
                c = &c->chunk_at(i, local);
                i = local;
                break;
            }
            default:
                return false;
        }
    }
}

/// Rank of each row of a Dictionary string column in byte order: equal
/// values share a rank, a null row (own bitmap, null entry, bad code) is -1.
/// `nranks` is the number of distinct values. False when `v` is not a
/// Dictionary with a child.
inline bool dictionary_ranks(const dftu_series* v,
                             std::vector<std::int32_t>& per_row,
                             std::int32_t& nranks) {
    if (v->encoding != Encoding::Dictionary || !v->child()) return false;
    const dftu_series* dict = v->child().get();
    const auto k = static_cast<std::size_t>(dict->length);
    std::vector<std::string_view> vals(k);
    std::vector<std::int32_t> live;
    live.reserve(k);
    for (std::size_t j = 0; j < k; ++j)
        if (str_at(dict, static_cast<std::int64_t>(j), vals[j]))
            live.push_back(static_cast<std::int32_t>(j));
    std::sort(live.begin(), live.end(), [&](std::int32_t a, std::int32_t b) {
        return vals[static_cast<std::size_t>(a)] <
               vals[static_cast<std::size_t>(b)];
    });
    std::vector<std::int32_t> code_rank(k, -1);
    nranks = 0;
    for (std::size_t j = 0; j < live.size(); ++j) {
        if (j > 0 && vals[static_cast<std::size_t>(live[j])] !=
                         vals[static_cast<std::size_t>(live[j - 1])])
            ++nranks;
        code_rank[static_cast<std::size_t>(live[j])] = nranks;
    }
    if (!live.empty()) ++nranks;
    const auto* codes = reinterpret_cast<const std::int32_t*>(v->data->data());
    per_row.resize(static_cast<std::size_t>(v->length));
    for (std::int64_t i = 0; i < v->length; ++i) {
        const bool valid =
            !v->validity || ((v->validity->data()[i >> 3] >> (i & 7)) & 1);
        per_row[static_cast<std::size_t>(i)] =
            valid && static_cast<std::uint32_t>(codes[i]) < k
                ? code_rank[static_cast<std::size_t>(codes[i])]
                : -1;
    }
    return true;
}

/// Ranks of a string column in any encoding, as `dictionary_ranks` gives for
/// a Dictionary. Any other encoding is deduplicated in one pass, and false is
/// returned once the distinct values pass a quarter of the rows, where a
/// compare sort is the better path.
inline bool string_ranks(const dftu_series* v,
                         std::vector<std::int32_t>& per_row,
                         std::int32_t& nranks) {
    if (v->encoding == Encoding::Dictionary && v->child())
        return dictionary_ranks(v, per_row, nranks);
    const std::int64_t n = v->length;
    const std::int64_t cap = n / 4;
    ankerl::unordered_dense::map<std::string_view, std::int32_t> seen;
    std::vector<std::string_view> vals;
    per_row.resize(static_cast<std::size_t>(n));
    std::string_view s;
    for (std::int64_t i = 0; i < n; ++i) {
        if (!str_at(v, i, s)) {
            per_row[static_cast<std::size_t>(i)] = -1;
            continue;
        }
        auto [it, fresh] =
            seen.try_emplace(s, static_cast<std::int32_t>(vals.size()));
        if (fresh) {
            if (static_cast<std::int64_t>(vals.size()) >= cap) {
                per_row.clear();
                return false;
            }
            vals.push_back(s);
        }
        per_row[static_cast<std::size_t>(i)] = it->second;
    }
    std::vector<std::int32_t> by_bytes(vals.size());
    std::iota(by_bytes.begin(), by_bytes.end(), 0);
    std::sort(by_bytes.begin(), by_bytes.end(),
              [&](std::int32_t a, std::int32_t b) {
                  return vals[static_cast<std::size_t>(a)] <
                         vals[static_cast<std::size_t>(b)];
              });
    std::vector<std::int32_t> code_rank(vals.size());
    for (std::size_t j = 0; j < by_bytes.size(); ++j)
        code_rank[static_cast<std::size_t>(by_bytes[j])] =
            static_cast<std::int32_t>(j);
    nranks = static_cast<std::int32_t>(vals.size());
    for (std::int32_t& r : per_row)
        if (r >= 0) r = code_rank[static_cast<std::size_t>(r)];
    return true;
}

/// Stable row order by `ranks` (-1 sorts last in both directions): one
/// histogram pass and one placement pass.
inline void counting_order(const std::vector<std::int32_t>& ranks,
                           std::int32_t nranks, bool descending,
                           std::int64_t* order) {
    const auto k = static_cast<std::size_t>(nranks);
    std::vector<std::int64_t> start(k + 2, 0);
    auto bucket = [&](std::int32_t r) -> std::size_t {
        return r < 0
                   ? k
                   : static_cast<std::size_t>(descending ? nranks - 1 - r : r);
    };
    for (std::int32_t r : ranks) ++start[bucket(r) + 1];
    for (std::size_t b = 1; b < start.size(); ++b) start[b] += start[b - 1];
    for (std::size_t i = 0; i < ranks.size(); ++i)
        order[start[bucket(ranks[i])]++] = static_cast<std::int64_t>(i);
}

/// Gives `out` the nulls of `v`: shared when the own bitmap is the whole
/// story, rebuilt row by row when a Selection or a null dictionary entry
/// decides them.
inline void adopt_string_nulls(dftu_series& out, const dftu_series& v) {
    if (!v.rowwise_nulls()) {
        out.validity = v.validity;
        out.null_count = v.null_count;
        return;
    }
    const std::size_t bytes = buffer_bytes(TypeId::Bool, v.length);
    auto valid = Buffer::allocate(bytes);
    std::memset(valid->data(), 0, bytes);
    std::int64_t nulls = 0;
    std::string_view s;
    for (std::int64_t i = 0; i < v.length; ++i) {
        if (str_at(&v, i, s))
            valid->data()[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        else
            ++nulls;
    }
    out.validity = nulls ? std::move(valid) : nullptr;
    out.null_count = nulls;
}

/// Rows of `v` (a string column in any encoding) where `pred(string_view)`
/// holds, as a Bool column carrying `v`'s nulls. A Dictionary runs `pred` once
/// per entry. Null on a non-string input.
template <class Pred>
dftu_series* string_mask(const dftu_series* v, Pred pred) {
    DFTU_PER_CHUNK(v, string_mask, pred);
    if (!is_string_column(*v)) return nullptr;
    auto* out = new dftu_series();
    out->type = TypeId::Bool;
    out->encoding = Encoding::Flat;
    out->length = v->length;
    const std::size_t bytes = buffer_bytes(TypeId::Bool, v->length);
    out->data = Buffer::allocate(bytes);
    std::memset(out->data->data(), 0, bytes);
    std::uint8_t* bits = out->data->data();
    auto set = [&](std::int64_t i) {
        bits[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    };
    constexpr std::int64_t GRAIN = std::int64_t{1} << 15;
    if (v->encoding == Encoding::Dictionary && v->child()) {
        const dftu_series* dict = v->child().get();
        std::vector<char> hit(static_cast<std::size_t>(dict->length), 0);
        std::string_view s;
        for (std::int64_t k = 0; k < dict->length; ++k)
            hit[static_cast<std::size_t>(k)] = str_at(dict, k, s) && pred(s);
        const auto* codes =
            reinterpret_cast<const std::int32_t*>(v->data->data());
        for (std::int64_t i = 0; i < v->length; ++i)
            if (static_cast<std::uint32_t>(codes[i]) < hit.size() &&
                hit[static_cast<std::size_t>(codes[i])])
                set(i);
    } else if (v->encoding == Encoding::Flat && v->offsets &&
               !v->wide_offsets()) {
        const auto* off =
            reinterpret_cast<const std::int32_t*>(v->offsets->data());
        const char* d =
            v->data ? reinterpret_cast<const char*>(v->data->data()) : nullptr;
        parallel_for(v->length, GRAIN, [&](std::int64_t b, std::int64_t e) {
            for (std::int64_t i = b; i < e; ++i)
                if (pred(std::string_view(
                        d + off[i],
                        static_cast<std::size_t>(off[i + 1] - off[i]))))
                    set(i);
        });
    } else {
        parallel_for(v->length, GRAIN, [&](std::int64_t b, std::int64_t e) {
            std::string_view s;
            for (std::int64_t i = b; i < e; ++i)
                if (str_at(v, i, s) && pred(s)) set(i);
        });
    }
    adopt_string_nulls(*out, *v);
    if (out->validity)
        for (std::size_t k = 0; k < bytes; ++k)
            bits[k] &= out->validity->data()[k];
    return out;
}

/// An Int64 column of `fn(string_view)` per row of `v`, null where `v` is
/// null; a Dictionary runs `fn` once per entry.
template <class Fn>
dftu_series* string_int64(const dftu_series* v, Fn fn) {
    DFTU_PER_CHUNK(v, string_int64, fn);
    if (!is_string_column(*v)) return nullptr;
    auto* out = new dftu_series();
    out->type = TypeId::Int64;
    out->encoding = Encoding::Flat;
    out->length = v->length;
    out->data = Buffer::allocate(buffer_bytes(TypeId::Int64, v->length));
    auto* vals = reinterpret_cast<std::int64_t*>(out->data->data());
    std::string_view s;
    if (v->encoding == Encoding::Dictionary && v->child()) {
        const dftu_series* dict = v->child().get();
        std::vector<std::int64_t> per(static_cast<std::size_t>(dict->length),
                                      0);
        for (std::int64_t k = 0; k < dict->length; ++k)
            if (str_at(dict, k, s)) per[static_cast<std::size_t>(k)] = fn(s);
        const auto* codes =
            reinterpret_cast<const std::int32_t*>(v->data->data());
        for (std::int64_t i = 0; i < v->length; ++i)
            vals[i] = static_cast<std::uint32_t>(codes[i]) < per.size()
                          ? per[static_cast<std::size_t>(codes[i])]
                          : 0;
    } else {
        for (std::int64_t i = 0; i < v->length; ++i)
            vals[i] = str_at(v, i, s) ? fn(s) : 0;
    }
    adopt_string_nulls(*out, *v);
    return out;
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_STRING_READER_H
