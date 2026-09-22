#include <ankerl/unordered_dense.h>
#include <dftracer/utils/core/common/hash/hash.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/varwidth_offsets.h>
#include <dftracer/utils/dataframe/kernels/dictionary.h>
#include <dftracer/utils/dataframe/parallel.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dftracer::utils::dataframe {

Series dictionary_encode(const Series& v) {
    return Series{dftu_series_dictionary_encode(v.handle())};
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
    static ShortKey of(std::string_view s) {
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
        dftracer::utils::hash_combine(h,
                                      dftracer::utils::hash::splitmix64(k.a));
        dftracer::utils::hash_combine(h,
                                      dftracer::utils::hash::splitmix64(k.b));
        dftracer::utils::hash_combine(h, k.len);
        return h;
    }
};

template <class Off>
std::vector<std::int32_t> dictionary_encode_parallel(
    const Off* off, const char* data, std::int64_t n,
    std::vector<Off>& dict_offsets, std::string& dict_data) {
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
        ankerl::unordered_dense::map<ShortKey, std::int32_t, ShortKeyHash>
            shorts;
        ankerl::unordered_dense::map<std::string_view, std::int32_t> longs;
        std::vector<std::int64_t> first_row;  // per local code
    };
    auto at = [&](std::int64_t i) {
        return std::string_view(data + off[i],
                                static_cast<std::size_t>(off[i + 1] - off[i]));
    };
    std::vector<Local> local(static_cast<std::size_t>(nchunks));
    std::vector<std::int32_t> codes(static_cast<std::size_t>(n));
    parallel_for(n, DICT_ENCODE_GRAIN, [&](std::int64_t b, std::int64_t e) {
        Local& l = local[static_cast<std::size_t>(b / DICT_ENCODE_GRAIN)];
        for (std::int64_t i = b; i < e; ++i) {
            const std::string_view s = at(i);
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
    std::vector<std::pair<std::int64_t, std::string_view>> ordered;
    ankerl::unordered_dense::map<std::string_view, std::int64_t> first_seen;
    for (const Local& l : local)
        for (const std::int64_t row : l.first_row) {
            auto [it, fresh] = first_seen.try_emplace(at(row), row);
            if (!fresh && row < it->second) it->second = row;
        }
    ordered.reserve(first_seen.size());
    for (const auto& [s, row] : first_seen) ordered.emplace_back(row, s);
    std::sort(ordered.begin(), ordered.end());
    ankerl::unordered_dense::map<std::string_view, std::int32_t> code_of;
    code_of.reserve(ordered.size());
    dict_offsets.assign(1, 0);
    for (std::size_t c = 0; c < ordered.size(); ++c) {
        const std::string_view s = ordered[c].second;
        code_of.emplace(s, static_cast<std::int32_t>(c));
        dict_data.append(s);
        dict_offsets.push_back(static_cast<Off>(dict_data.size()));
    }
    std::vector<std::vector<std::int32_t>> remap(
        static_cast<std::size_t>(nchunks));
    for (std::size_t c = 0; c < local.size(); ++c) {
        remap[c].resize(local[c].first_row.size());
        for (std::size_t lc = 0; lc < local[c].first_row.size(); ++lc)
            remap[c][lc] = code_of.at(at(local[c].first_row[lc]));
    }
    parallel_for(n, DICT_ENCODE_GRAIN, [&](std::int64_t b, std::int64_t e) {
        const std::vector<std::int32_t>& r =
            remap[static_cast<std::size_t>(b / DICT_ENCODE_GRAIN)];
        for (std::int64_t i = b; i < e; ++i)
            codes[static_cast<std::size_t>(i)] =
                r[static_cast<std::size_t>(codes[static_cast<std::size_t>(i)])];
    });
    return codes;
}

template <class Off>
dftu_series* dictionary_encode_w(const dftu_series* v) {
    const Off* off = reinterpret_cast<const Off*>(offsets_of<Off>(*v)->data());
    const char* data = reinterpret_cast<const char*>(v->data->data());

    std::vector<std::int32_t> codes;
    std::vector<Off> dict_offsets{0};
    std::string dict_data;
    if (parallel_backend_installed() && v->length > DICT_ENCODE_GRAIN) {
        codes = dictionary_encode_parallel<Off>(off, data, v->length,
                                                dict_offsets, dict_data);
    } else {
        codes.resize(static_cast<std::size_t>(v->length));
        std::unordered_map<std::string_view, std::int32_t> codes_of;
        for (std::int64_t i = 0; i < v->length; ++i) {
            std::string_view s(data + off[i],
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
    offsets_of<Off>(*dict) = Buffer::allocate(off_bytes);
    std::memcpy(offsets_of<Off>(*dict)->data(), dict_offsets.data(), off_bytes);
    dict->data = Buffer::allocate(dict_data.size());
    if (!dict_data.empty())
        std::memcpy(dict->data->data(), dict_data.data(), dict_data.size());

    auto* out = new dftu_series();
    out->type = v->type;
    out->encoding = Encoding::Dictionary;
    out->length = v->length;
    out->null_count = v->null_count;
    out->validity = v->validity;
    out->data = Buffer::allocate(codes.size() * sizeof(std::int32_t));
    std::memcpy(out->data->data(), codes.data(),
                codes.size() * sizeof(std::int32_t));
    out->child = std::shared_ptr<dftu_series>(dict);
    return out;
}

}  // namespace

}  // namespace dftracer::utils::dataframe

dftu_series* dftu_series_dictionary_encode(const dftu_series* v) {
    using namespace dftracer::utils::dataframe;
    if (!v) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_dictionary_encode(flat_v));
    const TypeId base = narrow_varwidth_type(v->type);
    if (base != TypeId::String && base != TypeId::Binary) return nullptr;
    const bool wide = is_wide_offset_type(v->type);
    if ((wide && !v->offsets64) || (!wide && !v->offsets) || !v->data)
        return nullptr;
    return wide ? dictionary_encode_w<std::int64_t>(v)
                : dictionary_encode_w<std::int32_t>(v);
}
