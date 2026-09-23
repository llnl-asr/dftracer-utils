#include <dftracer/utils/core/common/hash/hash.h>
#include <dftracer/utils/dataframe/agg/detail.h>
#include <dftracer/utils/dataframe/internal/column_data.h>  // dftu_series
#include <dftracer/utils/dataframe/internal/column_read.h>  // read_i64/u64/f64
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/dataframe/sketch.h>  // DDSketch, sketch_bucket_keys

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::dataframe {

// The value domain a column accumulates in - fixes both which FieldStat::add
// overload runs (keeping integers exact) and the finalized Sum/Min/Max column
// type. Derived once from the column type, never per row, so a group with no
// present rows still lands in the same output column as its populated peers.
FieldStatDomain col_domain(TypeId t) {
    switch (t) {
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
        case TypeId::Uint64:
            return FieldStatDomain::U64;
        case TypeId::Float16:
        case TypeId::Float32:
        case TypeId::Float64:
            return FieldStatDomain::F64;
        default:
            return FieldStatDomain::I64;
    }
}

// Types group_of can key on: String/Binary/LargeString/LargeBinary/
// FixedSizeBinary/Decimal128/Decimal256 via the bytes path (length-prefixed
// or fixed-width raw bytes into the key buffer), the rest read exactly by
// read_bits (Float16 through the F64 domain above, promoted to double so its
// key bits stay an injective function of the half bit pattern - see
// read_f64's exact Float16 decode). Anything else would key every row on the
// same zero and collapse the batch into one group. No default, so a new
// TypeId lands here.
static bool is_group_key_type(TypeId t) {
    switch (t) {
        case TypeId::Bool:
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
        case TypeId::Uint64:
        case TypeId::Float16:
        case TypeId::Float32:
        case TypeId::Float64:
        case TypeId::Date32:
        case TypeId::Date64:
        case TypeId::Time32:
        case TypeId::Time64:
        case TypeId::Timestamp:
        case TypeId::Duration:
        case TypeId::String:
        case TypeId::Binary:
        case TypeId::LargeString:
        case TypeId::LargeBinary:
        case TypeId::FixedSizeBinary:
        case TypeId::Decimal128:
        case TypeId::Decimal256:
            return true;
        case TypeId::Unknown:
        case TypeId::List:
        case TypeId::Struct:
        case TypeId::LargeList:
        case TypeId::FixedSizeList:
        case TypeId::Map:
            return false;
    }
    return false;
}

// A group key's byte-domain storage kind: which of skey_cols (bytes) /
// ikey_cols (int64 read_bits) it keys through.
bool is_bytes_key_type(TypeId t) {
    return t == TypeId::String || t == TypeId::Binary ||
           t == TypeId::LargeString || t == TypeId::LargeBinary ||
           t == TypeId::FixedSizeBinary || t == TypeId::Decimal128 ||
           t == TypeId::Decimal256;
}

// Raw value bits for a fixed-width cell, reinterpreted on finalize by the
// column's domain. Keeps first/last exact for every numeric type.
std::uint64_t read_bits(const Series& c, std::int64_t i, FieldStatDomain d) {
    switch (d) {
        case FieldStatDomain::F64:
            return std::bit_cast<std::uint64_t>(read_f64(c, i));
        case FieldStatDomain::U64:
            return read_u64(c, i);
        default:
            return std::bit_cast<std::uint64_t>(read_i64(c, i));
    }
}

namespace {

double read_as_double(const Series& c, std::int64_t i, FieldStatDomain d) {
    switch (d) {
        case FieldStatDomain::F64:
            return read_f64(c, i);
        case FieldStatDomain::U64:
            return static_cast<double>(read_u64(c, i));
        default:
            return static_cast<double>(read_i64(c, i));
    }
}

// Exact occupancy-endpoint read: an integer column keeps its raw u64/i64 so a
// timestamp above 2^53 stays exact; a Float64 column goes through double.
std::uint64_t read_u64_exact(const Series& c, std::int64_t i,
                             FieldStatDomain d) {
    switch (d) {
        case FieldStatDomain::F64:
            return static_cast<std::uint64_t>(read_f64(c, i));
        case FieldStatDomain::U64:
            return read_u64(c, i);
        default:
            return static_cast<std::uint64_t>(read_i64(c, i));
    }
}

void fs_add(FieldStat& fs, const Series& c, std::int64_t i, FieldStatDomain d) {
    switch (d) {
        case FieldStatDomain::F64:
            fs.add(read_f64(c, i));
            break;
        case FieldStatDomain::U64:
            fs.add(read_u64(c, i));
            break;
        default:
            fs.add(read_i64(c, i));
    }
}

// Generic string form of a cell, for ArgMax's repr and SetUnion's values:
// matches the View's to_str (std::to_string for numbers, the raw text for a
// String column) so the two engines agree on ArgMax/SetUnion output.
std::string cell_repr(const Series& c, std::int64_t i) {
    if (c.type() == TypeId::String) return std::string(c.string_at(i));
    switch (col_domain(c.type())) {
        case FieldStatDomain::F64:
            return std::to_string(read_f64(c, i));
        case FieldStatDomain::U64:
            return std::to_string(read_u64(c, i));
        default:
            return std::to_string(read_i64(c, i));
    }
}

// KMV element hash. FNV-1a avalanches poorly, so the mix is what makes the
// bottom-k order (and the [0,1) normalization the estimator divides by)
// uniform.
std::uint64_t kmv_hash(std::string_view v) {
    return dftracer::utils::hash::fnv1a_mix(
        dftracer::utils::hash::fnv1a_hash(v));
}

}  // namespace

void AggStateDeleter::operator()(AggState* p) const noexcept { delete p; }

AggStatePtr agg_new(std::vector<AggSpec> specs, std::vector<AggDynSpec> dyn) {
    AggStatePtr s(new AggState());
    s->specs = std::move(specs);
    s->dyn_specs = std::move(dyn);
    s->init_layout();
    return s;
}

void agg_accumulate(AggState& st, const std::vector<const Series*>& keys,
                    const std::vector<const Series*>& values,
                    std::int64_t begin, std::int64_t end) {
    agg_accumulate(st, keys, values, std::vector<AggDynInput>{}, begin, end);
}

namespace {

// A column as the plain loop reads it: one 8-byte load per cell, the
// validity bitmap beside it.
struct Plain8 {
    const std::uint8_t* data = nullptr;
    const std::uint8_t* validity = nullptr;
    FieldStatDomain domain = FieldStatDomain::I64;
    bool is_null(std::int64_t i) const {
        return validity && !((validity[i >> 3] >> (i & 7)) & 1);
    }
    std::uint64_t bits(std::int64_t i) const {
        std::uint64_t v;
        std::memcpy(&v, data + static_cast<std::size_t>(i) * 8, 8);
        return v;
    }
};

bool plain8(const Series& c, Plain8& out) {
    const TypeId t = c.type();
    if (t != TypeId::Int64 && t != TypeId::Uint64 && t != TypeId::Float64)
        return false;
    const dftu_series& h = *c.handle();
    if (h.encoding != Encoding::Flat || !h.data) return false;
    out.data = h.data->data();
    out.validity = h.validity ? h.validity->data() : nullptr;
    out.domain = col_domain(t);
    return true;
}

// A key column as the plain loop reads it: an 8-byte cell, or a String's
// offsets and bytes (either width), the validity bitmap beside them.
struct PlainKey {
    Plain8 fixed;
    const std::int32_t* off32 = nullptr;
    const std::int64_t* off64 = nullptr;
    const char* text = nullptr;
    std::int64_t text_size = 0;
    bool is_str = false;
    bool is_null(std::int64_t i) const { return fixed.is_null(i); }
    std::int64_t offset(std::int64_t i) const {
        return off64 ? off64[i] : off32[i];
    }
    std::string_view str(std::int64_t i) const {
        if (off64)
            return std::string_view(
                text + off64[i],
                static_cast<std::size_t>(off64[i + 1] - off64[i]));
        return std::string_view(
            text + off32[i], static_cast<std::size_t>(off32[i + 1] - off32[i]));
    }
};

bool plain_key(const Series& c, PlainKey& out) {
    if (plain8(c, out.fixed)) return true;
    const dftu_series& h = *c.handle();
    const TypeId narrow = narrow_varwidth_type(h.type);
    if ((narrow != TypeId::String && narrow != TypeId::Binary) ||
        h.encoding != Encoding::Flat || !h.data)
        return false;
    out.is_str = true;
    out.text = reinterpret_cast<const char*>(h.data->data());
    out.text_size = static_cast<std::int64_t>(h.data->size());
    out.fixed.validity = h.validity ? h.validity->data() : nullptr;
    if (h.offsets64)
        out.off64 = reinterpret_cast<const std::int64_t*>(h.offsets64->data());
    else if (h.offsets)
        out.off32 = reinterpret_cast<const std::int32_t*>(h.offsets->data());
    else
        return false;
    return true;
}

// Whether the plain loop can run this state over these columns, and the
// column views it reads if so.
bool plain_shape(const AggState& st, const std::vector<const Series*>& keys,
                 const std::vector<const Series*>& values,
                 std::vector<PlainKey>& k, std::vector<Plain8>& v) {
    if (st.has_fl || st.has_arg || st.has_bitor || st.has_prod || st.has_kmv ||
        st.has_lst || st.has_ss || st.has_co || st.has_set || st.has_occ ||
        st.has_dyn)
        return false;
    k.resize(keys.size());
    for (std::size_t j = 0; j < keys.size(); ++j)
        if (!plain_key(*keys[j], k[j])) return false;
    v.resize(st.nf);
    for (std::size_t fj = 0; fj < st.nf; ++fj) {
        if (st.field_is_str[fj]) return false;
        if (!plain8(*values[static_cast<std::size_t>(st.field_vc[fj])], v[fj]))
            return false;
    }
    return true;
}

// The word table (AggState::fast_*): a key row as words, an 8-byte cell one
// word, a string of up to 16 bytes its length and two words.
constexpr std::uint64_t FAST_TAG = 0xFFFFFFFF00000000ULL;

std::size_t key_word_count(const std::vector<PlainKey>& k) {
    std::size_t w = 0;
    for (const PlainKey& kk : k) w += kk.is_str ? 3 : 1;
    return w;
}

// Row i's key words; false for a string longer than 16 bytes.
bool key_words(const std::vector<PlainKey>& k, std::int64_t i,
               std::uint64_t* w) {
    for (const PlainKey& kk : k) {
        if (!kk.is_str) {
            *w++ = kk.fixed.bits(i);
            continue;
        }
        std::int64_t off, len;
        if (kk.off32) {
            off = kk.off32[i];
            len = kk.off32[i + 1] - off;
        } else {
            off = kk.off64[i];
            len = kk.off64[i + 1] - off;
        }
        if (len > 16) return false;
        std::uint64_t a = 0, b = 0;
        if (off + 16 <= kk.text_size) {
            std::memcpy(&a, kk.text + off, 8);
            std::memcpy(&b, kk.text + off + 8, 8);
            if (len < 8) {
                a = len == 0 ? 0 : a & (~std::uint64_t{0} >> (64 - 8 * len));
                b = 0;
            } else if (len < 16) {
                b = len == 8 ? 0 : b & (~std::uint64_t{0} >> (128 - 8 * len));
            }
        } else {
            std::memcpy(
                &a, kk.text + off,
                static_cast<std::size_t>(std::min<std::int64_t>(len, 8)));
            if (len > 8)
                std::memcpy(&b, kk.text + off + 8,
                            static_cast<std::size_t>(len - 8));
        }
        *w++ = static_cast<std::uint64_t>(len);
        *w++ = a;
        *w++ = b;
    }
    return true;
}

// Independent products summed, so the words hash in parallel, then one mix.
std::uint64_t hash_words(const std::uint64_t* w, std::size_t W) {
    std::uint64_t h = 0;
    constexpr std::uint64_t K = dftracer::utils::hash::GOLDEN_RATIO;
    std::uint64_t m = K;
    for (std::size_t j = 0; j < W; ++j, m += 2 * K) h += w[j] * m;
    return dftracer::utils::hash::splitmix64(h);
}

// The group in the word table with these words, or -1.
std::int64_t fast_lookup(const AggState& st, std::uint64_t h,
                         const std::uint64_t* w) {
    const std::size_t W = st.fast_stride;
    const std::size_t mask = st.fast_slots.size() - 1;
    const std::uint64_t tag = h & FAST_TAG;
    for (std::size_t s = static_cast<std::size_t>(h) & mask;;
         s = (s + 1) & mask) {
        const std::uint64_t e = st.fast_slots[s];
        if (e == AggState::FAST_NONE) return -1;
        if ((e & FAST_TAG) != tag) continue;
        const auto g = static_cast<std::size_t>(e & ~FAST_TAG);
        const std::uint64_t* gw = st.fast_words.data() + g * W;
        bool same = true;
        for (std::size_t j = 0; j < W; ++j) same &= gw[j] == w[j];
        if (same) return static_cast<std::int64_t>(g);
    }
}

void fast_place(AggState& st, std::uint64_t h, std::uint64_t g) {
    const std::size_t mask = st.fast_slots.size() - 1;
    std::size_t s = static_cast<std::size_t>(h) & mask;
    while (st.fast_slots[s] != AggState::FAST_NONE) s = (s + 1) & mask;
    st.fast_slots[s] = (h & FAST_TAG) | g;
}

// Record the newest group's words (null: not representable) in the table,
// growing it to stay under half full.
void fast_add(AggState& st, std::uint64_t h, const std::uint64_t* w) {
    const std::size_t W = st.fast_stride;
    const auto g = static_cast<std::uint64_t>(st.fast_hash.size());
    if (!w) {
        st.fast_hash.push_back(AggState::FAST_NONE);
        st.fast_words.insert(st.fast_words.end(), W, AggState::FAST_NONE);
        return;
    }
    if ((st.fast_hash.size() + 1) * 2 > st.fast_slots.size()) {
        st.fast_slots.assign(st.fast_slots.size() * 2, AggState::FAST_NONE);
        for (std::size_t o = 0; o < st.fast_hash.size(); ++o)
            if (st.fast_hash[o] != AggState::FAST_NONE)
                fast_place(st, st.fast_hash[o], o);
    }
    st.fast_hash.push_back(h);
    st.fast_words.insert(st.fast_words.end(), w, w + W);
    fast_place(st, h, g);
}

// Whether the word table takes this state over these keys (string keys,
// no nulls, every group so far seen through it), opening it on first use.
bool fast_open(AggState& st, const std::vector<PlainKey>& k,
               bool any_keys = false) {
    if (st.fast_refused) return false;
    bool fast = any_keys;
    for (const PlainKey& kk : k) {
        fast |= kk.is_str;
        if (kk.fixed.validity) return false;
    }
    if (!fast) return false;
    if (st.fast_hash.size() != static_cast<std::size_t>(st.ngroups()) ||
        st.ngroups() >= (std::int64_t{1} << 30)) {
        st.fast_refused = true;
        return false;
    }
    if (st.fast_stride == 0) {
        st.fast_stride = key_word_count(k);
        st.fast_slots.assign(1024, AggState::FAST_NONE);
    }
    return true;
}

// One cell into a light accumulator, in the cell's domain.
__attribute__((always_inline)) inline void light_add(AggState::LightStat& l,
                                                     FieldStatDomain domain,
                                                     std::uint64_t bits) {
    switch (domain) {
        case FieldStatDomain::F64: {
            const double x = std::bit_cast<double>(bits);
            if (l.n == 0) {
                l.sum = bits;
                l.lo = l.hi = bits;
            } else {
                l.sum = std::bit_cast<std::uint64_t>(
                    std::bit_cast<double>(l.sum) + x);
                if (x < std::bit_cast<double>(l.lo)) l.lo = bits;
                if (x > std::bit_cast<double>(l.hi)) l.hi = bits;
            }
            break;
        }
        case FieldStatDomain::I64: {
            const auto x = std::bit_cast<std::int64_t>(bits);
            if (l.n == 0) {
                l.sum = bits;
                l.lo = l.hi = bits;
            } else {
                l.sum = std::bit_cast<std::uint64_t>(
                    std::bit_cast<std::int64_t>(l.sum) + x);
                if (x < std::bit_cast<std::int64_t>(l.lo)) l.lo = bits;
                if (x > std::bit_cast<std::int64_t>(l.hi)) l.hi = bits;
            }
            break;
        }
        case FieldStatDomain::U64:
            if (l.n == 0) {
                l.sum = bits;
                l.lo = l.hi = bits;
            } else {
                l.sum += bits;
                if (bits < l.lo) l.lo = bits;
                if (bits > l.hi) l.hi = bits;
            }
            break;
    }
    ++l.n;
}

// find_or_add_group over a row's key words.
std::int64_t slow_group_words(AggState& st, const std::vector<PlainKey>& k,
                              const std::uint64_t* w) {
    std::size_t at[64];
    for (std::size_t j = 0, p = 0; j < k.size(); ++j) {
        at[j] = p;
        p += k[j].is_str ? 3 : 1;
    }
    return st.find_or_add_group(
        [&](std::size_t j) { return static_cast<std::int64_t>(w[at[j]]); },
        [&](std::size_t j) -> std::string_view {
            return std::string_view(
                reinterpret_cast<const char*>(w + at[j] + 1),
                static_cast<std::size_t>(w[at[j]]));
        },
        [](std::size_t) { return false; });
}

bool light_reducers(const AggState& st) {
    for (const AggSpec& sp : st.specs)
        if (sp.op != AggOp::Count && sp.op != AggOp::CountValid &&
            sp.op != AggOp::Sum && sp.op != AggOp::Mean &&
            sp.op != AggOp::Min && sp.op != AggOp::Max)
            return false;
    return true;
}

bool accumulate_plain(AggState& st, const std::vector<const Series*>& keys,
                      const std::vector<const Series*>& values,
                      std::int64_t begin, std::int64_t end) {
    std::vector<PlainKey> k;
    std::vector<Plain8> v;
    if (!plain_shape(st, keys, values, k, v)) return false;
    const std::size_t nf = st.nf;
    // The higher moments are computed only when a reducer reads them; with
    // none, a fresh state takes the light accumulators.
    const bool moments = !light_reducers(st);
    if (!moments && st.ngroups() == 0 && st.fstats.empty() && !st.has_sketch)
        st.light_on = true;
    // A quantile field's sketch keys for the chunk, one SIMD pass per field.
    std::vector<std::vector<std::int32_t>> sketch_keys;
    std::vector<std::vector<double>> sketch_vals;
    if (st.has_sketch) {
        const double lg = DDSketch{}.log_gamma();
        const auto clen = static_cast<std::size_t>(end - begin);
        sketch_keys.assign(st.n_sketch, {});
        sketch_vals.assign(st.n_sketch, {});
        for (std::size_t fj = 0; fj < nf; ++fj) {
            const int sk = st.field_sketch[fj];
            if (sk < 0) continue;
            std::vector<double>& vals =
                sketch_vals[static_cast<std::size_t>(sk)];
            vals.resize(clen);
            for (std::size_t r = 0; r < clen; ++r) {
                const std::uint64_t bits =
                    v[fj].bits(begin + static_cast<std::int64_t>(r));
                switch (v[fj].domain) {
                    case FieldStatDomain::F64:
                        vals[r] = std::bit_cast<double>(bits);
                        break;
                    case FieldStatDomain::U64:
                        vals[r] = static_cast<double>(bits);
                        break;
                    case FieldStatDomain::I64:
                        vals[r] = static_cast<double>(
                            std::bit_cast<std::int64_t>(bits));
                        break;
                }
            }
            std::vector<std::int32_t>& ks =
                sketch_keys[static_cast<std::size_t>(sk)];
            ks.resize(clen);
            sketch_bucket_keys(vals.data(), static_cast<std::int64_t>(clen), lg,
                               ks.data());
        }
    }

    // Up to three 8-byte keys over dense ranges: a direct table in front of
    // the hash map, indexed by the keys packed as mixed-radix digits, so the
    // common row is one load, one compare, no hash. A cache: a miss goes to
    // find_or_add_group and is written back; a chunk that widens a range
    // rebuilds the table empty, and one that widens it past the limit
    // switches the cache off for good.
    const std::int64_t* direct = nullptr;
    std::uint64_t span = 0;
    std::uint64_t lo_k[3] = {0, 0, 0}, stride[3] = {1, 1, 1};
    const std::size_t nk = k.size();
    bool all_fixed = nk >= 1 && nk <= 3;
    for (const PlainKey& kk : k) all_fixed &= !kk.is_str;
    if (all_fixed && !st.direct_refused && end > begin) {
        std::uint64_t lo[3], hi[3];
        bool any = false;
        for (std::size_t j = 0; j < nk; ++j) {
            lo[j] = ~std::uint64_t{0};
            hi[j] = 0;
        }
        // One key, no nulls, is the common shape and gets a plain loop.
        if (nk == 1 && !k[0].fixed.validity) {
            const std::uint8_t* d = k[0].fixed.data;
            for (std::int64_t i = begin; i < end; ++i) {
                std::uint64_t b;
                std::memcpy(&b, d + static_cast<std::size_t>(i) * 8, 8);
                lo[0] = std::min(lo[0], b);
                hi[0] = std::max(hi[0], b);
            }
            any = end > begin;
        } else {
            for (std::int64_t i = begin; i < end; ++i) {
                bool null = false;
                for (std::size_t j = 0; j < nk && !null; ++j)
                    null = k[j].is_null(i);
                if (null) continue;
                any = true;
                for (std::size_t j = 0; j < nk; ++j) {
                    const std::uint64_t b = k[j].fixed.bits(i);
                    lo[j] = std::min(lo[j], b);
                    hi[j] = std::max(hi[j], b);
                }
            }
        }
        if (any) {
            // Union with the ranges the table already covers.
            if (!st.direct_groups.empty())
                for (std::size_t j = 0; j < nk; ++j) {
                    lo[j] = std::min(lo[j], st.direct_lo[j]);
                    hi[j] = std::max(hi[j],
                                     st.direct_lo[j] + st.direct_span[j] - 1);
                }
            std::uint64_t total = 1;
            bool fits = true;
            for (std::size_t j = 0; j < nk && fits; ++j) {
                const std::uint64_t w = hi[j] - lo[j] + 1;
                fits = w != 0 && total <= (std::uint64_t{1} << 26) / w;
                total *= w;
            }
            const std::uint64_t limit =
                static_cast<std::uint64_t>(end - begin) * 4 + 1024;
            if (fits && total <= limit && total <= (std::uint64_t{1} << 26)) {
                bool same = !st.direct_groups.empty();
                for (std::size_t j = 0; j < nk && same; ++j)
                    same = st.direct_lo[j] == lo[j] &&
                           st.direct_span[j] == hi[j] - lo[j] + 1;
                if (!same) {
                    st.direct_groups.assign(static_cast<std::size_t>(total),
                                            -1);
                    for (std::size_t j = 0; j < nk; ++j) {
                        st.direct_lo[j] = lo[j];
                        st.direct_span[j] = hi[j] - lo[j] + 1;
                    }
                }
                direct = st.direct_groups.data();
                span = st.direct_groups.size();
                std::uint64_t s = 1;
                for (std::size_t j = 0; j < nk; ++j) {
                    lo_k[j] = st.direct_lo[j];
                    stride[j] = s;
                    s *= st.direct_span[j];
                }
            } else {
                st.direct_refused = true;
                st.direct_groups.clear();
            }
        }
    }

    // Add row i to group g.
    auto add_row = [&]<bool Moments, bool Nulls>(std::int64_t g,
                                                 std::int64_t i) {
        if (static_cast<std::size_t>(g) == st.group_first_row.size())
            st.group_first_row.push_back(i);
        st.counts[static_cast<std::size_t>(g)]++;
        if constexpr (!Moments) {
            if (st.light_on) {
                AggState::LightStat* ls =
                    st.light.data() + static_cast<std::size_t>(g) * nf;
                for (std::size_t fj = 0; fj < nf; ++fj) {
                    const Plain8& c = v[fj];
                    if (Nulls && c.is_null(i)) continue;
                    light_add(ls[fj], c.domain, c.bits(i));
                }
                return;
            }
        }
        FieldStat* fs = st.fstats.data() + static_cast<std::size_t>(g) * nf;
        for (std::size_t fj = 0; fj < nf; ++fj) {
            const Plain8& c = v[fj];
            if (Nulls && c.is_null(i)) continue;
            const std::uint64_t bits = c.bits(i);
            if (st.has_sketch && st.field_sketch[fj] >= 0) {
                const auto sk = static_cast<std::size_t>(st.field_sketch[fj]);
                const auto r = static_cast<std::size_t>(i - begin);
                st.sketches[static_cast<std::size_t>(g) * st.n_sketch + sk]
                    .add_key(sketch_keys[sk][r], sketch_vals[sk][r]);
            }
            switch (c.domain) {
                case FieldStatDomain::F64:
                    fs[fj].add_with<Moments>(std::bit_cast<double>(bits));
                    break;
                case FieldStatDomain::U64:
                    fs[fj].add_with<Moments>(bits);
                    break;
                case FieldStatDomain::I64:
                    fs[fj].add_with<Moments>(std::bit_cast<std::int64_t>(bits));
                    break;
            }
        }
    };
    auto slow_group = [&](std::int64_t i) {
        return st.find_or_add_group(
            [&](std::size_t j) {
                return static_cast<std::int64_t>(k[j].fixed.bits(i));
            },
            [&](std::size_t j) -> std::string_view { return k[j].str(i); },
            [&](std::size_t j) { return k[j].is_null(i); });
    };

    // A batch of rows resolves its groups first and adds them after, each
    // pass touching the table's lines for rows ahead, so the loads a row
    // depends on overlap instead of stalling one after another.
    constexpr std::int64_t BATCH = 1024;
    constexpr std::int64_t AHEAD = 16;
    std::vector<std::int64_t> groups(static_cast<std::size_t>(BATCH));
    auto add_batch = [&]<bool Moments, bool Nulls>(std::int64_t b0,
                                                   std::int64_t m) {
        for (std::int64_t r = 0; r < m; ++r) {
            // No prefetch of an empty stats array: a prefetch of address
            // zero walks the page tables for nothing, 25 ns a row.
            if (r + AHEAD < m) {
                const auto ga = static_cast<std::size_t>(
                    groups[static_cast<std::size_t>(r + AHEAD)]);
                if (nf > 0) {
                    if (st.light_on)
                        __builtin_prefetch(st.light.data() + ga * nf, 1);
                    else
                        __builtin_prefetch(st.fstats.data() + ga * nf, 1);
                }
                __builtin_prefetch(st.counts.data() + ga, 1);
            }
            add_row.template operator()<Moments, Nulls>(
                groups[static_cast<std::size_t>(r)], b0 + r);
        }
    };

    // The row loop, instantiated once per (moments, nulls-anywhere) so
    // neither test sits inside it.
    auto run = [&]<bool Moments, bool Nulls, std::size_t NK>() {
        // NK is the key count the packing unrolls over; 0 leaves the cache
        // off (a string key, or more keys than it covers).
        auto packed = [&](std::int64_t i) {
            std::uint64_t at = 0;
            for (std::size_t j = 0; j < NK; ++j)
                at += (k[j].fixed.bits(i) - lo_k[j]) * stride[j];
            return at;
        };
        auto key_null = [&](std::int64_t i) {
            for (std::size_t j = 0; j < NK; ++j)
                if (k[j].is_null(i)) return true;
            return false;
        };
        // No key: the one group, made on the first row.
        if (nk == 0) {
            for (std::int64_t i = begin; i < end; ++i) {
                const std::int64_t g = i == begin ? slow_group(i) : 0;
                add_row.template operator()<Moments, Nulls>(g, i);
            }
            return;
        }
        for (std::int64_t i = begin; i < end; ++i) {
            std::int64_t g = -1;
            std::uint64_t at = 0;
            const bool cached = NK > 0 && direct && (!Nulls || !key_null(i));
            if (cached) {
                at = packed(i);
                g = at < span ? direct[at] : -1;
            }
            if (g < 0) {
                g = slow_group(i);
                if (cached && at < span)
                    st.direct_groups[static_cast<std::size_t>(at)] = g;
            }
            add_row.template operator()<Moments, Nulls>(g, i);
        }
    };

    // The string-keyed row loop over the word table, a batch at a time: the
    // key words and hashes; a slot read per row with the slots ahead on
    // their way, which names a candidate group and starts its words and
    // stats loading; the words compared; the rows added. A row whose
    // candidate is wrong or missing takes the scalar path.
    auto run_fast = [&]<bool Moments, bool Nulls>() {
        const std::size_t W = st.fast_stride;
        std::vector<std::uint64_t> words(static_cast<std::size_t>(BATCH) * W);
        std::vector<std::uint64_t> hashes(static_cast<std::size_t>(BATCH));
        std::vector<std::uint8_t> packable(static_cast<std::size_t>(BATCH));
        std::vector<std::int64_t> cand(static_cast<std::size_t>(BATCH));
        for (std::int64_t b0 = begin; b0 < end; b0 += BATCH) {
            const std::int64_t m = std::min(BATCH, end - b0);
            for (std::int64_t r = 0; r < m; ++r) {
                std::uint64_t* w =
                    words.data() + static_cast<std::size_t>(r) * W;
                const bool ok = key_words(k, b0 + r, w);
                packable[static_cast<std::size_t>(r)] = ok;
                hashes[static_cast<std::size_t>(r)] = ok ? hash_words(w, W) : 0;
            }
            const std::size_t mask = st.fast_slots.size() - 1;
            for (std::int64_t r = 0; r < std::min(m, AHEAD); ++r)
                __builtin_prefetch(
                    st.fast_slots.data() +
                        (static_cast<std::size_t>(
                             hashes[static_cast<std::size_t>(r)]) &
                         mask),
                    0);
            for (std::int64_t r = 0; r < m; ++r) {
                if (r + AHEAD < m)
                    __builtin_prefetch(
                        st.fast_slots.data() +
                            (static_cast<std::size_t>(
                                 hashes[static_cast<std::size_t>(r + AHEAD)]) &
                             mask),
                        0);
                const std::uint64_t h = hashes[static_cast<std::size_t>(r)];
                const std::uint64_t e =
                    st.fast_slots[static_cast<std::size_t>(h) & mask];
                std::int64_t g = -1;
                if (packable[static_cast<std::size_t>(r)] &&
                    e != AggState::FAST_NONE &&
                    (e & FAST_TAG) == (h & FAST_TAG)) {
                    g = static_cast<std::int64_t>(e & ~FAST_TAG);
                    const auto gi = static_cast<std::size_t>(g);
                    __builtin_prefetch(st.fast_words.data() + gi * W, 0);
                    if (nf > 0) {
                        if (st.light_on)
                            __builtin_prefetch(st.light.data() + gi * nf, 1);
                        else
                            __builtin_prefetch(st.fstats.data() + gi * nf, 1);
                    }
                    __builtin_prefetch(st.counts.data() + gi, 1);
                }
                cand[static_cast<std::size_t>(r)] = g;
            }
            for (std::int64_t r = 0; r < m; ++r) {
                const std::int64_t i = b0 + r;
                const std::uint64_t* w =
                    words.data() + static_cast<std::size_t>(r) * W;
                std::int64_t g = cand[static_cast<std::size_t>(r)];
                if (g >= 0) {
                    const std::uint64_t* gw =
                        st.fast_words.data() + static_cast<std::size_t>(g) * W;
                    bool same = true;
                    for (std::size_t j = 0; j < W; ++j) same &= gw[j] == w[j];
                    if (!same) g = -1;
                }
                if (g < 0) {
                    if (packable[static_cast<std::size_t>(r)]) {
                        const std::uint64_t h =
                            hashes[static_cast<std::size_t>(r)];
                        g = fast_lookup(st, h, w);
                        if (g < 0) {
                            g = slow_group(i);
                            if (static_cast<std::size_t>(g) ==
                                st.fast_hash.size())
                                fast_add(st, h, w);
                        }
                    } else {
                        g = slow_group(i);
                        if (static_cast<std::size_t>(g) == st.fast_hash.size())
                            fast_add(st, 0, nullptr);
                    }
                }
                groups[static_cast<std::size_t>(r)] = g;
            }
            add_batch.template operator()<Moments, Nulls>(b0, m);
        }
    };
    bool nulls = false;
    for (const PlainKey& kk : k) nulls |= kk.fixed.validity != nullptr;
    for (const Plain8& c : v) nulls |= c.validity != nullptr;
    const bool fast = fast_open(st, k);
    const std::size_t dk = direct ? nk : 0;
    auto dispatch = [&]<bool Moments, bool Nulls>() {
        if (fast) {
            run_fast.template operator()<Moments, Nulls>();
            return;
        }
        switch (dk) {
            case 1:
                run.template operator()<Moments, Nulls, 1>();
                break;
            case 2:
                run.template operator()<Moments, Nulls, 2>();
                break;
            case 3:
                run.template operator()<Moments, Nulls, 3>();
                break;
            default:
                run.template operator()<Moments, Nulls, 0>();
        }
    };
    if (moments) {
        if (nulls)
            dispatch.template operator()<true, true>();
        else
            dispatch.template operator()<true, false>();
    } else {
        if (nulls)
            dispatch.template operator()<false, true>();
        else
            dispatch.template operator()<false, false>();
    }
    return true;
}

}  // namespace

void agg_accumulate(AggState& st, const std::vector<const Series*>& keys,
                    const std::vector<const Series*>& values,
                    const std::vector<AggDynInput>& dyn_in, std::int64_t begin,
                    std::int64_t end) {
    if (st.has_dyn)
        for (const AggDynInput& di : dyn_in)
            st.dyn_domain.emplace(di.name, col_domain(di.col->type()));
    if (!st.inited) {
        st.nkeys = keys.size();
        st.key_is_bytes.resize(st.nkeys);
        st.key_domain.resize(st.nkeys);
        st.key_type.resize(st.nkeys);
        st.key_time_unit.resize(st.nkeys);
        st.key_timezone.resize(st.nkeys);
        st.key_byte_width.resize(st.nkeys);
        st.key_decimal_precision.resize(st.nkeys);
        st.key_decimal_scale.resize(st.nkeys);
        st.ikey_cols.resize(st.nkeys);
        st.skey_cols.resize(st.nkeys);
        st.nkey_cols.resize(st.nkeys);
        for (std::size_t k = 0; k < st.nkeys; ++k) {
            const TypeId kt = keys[k]->type();
            if (!is_group_key_type(kt)) {
                throw std::invalid_argument(
                    std::string("group_by: key column type '") + type_name(kt) +
                    "' is not supported as a group key (no int64/double "
                    "domain to hash it in)");
            }
            st.key_is_bytes[k] = is_bytes_key_type(kt) ? 1 : 0;
            st.key_domain[k] = col_domain(kt);
            st.key_type[k] = kt;
            if (kt == TypeId::Time32 || kt == TypeId::Time64 ||
                kt == TypeId::Timestamp || kt == TypeId::Duration) {
                const DataType dt = keys[k]->data_type();
                st.key_time_unit[k] = dt.time_unit;
                st.key_timezone[k] = dt.timezone;
            } else if (kt == TypeId::FixedSizeBinary ||
                       kt == TypeId::Decimal128 || kt == TypeId::Decimal256) {
                const DataType dt = keys[k]->data_type();
                st.key_byte_width[k] = static_cast<std::int32_t>(
                    byte_width(kt, dt.fixed_size).value_or(0));
                st.key_decimal_precision[k] = dt.decimal_precision;
                st.key_decimal_scale[k] = dt.decimal_scale;
            }
        }
        st.field_domain.resize(st.nf);
        st.field_is_str.resize(st.nf);
        for (std::size_t fj = 0; fj < st.nf; ++fj) {
            const Series* vc =
                values[static_cast<std::size_t>(st.field_vc[fj])];
            st.field_domain[fj] = col_domain(vc->type());
            st.field_is_str[fj] = vc->type() == TypeId::String ? 1 : 0;
        }
        // A String field keeps only its presence count (n) and its
        // first/last text; a numeric reducer over it would read the zeroed
        // moments and report a number where none exists.
        for (std::size_t s = 0; s < st.specs.size(); ++s) {
            const int fi = st.spec_field[s];
            if (fi < 0 || !st.field_is_str[static_cast<std::size_t>(fi)])
                continue;
            const AggOp op = st.specs[s].op;
            if (op != AggOp::CountValid && op != AggOp::First &&
                op != AggOp::Last)
                throw std::invalid_argument(
                    std::string("group_by: aggregate '") + st.specs[s].out +
                    "' reduces a String column numerically; only "
                    "count_valid, first and last apply to strings");
        }
        st.inited = true;
    }
    // A zero-key state (the whole batch is one group) has no key column to read
    // the row count from, so fall back to a value column; a caller with neither
    // must pass `end` explicitly.
    if (end < 0)
        end = !keys.empty()     ? keys[0]->length()
              : !values.empty() ? values[0]->length()
                                : 0;
    const std::int64_t clen = end - begin;

    // The plain case, which is most group-bys: 8-byte keys and values, no
    // positional / collection state. Every buffer is resolved once and the
    // row loop is loads and adds; the generic loop below makes a C ABI call
    // per cell.
    if (accumulate_plain(st, keys, values, begin, end)) return;
    st.settle_light();

    // Precompute DDSketch bucket keys for each sketch field in one SIMD pass
    // over the chunk (the logarithm is the costly step); the per-group scatter
    // below just increments a bin. Null rows get a garbage key that the scatter
    // skips.
    std::vector<std::vector<std::int32_t>> chunk_keys;
    std::vector<std::vector<double>> chunk_vals;
    if (st.has_sketch && clen > 0) {
        const double lg = DDSketch{}.log_gamma();
        chunk_keys.assign(st.n_sketch, {});
        chunk_vals.assign(st.n_sketch, {});
        for (std::size_t fj = 0; fj < st.nf; ++fj) {
            const int sk = st.field_sketch[fj];
            if (sk < 0) continue;
            const Series* vc =
                values[static_cast<std::size_t>(st.field_vc[fj])];
            std::vector<double>& vals =
                chunk_vals[static_cast<std::size_t>(sk)];
            vals.resize(static_cast<std::size_t>(clen));
            for (std::int64_t r = 0; r < clen; ++r)
                vals[static_cast<std::size_t>(r)] =
                    read_as_double(*vc, begin + r, st.field_domain[fj]);
            chunk_keys[static_cast<std::size_t>(sk)].resize(
                static_cast<std::size_t>(clen));
            sketch_bucket_keys(vals.data(), clen, lg,
                               chunk_keys[static_cast<std::size_t>(sk)].data());
        }
    }

    for (std::int64_t i = begin; i < end; ++i) {
        std::int64_t g = st.group_of(keys, i);
        if (static_cast<std::size_t>(g) == st.group_first_row.size())
            st.group_first_row.push_back(i);
        st.counts[static_cast<std::size_t>(g)]++;
        const std::size_t base = static_cast<std::size_t>(g) * st.nf;
        for (std::size_t fj = 0; fj < st.nf; ++fj) {
            const Series* vc =
                values[static_cast<std::size_t>(st.field_vc[fj])];
            if (vc->is_null(i)) continue;
            if (!st.field_is_str[fj])
                fs_add(st.fstats[base + fj], *vc, i, st.field_domain[fj]);
            else
                st.fstats[base + fj].n++;
            if (st.has_sketch && st.field_sketch[fj] >= 0) {
                const std::size_t sk =
                    static_cast<std::size_t>(st.field_sketch[fj]);
                const auto r = static_cast<std::size_t>(i - begin);
                st.sketches[static_cast<std::size_t>(g) * st.n_sketch + sk]
                    .add_key(chunk_keys[sk][r], chunk_vals[sk][r]);
            }
            if (st.has_fl) {
                const std::size_t sl = base + fj;
                if (st.fl_first_idx[sl] < 0 || i < st.fl_first_idx[sl]) {
                    st.fl_first_idx[sl] = i;
                    if (st.field_is_str[fj])
                        st.fl_first_s[sl] = std::string(vc->string_at(i));
                    else
                        st.fl_first[sl] =
                            read_bits(*vc, i, st.field_domain[fj]);
                }
                if (i > st.fl_last_idx[sl]) {
                    st.fl_last_idx[sl] = i;
                    if (st.field_is_str[fj])
                        st.fl_last_s[sl] = std::string(vc->string_at(i));
                    else
                        st.fl_last[sl] = read_bits(*vc, i, st.field_domain[fj]);
                }
            }
        }
        if (st.has_arg) {
            const std::size_t abase = static_cast<std::size_t>(g) * st.n_arg;
            const std::size_t rbase =
                static_cast<std::size_t>(g) * st.n_arg_repr;
            for (std::size_t slot = 0; slot < st.n_arg; ++slot) {
                const Series* byc =
                    values[static_cast<std::size_t>(st.arg_by_col[slot])];
                if (byc->is_null(i)) continue;
                // arg_by is double-keyed (storage, merge compare, serialize),
                // so an integer by-value above 2^53 loses exactness here.
                const double byv =
                    read_as_double(*byc, i, col_domain(byc->type()));
                const std::size_t as = abase + slot;
                const std::vector<int>& reprs = st.arg_slot_reprs[slot];
                // A missing value at the winning row reprs as "" (matches the
                // View fold's src.value(field) for an absent field).
                auto repr_of = [&](int ri) -> std::string {
                    const Series* vc = values[static_cast<std::size_t>(
                        st.arg_val_col[static_cast<std::size_t>(ri)])];
                    return vc->is_null(i) ? std::string() : cell_repr(*vc, i);
                };
                // Strict compare, so a tie keeps the row seen first; the View
                // fold does the same and the two engines are compared row for
                // row.
                const bool adopt =
                    !st.arg_has[as] ||
                    (st.arg_dir[slot] == ArgDir::Max ? byv > st.arg_by[as]
                                                     : byv < st.arg_by[as]);
                if (!adopt) continue;
                st.arg_by[as] = byv;
                st.arg_has[as] = 1;
                for (int ri : reprs)
                    st.arg_repr[rbase + static_cast<std::size_t>(ri)] =
                        repr_of(ri);
            }
        }
        if (st.has_bitor) {
            const std::size_t bbase = static_cast<std::size_t>(g) * st.n_bitor;
            for (std::size_t slot = 0; slot < st.n_bitor; ++slot) {
                const Series* vc =
                    values[static_cast<std::size_t>(st.bitor_val_col[slot])];
                if (vc->is_null(i)) continue;
                st.bitor_acc[bbase + slot] |=
                    read_u64_exact(*vc, i, col_domain(vc->type()));
            }
        }
        if (st.has_prod) {
            const std::size_t pbase = static_cast<std::size_t>(g) * st.n_prod;
            for (std::size_t slot = 0; slot < st.n_prod; ++slot) {
                const Series* vc =
                    values[static_cast<std::size_t>(st.prod_val_col[slot])];
                if (vc->is_null(i)) continue;
                st.prod_acc[pbase + slot] *=
                    read_as_double(*vc, i, col_domain(vc->type()));
            }
        }
        if (st.has_kmv) {
            const std::size_t kbase = static_cast<std::size_t>(g) * st.n_kmv;
            for (std::size_t slot = 0; slot < st.n_kmv; ++slot) {
                const Series* vc =
                    values[static_cast<std::size_t>(st.kmv_val_col[slot])];
                if (vc->is_null(i)) continue;
                std::string v = cell_repr(*vc, i);
                KmvMap& m = st.kmv[kbase + slot];
                const std::uint64_t h = kmv_hash(v);
                // Past the k-th smallest hash the element cannot enter, so the
                // common case costs one compare and no allocation.
                if (m.size() >= st.kmv_k[slot] && h >= m.rbegin()->first)
                    continue;
                m.insert_or_assign(h, std::move(v));
                kmv_trim(m, st.kmv_k[slot]);
            }
        }
        if (st.has_lst) {
            const std::size_t lbase = static_cast<std::size_t>(g) * st.n_lst;
            for (std::size_t slot = 0; slot < st.n_lst; ++slot) {
                const Series* vc =
                    values[static_cast<std::size_t>(st.lst_val_col[slot])];
                const Series* byc =
                    values[static_cast<std::size_t>(st.lst_by_col[slot])];
                if (byc->is_null(i) || vc->is_null(i)) continue;
                ListItems& items = st.lst[lbase + slot];
                items.emplace_back(
                    read_as_double(*byc, i, col_domain(byc->type())),
                    cell_repr(*vc, i));
                const std::size_t k = st.lst_k[slot];
                if (k > 0 && items.size() > k)
                    list_bound(items, st.lst_kind[slot], k);
            }
        }
        if (st.has_ss) {
            const std::size_t sbase = static_cast<std::size_t>(g) * st.n_ss;
            for (std::size_t slot = 0; slot < st.n_ss; ++slot) {
                const Series* vc =
                    values[static_cast<std::size_t>(st.ss_val_col[slot])];
                if (vc->is_null(i)) continue;
                space_saving_offer(st.ss_counters[sbase + slot], st.ss_k[slot],
                                   cell_repr(*vc, i), 1);
            }
        }
        if (st.has_co) {
            const std::size_t cbase = static_cast<std::size_t>(g) * st.n_co;
            for (std::size_t slot = 0; slot < st.n_co; ++slot) {
                const Series* vc =
                    values[static_cast<std::size_t>(st.co_val_col[slot])];
                const Series* byc =
                    values[static_cast<std::size_t>(st.co_by_col[slot])];
                if (vc->is_null(i) || byc->is_null(i)) continue;
                const double x =
                    read_as_double(*byc, i, col_domain(byc->type()));
                const double y = read_as_double(*vc, i, col_domain(vc->type()));
                const std::size_t cs = cbase + slot;
                st.co_n[cs] += 1.0;
                st.co_sx[cs] += x;
                st.co_sy[cs] += y;
                st.co_sxx[cs] += x * x;
                st.co_syy[cs] += y * y;
                st.co_sxy[cs] += x * y;
            }
        }
        if (st.has_set) {
            for (std::size_t slot = 0; slot < st.n_set; ++slot) {
                const Series* vc =
                    values[static_cast<std::size_t>(st.set_val_col[slot])];
                if (vc->is_null(i)) continue;
                std::string v = cell_repr(*vc, i);
                if (!v.empty())
                    st.sets[static_cast<std::size_t>(g) * st.n_set + slot]
                        .insert(std::move(v));
            }
        }
        if (st.has_occ) {
            for (std::size_t slot = 0; slot < st.n_occ; ++slot) {
                const Series* tsc =
                    values[static_cast<std::size_t>(st.occ_val_col[slot])];
                const Series* durc =
                    values[static_cast<std::size_t>(st.occ_by_col[slot])];
                if (tsc->is_null(i) || durc->is_null(i)) continue;
                const FieldStatDomain durd = col_domain(durc->type());
                std::uint64_t dur;
                if (durd == FieldStatDomain::I64) {
                    const std::int64_t d = read_i64(*durc, i);
                    if (d <= 0) continue;
                    dur = static_cast<std::uint64_t>(d);
                } else if (durd == FieldStatDomain::U64) {
                    dur = read_u64(*durc, i);
                    if (dur == 0) continue;
                } else {
                    const double d = read_f64(*durc, i);
                    if (!(d > 0)) continue;
                    dur = static_cast<std::uint64_t>(d);
                }
                std::uint64_t s0 =
                    read_u64_exact(*tsc, i, col_domain(tsc->type()));
                std::uint64_t e0 = s0 + dur;
                const std::uint64_t cell = st.occ_cell[slot];
                if (cell) {  // optional tolerance: snap start down, end up
                    s0 = s0 / cell * cell;
                    e0 = (e0 + cell - 1) / cell * cell;
                }
                const std::size_t obase =
                    static_cast<std::size_t>(g) * st.n_occ + slot;
                st.occ_total[obase] += dur;
                if (s0 < st.occ_ts[obase]) st.occ_ts[obase] = s0;
                if (e0 > st.occ_te[obase]) st.occ_te[obase] = e0;
                st.occ_deltas[obase][s0] += 1;
                st.occ_deltas[obase][e0] -= 1;
            }
        }
        if (st.has_dyn) {
            for (const AggDynInput& di : dyn_in) {
                if (di.col->is_null(i)) continue;
                const FieldStatDomain d = col_domain(di.col->type());
                std::map<std::string, FieldStat>& gmap =
                    st.dyn_fs[static_cast<std::size_t>(g)];
                fs_add(gmap[di.name], *di.col, i, d);
                if (st.dyn_has_sketch)
                    st.dyn_sketch[static_cast<std::size_t>(g)][di.name].add(
                        read_as_double(*di.col, i, d));
            }
        }
    }
}

void agg_accumulate(AggState& st, const Series& key,
                    const std::vector<const Series*>& values,
                    std::int64_t begin, std::int64_t end) {
    const std::vector<const Series*> keys{&key};
    agg_accumulate(st, keys, values, begin, end);
}

void agg_merge(AggState& into, const AggState& other) {
    std::vector<std::int64_t> order(static_cast<std::size_t>(other.ngroups()));
    std::iota(order.begin(), order.end(), 0);
    agg_merge(into, other, order);
}

void agg_merge(AggState& into, const AggState& other_in,
               const std::vector<std::int64_t>& order) {
    const AggState& other = settled(other_in);
    into.settle_light();
    if (!other.inited) return;
    if (!into.inited) {
        into.adopt_layout(other);
        into.nkeys = other.nkeys;
        into.key_is_bytes = other.key_is_bytes;
        into.key_domain = other.key_domain;
        into.key_type = other.key_type;
        into.key_time_unit = other.key_time_unit;
        into.key_timezone = other.key_timezone;
        into.key_byte_width = other.key_byte_width;
        into.key_decimal_precision = other.key_decimal_precision;
        into.key_decimal_scale = other.key_decimal_scale;
        into.ikey_cols.assign(into.nkeys, {});
        into.skey_cols.assign(into.nkeys, {});
        into.nkey_cols.assign(into.nkeys, {});
        into.inited = true;
    }
    for (const auto& [name, d] : other.dyn_domain)
        into.dyn_domain.emplace(name, d);
    for (const std::int64_t j : order)
        into.merge_group(into.group_of_other(other, j), other, j);
}

bool agg_pack_shape(AggState& st, const std::vector<const Series*>& keys,
                    const std::vector<const Series*>& values, AggPacked& out) {
    if (!st.inited) agg_accumulate(st, keys, values, 0, 0);
    std::vector<PlainKey> k;
    std::vector<Plain8> v;
    if (!st.inited || !light_reducers(st) || st.has_sketch ||
        !plain_shape(st, keys, values, k, v) || k.empty() || k.size() > 21)
        return false;
    for (const PlainKey& kk : k)
        if (kk.fixed.validity) return false;
    out.W = key_word_count(k);
    out.strings = out.W > k.size();
    out.nf = st.nf;
    out.nulls = false;
    for (const Plain8& c : v) out.nulls |= c.validity != nullptr;
    out.stride = 1 + out.W + out.nf + (out.nulls ? 1 : 0);
    return out.stride <= AggPages::PAGE_WORDS / 8 &&
           keys[0]->length() < (std::int64_t{1} << 32);
}

bool agg_pack(const AggState& st, const std::vector<const Series*>& keys,
              const std::vector<const Series*>& values, std::int64_t begin,
              std::int64_t end, const AggPacked& shape,
              std::vector<AggPages>& out,
              const std::function<void(std::size_t)>& full) {
    std::vector<PlainKey> k;
    std::vector<Plain8> v;
    if (!plain_shape(st, keys, values, k, v)) return false;
    // The partition from the hash's top word, scaled to any count.
    const auto parts = static_cast<std::uint64_t>(out.size());
    for (AggPages& p : out)
        if (p.stride == 0) p.open(shape.stride);
    // The row loop with the word count fixed, so the copies are plain
    // stores; a wider key takes the general loop.
    auto run = [&]<std::size_t WK>() {
        std::uint64_t w[WK > 0 ? WK : 64];
        for (std::int64_t i = begin; i < end; ++i) {
            if (!key_words(k, i, w)) return false;
            const std::uint64_t h = hash_words(w, WK > 0 ? WK : shape.W);
            const auto p = static_cast<std::size_t>(((h >> 32) * parts) >> 32);
            AggPages& dst = out[p];
            std::uint64_t* r = dst.next();
            r[0] = (static_cast<std::uint64_t>(i) << 32) |
                   static_cast<std::uint32_t>(h);
            if constexpr (WK > 0) {
                for (std::size_t j = 0; j < WK; ++j) r[1 + j] = w[j];
            } else {
                for (std::size_t j = 0; j < shape.W; ++j) r[1 + j] = w[j];
            }
            std::uint64_t* vals = r + 1 + shape.W;
            std::uint64_t mask = 0;
            for (std::size_t fj = 0; fj < shape.nf; ++fj) {
                vals[fj] = v[fj].bits(i);
                if (v[fj].is_null(i)) mask |= std::uint64_t{1} << fj;
            }
            if (shape.nulls) vals[shape.nf] = mask;
            if (dst.count == dst.per_page) full(p);
        }
        return true;
    };
    switch (shape.W) {
        case 1:
            return run.template operator()<1>();
        case 2:
            return run.template operator()<2>();
        case 3:
            return run.template operator()<3>();
        case 4:
            return run.template operator()<4>();
        case 5:
            return run.template operator()<5>();
        case 6:
            return run.template operator()<6>();
        default:
            return run.template operator()<0>();
    }
}

void agg_accumulate_packed(AggState& st, const std::vector<const Series*>& keys,
                           const std::vector<const Series*>& values,
                           const AggPacked& shape, const AggPages& rows) {
    if (!st.inited) agg_accumulate(st, keys, values, 0, 0);
    std::vector<PlainKey> k;
    std::vector<Plain8> v;
    if (!plain_shape(st, keys, values, k, v))
        throw std::logic_error("agg_accumulate_packed: not a packable shape");
    if (st.ngroups() == 0 && st.fstats.empty()) st.light_on = true;
    if (!st.light_on || !fast_open(st, k, true))
        throw std::logic_error("agg_accumulate_packed: not a light state");
    const std::size_t nf = st.nf;
    const std::size_t W = shape.W;
    for (std::size_t pg = 0; pg < rows.pages.size(); ++pg) {
        const std::size_t m =
            std::min(rows.per_page, rows.count - pg * rows.per_page);
        const std::uint64_t* page = rows.pages[pg].get();
        for (std::size_t r = 0; r < m; ++r) {
            const std::uint64_t* rec = page + r * shape.stride;
            const std::uint64_t* w = rec + 1;
            // The hash's low word, doubled up: the slot bits and a tag.
            const std::uint64_t h32 = rec[0] & 0xFFFFFFFFULL;
            const std::uint64_t h = (h32 << 32) | h32;
            std::int64_t g = fast_lookup(st, h, w);
            if (g < 0) {
                g = slow_group_words(st, k, w);
                if (static_cast<std::size_t>(g) == st.fast_hash.size())
                    fast_add(st, h, w);
            }
            const auto gi = static_cast<std::size_t>(g);
            const auto row = static_cast<std::int64_t>(rec[0] >> 32);
            if (gi == st.group_first_row.size())
                st.group_first_row.push_back(row);
            else if (row < st.group_first_row[gi])
                st.group_first_row[gi] = row;
            st.counts[gi]++;
            const std::uint64_t* vals = w + W;
            const std::uint64_t mask = shape.nulls ? vals[nf] : 0;
            AggState::LightStat* ls = st.light.data() + gi * nf;
            for (std::size_t fj = 0; fj < nf; ++fj) {
                if ((mask >> fj) & 1) continue;
                light_add(ls[fj], v[fj].domain, vals[fj]);
            }
        }
    }
}

AggStatePtr agg_in_first_seen_order(AggStatePtr st) {
    if (!st || !st->inited || !agg_first_rows_known(*st)) return st;
    const std::vector<std::int64_t>& first = st->group_first_row;
    if (std::is_sorted(first.begin(), first.end())) return st;
    std::vector<std::int64_t> order(first.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](std::int64_t a, std::int64_t b) {
        return first[static_cast<std::size_t>(a)] <
               first[static_cast<std::size_t>(b)];
    });
    agg_permute_groups(*st, order);
    return st;
}

AggStatePtr agg_merge_many(std::vector<AggStatePtr>& partials) {
    std::size_t widest = partials.size();
    for (std::size_t p = 0; p < partials.size(); ++p)
        if (partials[p] && partials[p]->inited &&
            (widest == partials.size() ||
             partials[p]->ngroups() > partials[widest]->ngroups()))
            widest = p;
    if (widest == partials.size()) {
        for (AggStatePtr& p : partials)
            if (p) return std::move(p);
        return nullptr;
    }
    AggStatePtr base = std::move(partials[widest]);
    base->settle_light();
    std::vector<const AggState*> others;
    for (const AggStatePtr& p : partials)
        if (p && p->inited) others.push_back(&settled(*p));
    for (const AggState* o : others)
        for (const auto& [name, d] : o->dyn_domain)
            base->dyn_domain.emplace(name, d);

    // Each other's group -> base group, or -1: one fork-join over every
    // (partial, group) pair.
    std::vector<std::vector<std::int64_t>> map(others.size());
    std::vector<std::int64_t> offset(others.size() + 1, 0);
    for (std::size_t p = 0; p < others.size(); ++p) {
        map[p].assign(static_cast<std::size_t>(others[p]->ngroups()), -1);
        offset[p + 1] = offset[p] + others[p]->ngroups();
    }
    parallel_for(offset.back(), 1024, [&](std::int64_t b, std::int64_t e) {
        std::size_t p = 0;
        while (offset[p + 1] <= b) ++p;
        for (std::int64_t x = b; x < e; ++x) {
            while (offset[p + 1] <= x) ++p;
            const std::int64_t j = x - offset[p];
            map[p][static_cast<std::size_t>(j)] =
                base->find_group_of_other(*others[p], j);
        }
    });
    const std::int64_t ng = base->ngroups();
    constexpr std::int64_t GRAIN = 1024;
    parallel_for(ng, GRAIN, [&](std::int64_t gb, std::int64_t ge) {
        for (std::size_t p = 0; p < others.size(); ++p) {
            const std::vector<std::int64_t>& m = map[p];
            for (std::size_t j = 0; j < m.size(); ++j)
                if (m[j] >= gb && m[j] < ge)
                    base->merge_group(m[j], *others[p],
                                      static_cast<std::int64_t>(j));
        }
    });
    for (std::size_t p = 0; p < others.size(); ++p) {
        const std::vector<std::int64_t>& m = map[p];
        for (std::size_t j = 0; j < m.size(); ++j)
            if (m[j] < 0)
                base->merge_group(base->group_of_other(
                                      *others[p], static_cast<std::int64_t>(j)),
                                  *others[p], static_cast<std::int64_t>(j));
    }
    return base;
}

AggStatePtr agg_regroup(const AggState& src_in,
                        const std::vector<std::int32_t>& keep,
                        std::int64_t bucket_recut) {
    const AggState& src = settled(src_in);
    AggStatePtr dst(new AggState());
    dst->specs = src.specs;
    dst->init_layout();
    dst->adopt_layout(src);  // field_domain/field_is_str are value-derived
    dst->nkeys = keep.size();
    dst->key_is_bytes.resize(dst->nkeys);
    dst->key_domain.resize(dst->nkeys);
    dst->key_type.resize(dst->nkeys);
    dst->key_time_unit.resize(dst->nkeys);
    dst->key_timezone.resize(dst->nkeys);
    dst->key_byte_width.resize(dst->nkeys);
    dst->key_decimal_precision.resize(dst->nkeys);
    dst->key_decimal_scale.resize(dst->nkeys);
    for (std::size_t k = 0; k < dst->nkeys; ++k) {
        const std::size_t s = static_cast<std::size_t>(keep[k]);
        dst->key_is_bytes[k] = src.key_is_bytes[s];
        dst->key_domain[k] = src.key_domain[s];
        dst->key_type[k] = src.key_type[s];
        dst->key_time_unit[k] = src.key_time_unit[s];
        dst->key_timezone[k] = src.key_timezone[s];
        dst->key_byte_width[k] = src.key_byte_width[s];
        dst->key_decimal_precision[k] = src.key_decimal_precision[s];
        dst->key_decimal_scale[k] = src.key_decimal_scale[s];
    }
    dst->ikey_cols.assign(dst->nkeys, {});
    dst->skey_cols.assign(dst->nkeys, {});
    dst->nkey_cols.assign(dst->nkeys, {});
    dst->inited = true;

    const std::int64_t ng = src.ngroups();
    for (std::int64_t j = 0; j < ng; ++j) {
        const std::int64_t g = dst->find_or_add_group(
            [&](std::size_t k) -> std::int64_t {
                std::int64_t v =
                    src.ikey_cols[static_cast<std::size_t>(keep[k])]
                                 [static_cast<std::size_t>(j)];
                if (k == 0 && bucket_recut > 0)
                    v = (v / bucket_recut) * bucket_recut;
                return v;
            },
            [&](std::size_t k) -> std::string_view {
                return src.skey_cols[static_cast<std::size_t>(keep[k])]
                                    [static_cast<std::size_t>(j)];
            },
            [&](std::size_t k) {
                return src.nkey_cols[static_cast<std::size_t>(keep[k])]
                                    [static_cast<std::size_t>(j)] != 0;
            });
        dst->merge_group(g, src, j);
    }
    return dst;
}

}  // namespace dftracer::utils::dataframe
