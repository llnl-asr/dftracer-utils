// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/varwidth_offsets.h>
#include <dftracer/utils/dataframe/kernels/dictionary.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/dstd/hash/hash.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace dftracer::utils::dataframe {

Series dictionary_encode(const Series& v) {
    return Series{dftu_series_dictionary_encode(v.handle())};
}

Series dictionary_from_codes(dstd::Span<const std::int32_t> codes,
                             Series&& values, const std::uint8_t* validity) {
    Series out =
        Series::flat(TypeId::Int32, codes.data(),
                     static_cast<std::int64_t>(codes.size()), validity);
    dftu_series* h = out.handle();
    h->type = TypeId::String;
    h->params = TypeParams{};
    h->encoding = Encoding::Dictionary;
    h->set_child(adopt_series(values.release()));
    return out;
}

namespace {

// Rows per parallel_encode chunk; also the threshold below which the plain
// single-pass build runs (fan-out plus a merge pass is pure overhead there).
constexpr std::int64_t DICT_ENCODE_GRAIN = 1 << 16;

// Same output as the single-pass build (stable first-seen dict order, same
// codes), computed with two parallel passes: each chunk builds a local
// first-seen map, then the chunks are merged by picking, for each distinct
// value, the smallest global row index it first appeared at - that ordering
// is exactly what the serial single pass would have assigned. `Off` is the
// source column's offset width (int32 for String/Binary, int64 for
// LargeString/LargeBinary); the dictionary's own offsets are built at the
// same width, so a DICTIONARY-encoded Large column's child is still a
// Large-typed, wide-offset column downstream ops can read.
// A string of at most 16 bytes as two little-endian words (zero padded)
// and its length, so two strings compare as three integers.
struct ShortKey {
    std::uint64_t a = 0, b = 0;
    std::uint32_t len = 0;
    static ShortKey of(dstd::StringView s) {
        ShortKey k;
        k.len = static_cast<std::uint32_t>(s.size());
        if (s.size() >= 8) {
            std::memcpy(&k.a, s.data(), 8);
            std::memcpy(&k.b, s.data() + 8, s.size() - 8);
        } else {
            std::memcpy(&k.a, s.data(), s.size());
        }
        return k;
    }
    bool operator==(const ShortKey& o) const {
        return a == o.a && b == o.b && len == o.len;
    }
};
struct ShortKeyHash {
    using is_avalanching = void;
    std::uint64_t operator()(const ShortKey& k) const noexcept {
        std::size_t h = 0;
        dftracer::utils::dstd::hash_combine(
            h, dftracer::utils::dstd::hash::splitmix64(k.a));
        dftracer::utils::dstd::hash_combine(
            h, dftracer::utils::dstd::hash::splitmix64(k.b));
        dftracer::utils::dstd::hash_combine(h, k.len);
        return h;
    }
};

template <class Off>
dstd::Vector<std::int32_t> dictionary_encode_parallel(
    const Off* off, dstd::StringView data, std::int64_t n,
    dstd::Vector<Off>& dict_offsets, dstd::String& dict_data) {
    // One hash per row: each chunk assigns local codes in its own flat map
    // and records them; the chunk dictionaries are then merged into the
    // global one (ordered by first appearance, as the serial pass would
    // number them), and a per-chunk remap turns local codes into global
    // ones without touching the strings again.
    const std::int64_t nchunks =
        (n + DICT_ENCODE_GRAIN - 1) / DICT_ENCODE_GRAIN;
    // A string of up to 16 bytes is keyed by its bytes as two words plus
    // its length (a whole-word hash and compare, no memcmp call); a longer
    // one by its bytes. Both share a chunk's local code space.
    struct Local {
        dstd::HashMap<ShortKey, std::int32_t, ShortKeyHash> shorts;
        dstd::HashMap<dstd::StringView, std::int32_t> longs;
        dstd::Vector<std::int64_t> first_row;  // per local code
    };
    auto at = [&](std::int64_t i) {
        return dstd::StringView(data.data() + off[i],
                                static_cast<std::size_t>(off[i + 1] - off[i]));
    };
    dstd::Vector<Local> local(static_cast<std::size_t>(nchunks));
    dstd::Vector<std::int32_t> codes(static_cast<std::size_t>(n));
    parallel_for(n, DICT_ENCODE_GRAIN, [&](std::int64_t b, std::int64_t e) {
        Local& l = local[static_cast<std::size_t>(b / DICT_ENCODE_GRAIN)];
        for (std::int64_t i = b; i < e; ++i) {
            const dstd::StringView s = at(i);
            const auto next = static_cast<std::int32_t>(l.first_row.size());
            std::int32_t code;
            bool fresh;
            if (s.size() <= 16) {
                auto r = l.shorts.try_emplace(ShortKey::of(s), next);
                code = r.first->second;
                fresh = r.second;
            } else {
                auto r = l.longs.try_emplace(s, next);
                code = r.first->second;
                fresh = r.second;
            }
            if (fresh) l.first_row.push_back(i);
            codes[static_cast<std::size_t>(i)] = code;
        }
    });
    // Global numbering by first appearance.
    dstd::Vector<dstd::Pair<std::int64_t, dstd::StringView>> ordered;
    dstd::HashMap<dstd::StringView, std::int64_t> first_seen;
    for (const Local& l : local)
        for (const std::int64_t row : l.first_row) {
            auto [it, fresh] = first_seen.try_emplace(at(row), row);
            if (!fresh && row < it->second) it->second = row;
        }
    ordered.reserve(first_seen.size());
    for (const auto& [s, row] : first_seen) ordered.emplace_back(row, s);
    std::sort(ordered.begin(), ordered.end());
    dstd::HashMap<dstd::StringView, std::int32_t> code_of;
    code_of.reserve(ordered.size());
    dict_offsets.assign(1, 0);
    for (std::size_t c = 0; c < ordered.size(); ++c) {
        const dstd::StringView s = ordered[c].second;
        code_of.emplace(s, static_cast<std::int32_t>(c));
        dict_data.append(s);
        dict_offsets.push_back(static_cast<Off>(dict_data.size()));
    }
    dstd::Vector<dstd::Vector<std::int32_t>> remap(
        static_cast<std::size_t>(nchunks));
    for (std::size_t c = 0; c < local.size(); ++c) {
        remap[c].resize(local[c].first_row.size());
        for (std::size_t lc = 0; lc < local[c].first_row.size(); ++lc)
            remap[c][lc] = code_of.at(at(local[c].first_row[lc]));
    }
    parallel_for(n, DICT_ENCODE_GRAIN, [&](std::int64_t b, std::int64_t e) {
        const dstd::Vector<std::int32_t>& r =
            remap[static_cast<std::size_t>(b / DICT_ENCODE_GRAIN)];
        for (std::int64_t i = b; i < e; ++i)
            codes[static_cast<std::size_t>(i)] =
                r[static_cast<std::size_t>(codes[static_cast<std::size_t>(i)])];
    });
    return codes;
}

template <class Off>
dftu_series* dictionary_encode_w(const dftu_series* v) {
    const Off* off = reinterpret_cast<const Off*>(offsets_of<Off>(*v).data());
    const dstd::StringView data(reinterpret_cast<const char*>(v->data.data()),
                                v->data.size());

    dstd::Vector<std::int32_t> codes;
    dstd::Vector<Off> dict_offsets{0};
    dstd::String dict_data;
    if (parallel_backend_installed() && v->length > DICT_ENCODE_GRAIN) {
        codes = dictionary_encode_parallel<Off>(off, data, v->length,
                                                dict_offsets, dict_data);
    } else {
        codes.resize(static_cast<std::size_t>(v->length));
        dstd::HashMap<dstd::StringView, std::int32_t> codes_of;
        for (std::int64_t i = 0; i < v->length; ++i) {
            dstd::StringView s(data.data() + off[i],
                               static_cast<std::size_t>(off[i + 1] - off[i]));
            auto it = codes_of.find(s);
            std::int32_t code;
            if (it == codes_of.end()) {
                code = static_cast<std::int32_t>(codes_of.size());
                codes_of.emplace(s, code);
                dict_data.append(s);
                dict_offsets.push_back(static_cast<Off>(dict_data.size()));
            } else {
                code = it->second;
            }
            codes[static_cast<std::size_t>(i)] = code;
        }
    }

    auto* dict = new dftu_series();
    dict->type = v->type;
    dict->encoding = Encoding::Flat;
    dict->length = static_cast<std::int64_t>(dict_offsets.size() - 1);
    std::size_t off_bytes = dict_offsets.size() * sizeof(Off);
    offsets_of<Off>(*dict) = new_buffer(off_bytes);
    std::memcpy(writable(offsets_of<Off>(*dict)), dict_offsets.data(),
                off_bytes);
    dict->data = new_buffer(dict_data.size());
    if (!dict_data.empty())
        std::memcpy(writable(dict->data), dict_data.data(), dict_data.size());

    auto* out = new dftu_series();
    out->type = v->type;
    out->encoding = Encoding::Dictionary;
    out->length = v->length;
    out->null_count = v->null_count;
    out->validity = v->validity;
    out->data = new_buffer(codes.size() * sizeof(std::int32_t));
    std::memcpy(writable(out->data), codes.data(),
                codes.size() * sizeof(std::int32_t));
    out->set_child(adopt_series(dict));
    return out;
}

}  // namespace

}  // namespace dftracer::utils::dataframe

namespace {

// A FLAT Bool column of `values[codes[i]]`, null where `v` is null at i or
// the entry is null. One table read per row; the table has one byte per entry.
dftu_series* expand_bool_dictionary(const dftu_series& v,
                                    const dftu_series& values) {
    using namespace dftracer::utils::dataframe;
    constexpr std::uint8_t BIT = 1;
    constexpr std::uint8_t VALID = 2;
    const std::int64_t n = v.length;
    const std::uint8_t* bits =
        values.data.block() ? values.data.data() : nullptr;
    const std::uint8_t* entry_valid =
        values.null_count > 0 && values.validity.block()
            ? values.validity.data()
            : nullptr;
    dftracer::utils::dstd::Vector<std::uint8_t> table(
        static_cast<std::size_t>(values.length));
    for (std::int64_t k = 0; k < values.length; ++k) {
        const bool bit = bits && ((bits[k >> 3] >> (k & 7)) & 1);
        const bool valid =
            !entry_valid || ((entry_valid[k >> 3] >> (k & 7)) & 1);
        table[static_cast<std::size_t>(k)] =
            static_cast<std::uint8_t>((bit ? BIT : 0) | (valid ? VALID : 0));
    }
    const std::uint8_t* own = v.validity.block() ? v.validity.data() : nullptr;
    const auto* codes = reinterpret_cast<const std::int32_t*>(v.data.data());
    const std::size_t bytes = buffer_bytes(TypeId::Bool, n);

    auto* out = new dftu_series();
    out->type = TypeId::Bool;
    out->encoding = Encoding::Flat;
    out->length = n;
    out->data = new_buffer(bytes == 0 ? 1 : bytes);
    std::memset(writable(out->data), 0, out->data.size());
    BufferView validity;
    if (own || entry_valid) {
        validity = new_buffer(bytes == 0 ? 1 : bytes);
        std::memset(writable(validity), 0, validity.size());
    }
    std::uint8_t* ob = writable(out->data);
    std::uint8_t* ov = validity.block() ? writable(validity) : nullptr;
    // Whole bytes per task (8 rows), so no two tasks write one byte.
    const auto nbytes = static_cast<std::int64_t>(bytes);
    out->null_count = parallel_reduce<std::int64_t>(
        nbytes, std::int64_t{1} << 13, std::int64_t{0},
        [&](std::int64_t b0, std::int64_t b1) {
            std::int64_t nulls = 0;
            for (std::int64_t i = b0 * 8; i < std::min(n, b1 * 8); ++i) {
                if (own && !((own[i >> 3] >> (i & 7)) & 1)) {
                    ++nulls;
                    continue;
                }
                const std::uint8_t t =
                    table[static_cast<std::size_t>(codes[i])];
                if (!(t & VALID)) {
                    ++nulls;
                    continue;
                }
                const auto mask = static_cast<std::uint8_t>(1u << (i & 7));
                if (t & BIT) ob[i >> 3] |= mask;
                if (ov) ov[i >> 3] |= mask;
            }
            return nulls;
        },
        [](std::int64_t a, std::int64_t b) { return a + b; });
    if (out->null_count > 0) out->validity = std::move(validity);
    return out;
}

}  // namespace

bool dftu_dictionary_codes_in_range(const dftu_series& v) {
    if (v.length == 0) return true;
    const auto entries = static_cast<std::uint32_t>(v.child()->length);
    const auto* codes = reinterpret_cast<const std::int32_t*>(v.data.data());
    const std::uint8_t* own =
        v.null_count > 0 && v.validity.block() ? v.validity.data() : nullptr;
    std::uint32_t outside = 0;
    if (!own) {
        for (std::int64_t i = 0; i < v.length; ++i)
            outside |= static_cast<std::uint32_t>(codes[i]) >= entries;
        return outside == 0;
    }
    for (std::int64_t i = 0; i < v.length; ++i)
        if (((own[i >> 3] >> (i & 7)) & 1) &&
            static_cast<std::uint32_t>(codes[i]) >= entries)
            return false;
    return true;
}

dftu_series* dftu_dictionary_elementwise(const dftu_series& v,
                                         dftu_series* values) {
    using namespace dftracer::utils::dataframe;
    if (!values) return nullptr;
    auto owned = adopt_series(values);
    if (owned->encoding != Encoding::Flat || owned->length != v.child()->length)
        return nullptr;
    if (owned->type == TypeId::Bool) return expand_bool_dictionary(v, *owned);
    auto* out = new dftu_series();
    adopt_type_from(*out, *owned);
    out->encoding = Encoding::Dictionary;
    out->length = v.length;
    out->null_count = v.null_count;
    out->validity = v.validity;
    out->data = v.data;
    out->set_child(std::move(owned));
    return out;
}

dftu_series* dftu_series_dictionary_encode(const dftu_series* v) {
    using namespace dftracer::utils::dataframe;
    if (!v) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_dictionary_encode(flat_v));
    const TypeId base = narrow_varwidth_type(v->type);
    if (base != TypeId::String && base != TypeId::Binary) return nullptr;
    const bool wide = is_wide_offset_type(v->type);
    if (!v->offsets.block() || !v->data.block()) return nullptr;
    return wide ? dictionary_encode_w<std::int64_t>(v)
                : dictionary_encode_w<std::int32_t>(v);
}
