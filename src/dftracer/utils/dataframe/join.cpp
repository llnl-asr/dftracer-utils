// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/internal/cell_ops.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/errors.h>
#include <dftracer/utils/dataframe/internal/varwidth_offsets.h>
#include <dftracer/utils/dataframe/join.h>
#include <dftracer/utils/dataframe/kernels/filter.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/dataframe/query/vectorize.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/hash/constants.h>
#include <dftracer/utils/dstd/hash/hash_combine.h>
#include <dftracer/utils/dstd/hasher.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace dftracer::utils::dataframe {

namespace {

constexpr const char* DEFAULT_SUFFIX = "_right";

bool keeps_unmatched_left(JoinHow how) {
    return how == JoinHow::Left || how == JoinHow::Outer;
}

bool keeps_unmatched_right(JoinHow how) {
    return how == JoinHow::Right || how == JoinHow::Outer;
}

bool left_only(JoinHow how) {
    return how == JoinHow::Semi || how == JoinHow::Anti;
}

bool by_value(JoinHow how) {
    return how == JoinHow::Lookup || how == JoinHow::Nest;
}

bool is_float(TypeId t) {
    return t == TypeId::Float16 || t == TypeId::Float32 || t == TypeId::Float64;
}

// The value key of row `r` over `keys`; false when a part has none.
bool value_key(dstd::String& out, const dstd::Vector<Series>& keys,
               std::int64_t r) {
    out.clear();
    for (const Series& k : keys)
        if (!dataframe::query::append_cell_key(out, k, r)) return false;
    return true;
}

Series flat(const Series& c) {
    return c.encoding() == Encoding::Flat ? c.share() : materialize(c);
}

std::int64_t index_of(const dstd::Vector<dstd::String>& names,
                      const dstd::String& name) {
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i] == name) return static_cast<std::int64_t>(i);
    return -1;
}

// The hash every null key cell gets when nulls match each other.
constexpr std::uint64_t NULL_KEY_HASH =
    dftracer::utils::dstd::hash::GOLDEN_RATIO;

// A key column as the probe reads it: the buffers resolved once, so the
// per-row hash, null test and compare are pointer arithmetic, not calls
// across the C ABI.
struct KeyView {
    enum Kind { Fixed, Bool, Str32, Str64 } kind = Fixed;
    const std::uint8_t* data = nullptr;
    const std::uint8_t* validity = nullptr;
    const std::int32_t* off32 = nullptr;
    const std::int64_t* off64 = nullptr;
    std::size_t width = 0;
    // A null cell is a value: it hashes to one constant and equals only another
    // null cell (the `nulls_equal` join). Off, a null key never matches.
    bool null_safe = false;

    explicit KeyView(const Series& c) {
        const dftu_series& h = *c.handle();
        if (!is_orderable_type(h.type))
            dstd::fatal(
                "join: a key column with no per-row value reached a "
                "probe");
        data = h.data.block() ? h.data.data() : nullptr;
        validity = h.validity.block() ? h.validity.data() : nullptr;
        const TypeId narrow = narrow_varwidth_type(h.type);
        if (narrow == TypeId::String || narrow == TypeId::Binary) {
            if (h.wide_offsets()) {
                kind = Str64;
                off64 = reinterpret_cast<const std::int64_t*>(h.offsets.data());
            } else {
                kind = Str32;
                off32 = reinterpret_cast<const std::int32_t*>(h.offsets.data());
            }
        } else if (h.type == TypeId::Bool) {
            kind = Bool;
        } else {
            kind = Fixed;
            width = byte_width(h.type, h.fixed_size()).value_or(0);
        }
    }
    bool is_null(std::int64_t i) const {
        return validity && !((validity[i >> 3] >> (i & 7)) & 1);
    }
    dstd::StringView bytes(std::int64_t i) const {
        if (kind == Str64)
            return dstd::StringView(
                reinterpret_cast<const char*>(data) + off64[i],
                static_cast<std::size_t>(off64[i + 1] - off64[i]));
        return dstd::StringView(
            reinterpret_cast<const char*>(data) + off32[i],
            static_cast<std::size_t>(off32[i + 1] - off32[i]));
    }
    // The value's bits for a fixed-width cell (a single integer key hashes
    // to itself), the bytes' hash otherwise.
    std::uint64_t hash(std::int64_t i) const {
        if (null_safe && is_null(i)) return NULL_KEY_HASH;
        switch (kind) {
            case Fixed: {
                std::uint64_t bits = 0;
                const std::uint8_t* p =
                    data + static_cast<std::size_t>(i) * width;
                if (width == 8) {
                    std::memcpy(&bits, p, 8);
                    return bits;
                }
                std::memcpy(&bits, p,
                            width < sizeof(bits) ? width : sizeof(bits));
                if (width > sizeof(bits))
                    bits ^= dstd::Hash<dstd::StringView>{}(dstd::StringView(
                        reinterpret_cast<const char*>(p), width));
                return bits;
            }
            case Bool:
                return (data[i >> 3] >> (i & 7)) & 1;
            case Str32:
            case Str64:
                return dstd::Hash<dstd::StringView>{}(bytes(i));
        }
        return 0;
    }
    bool equals(std::int64_t i, const KeyView& o, std::int64_t j) const {
        if (null_safe) {
            const bool an = is_null(i), bn = o.is_null(j);
            if (an || bn) return an && bn;
        }
        switch (kind) {
            case Fixed: {
                const std::uint8_t* a =
                    data + static_cast<std::size_t>(i) * width;
                const std::uint8_t* b =
                    o.data + static_cast<std::size_t>(j) * width;
                // The common widths compare as one load each; memcmp is a
                // call per row, which the probe loop cannot afford.
                if (width == 8) {
                    std::uint64_t x, y;
                    std::memcpy(&x, a, 8);
                    std::memcpy(&y, b, 8);
                    return x == y;
                }
                if (width == 4) {
                    std::uint32_t x, y;
                    std::memcpy(&x, a, 4);
                    std::memcpy(&y, b, 4);
                    return x == y;
                }
                return std::memcmp(a, b, width) == 0;
            }
            case Bool:
                return ((data[i >> 3] >> (i & 7)) & 1) ==
                       ((o.data[j >> 3] >> (j & 7)) & 1);
            case Str32:
            case Str64:
                return bytes(i) == o.bytes(j);
        }
        return false;
    }
};

dstd::Vector<KeyView> key_views(const dstd::Vector<Series>& keys,
                                bool nulls_equal = false) {
    dstd::Vector<KeyView> out;
    out.reserve(keys.size());
    for (const Series& k : keys) {
        out.emplace_back(k);
        out.back().null_safe = nulls_equal;
    }
    return out;
}

bool any_null(const dstd::Vector<KeyView>& keys, std::int64_t i) {
    for (const KeyView& k : keys)
        if (!k.null_safe && k.is_null(i)) return true;
    return false;
}

std::uint64_t row_hash(const dstd::Vector<KeyView>& keys, std::int64_t i) {
    if (keys.size() == 1) return keys[0].hash(i);
    std::size_t h = 0;
    for (const KeyView& k : keys)
        dftracer::utils::dstd::hash_combine(
            h, static_cast<std::size_t>(k.hash(i)));
    return h;
}

bool rows_equal(const dstd::Vector<KeyView>& a, std::int64_t i,
                const dstd::Vector<KeyView>& b, std::int64_t j) {
    for (std::size_t k = 0; k < a.size(); ++k)
        if (!a[k].equals(i, b[k], j)) return false;
    return true;
}

// The key hash per row, built in parallel; a row with a null key is
// reported through `null_row` and gets no hash.
dstd::Vector<std::uint64_t> build_hashes(const dstd::Vector<KeyView>& keys,
                                         std::int64_t n,
                                         dstd::Vector<std::uint8_t>& null_row) {
    dstd::Vector<std::uint64_t> out(static_cast<std::size_t>(n));
    null_row.assign(static_cast<std::size_t>(n), 0);
    parallel_for(n, std::int64_t{1} << 15, [&](std::int64_t b, std::int64_t e) {
        for (std::int64_t i = b; i < e; ++i) {
            if (any_null(keys, i)) {
                null_row[static_cast<std::size_t>(i)] = 1;
                continue;
            }
            out[static_cast<std::size_t>(i)] = row_hash(keys, i);
        }
    });
    return out;
}

}  // namespace

bool valid_join_how(JoinHow how) noexcept {
    switch (how) {
        case JoinHow::Inner:
        case JoinHow::Left:
        case JoinHow::Right:
        case JoinHow::Outer:
        case JoinHow::Semi:
        case JoinHow::Anti:
        case JoinHow::Cross:
        case JoinHow::Lookup:
        case JoinHow::Nest:
            return true;
    }
    return false;
}

dstd::Result<HashJoin> HashJoin::make(DataFrame right,
                                      dstd::Vector<dstd::String> left_on,
                                      dstd::Vector<dstd::String> right_on,
                                      JoinHow how, dstd::String suffix,
                                      bool nulls_equal) {
    if (!valid_join_how(how))
        return invalid_argument<"join: unknown join kind {}">(
            static_cast<int>(how));
    if (nulls_equal && (how == JoinHow::Cross || how == JoinHow::Lookup ||
                        how == JoinHow::Nest))
        return invalid_argument<
            "join: nulls_equal does not apply to a {} join">(
            how == JoinHow::Cross    ? "cross"
            : how == JoinHow::Lookup ? "lookup"
                                     : "nest");
    if (how == JoinHow::Cross) {
        left_on.clear();
        right_on.clear();
    } else {
        if (left_on.empty()) return invalid_argument<"join: no key columns">();
        if (left_on.size() != right_on.size())
            return invalid_argument<
                "join: left_on and right_on differ in length">();
    }
    if (how == JoinHow::Nest && suffix.empty())
        return invalid_argument<
            "join: a nest join names its list column with `suffix`">();
    for (const dstd::String& k : right_on) {
        const std::int64_t ri = index_of(right.names, k);
        if (ri < 0) return not_found<"join: no right column named {}">(k);
        const TypeId t = right.columns[static_cast<std::size_t>(ri)].type();
        if (!is_orderable_type(t))
            return invalid_argument<
                "join: key column type '{}' has no per-row value to key on">(
                type_name(t));
    }
    return HashJoin(std::move(right), std::move(left_on), std::move(right_on),
                    how, std::move(suffix), nulls_equal);
}

HashJoin::HashJoin(DataFrame right, dstd::Vector<dstd::String> left_on,
                   dstd::Vector<dstd::String> right_on, JoinHow how,
                   dstd::String suffix, bool nulls_equal)
    : right_(std::move(right)),
      left_on_(std::move(left_on)),
      right_on_(std::move(right_on)),
      how_(how),
      suffix_(suffix.empty() ? DEFAULT_SUFFIX : suffix),
      nulls_equal_(nulls_equal) {
    right_keys_.reserve(right_on_.size());
    for (const dstd::String& k : right_on_) {
        const std::int64_t ri = index_of(right_.names, k);
        right_keys_.push_back(
            flat(right_.columns[static_cast<std::size_t>(ri)]));
    }

    const std::int64_t n = right_.num_rows();
    if (keeps_unmatched_right(how_)) {
        right_matched_n_ = static_cast<std::size_t>(n);
        right_matched_ =
            dstd::make_unique<std::atomic<std::uint8_t>[]>(right_matched_n_);
    }
    if (how_ == JoinHow::Cross) return;

    dstd::Vector<std::uint8_t> null_row;
    const dstd::Vector<KeyView> views = key_views(right_keys_, nulls_equal_);
    dstd::Vector<std::uint64_t> hashes = build_hashes(views, n, null_row);
    next_.assign(static_cast<std::size_t>(n), -1);

    // A single integer key whose values span a range not much wider than
    // the build side is looked up by direct address: the key's bits index
    // an array, no hashing on either side. Signed negatives read as huge
    // unsigned values and widen the span past the limit, which is the
    // fall-back to the hash table.
    if (!nulls_equal_ && views.size() == 1 && views[0].kind == KeyView::Fixed &&
        views[0].width == 8 &&
        (right_keys_[0].type() == TypeId::Int64 ||
         right_keys_[0].type() == TypeId::Uint64)) {
        std::uint64_t lo = ~std::uint64_t{0}, hi = 0;
        bool any = false;
        for (std::int64_t i = 0; i < n; ++i) {
            if (null_row[static_cast<std::size_t>(i)]) continue;
            const std::uint64_t v = hashes[static_cast<std::size_t>(i)];
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            any = true;
        }
        const std::uint64_t span = any ? hi - lo + 1 : 0;
        if (any && span <= static_cast<std::uint64_t>(n) * 4 + 1024) {
            direct_base_ = lo;
            direct_.assign(static_cast<std::size_t>(span), -1);
            for (std::int64_t i = n - 1; i >= 0; --i) {
                if (null_row[static_cast<std::size_t>(i)]) continue;
                std::int64_t& slot = direct_[static_cast<std::size_t>(
                    hashes[static_cast<std::size_t>(i)] - lo)];
                if (slot >= 0) next_[static_cast<std::size_t>(i)] = slot;
                slot = i;
            }
        }
    }

    if (direct_.empty()) {
        first_.reserve(static_cast<std::size_t>(n));
        // Insert back to front so each chain lists its rows in ascending
        // order.
        for (std::int64_t i = n - 1; i >= 0; --i) {
            if (null_row[static_cast<std::size_t>(i)]) continue;
            auto [it, inserted] =
                first_.try_emplace(hashes[static_cast<std::size_t>(i)], i);
            if (!inserted) {
                next_[static_cast<std::size_t>(i)] = it->second;
                it->second = i;
            }
        }
    }
    if (how_ == JoinHow::Lookup) typed_conflicts_ = conflicts(true);
}

dstd::Result<dstd::Vector<std::int64_t>> HashJoin::left_key_indices(
    const dstd::Vector<dstd::String>& left_names) const {
    dstd::Vector<std::int64_t> idx;
    idx.reserve(left_on_.size());
    for (const dstd::String& k : left_on_) {
        const std::int64_t li = index_of(left_names, k);
        if (li < 0) return not_found<"join: no left column named {}">(k);
        idx.push_back(li);
    }
    return idx;
}

std::int64_t HashJoin::shared_key_of(
    const dstd::Vector<dstd::String>& left_names, std::size_t li) const {
    for (std::size_t p = 0; p < left_on_.size(); ++p)
        if (left_on_[p] == left_names[li] && right_on_[p] == left_names[li])
            return static_cast<std::int64_t>(p);
    return -1;
}

dstd::Result<dstd::Vector<dstd::String>> HashJoin::out_names(
    const dstd::Vector<dstd::String>& left_names) const {
    return join_out_names(left_names, right_.names, left_on_, right_on_, how_,
                          suffix_);
}

namespace {

JoinRightLayout right_layout(const dstd::Vector<dstd::String>& left_names,
                             const dstd::Vector<dstd::String>& right_names,
                             const dstd::Vector<dstd::String>& left_on,
                             const dstd::Vector<dstd::String>& right_on,
                             JoinHow how, const dstd::String& suffix) {
    JoinRightLayout out;
    if (left_only(how)) return out;
    if (how == JoinHow::Nest) {
        for (std::size_t ri = 0; ri < right_names.size(); ++ri) {
            out.keep.push_back(ri);
            out.names.push_back(right_names[ri]);
            out.fills.push_back(-1);
        }
        return out;
    }
    if (how == JoinHow::Lookup) {
        for (std::size_t ri = 0; ri < right_names.size(); ++ri) {
            if (std::find(right_on.begin(), right_on.end(), right_names[ri]) !=
                right_on.end())
                continue;
            out.keep.push_back(ri);
            out.names.push_back(right_names[ri]);
            out.fills.push_back(index_of(left_names, right_names[ri]));
        }
        return out;
    }
    const dstd::String& sfx = suffix.empty() ? DEFAULT_SUFFIX : suffix;
    for (std::size_t ri = 0; ri < right_names.size(); ++ri) {
        const dstd::String& name = right_names[ri];
        bool shared_key = false;
        for (std::size_t p = 0; p < right_on.size(); ++p)
            if (right_on[p] == name && left_on[p] == name) shared_key = true;
        if (shared_key) continue;
        out.keep.push_back(ri);
        out.names.push_back(index_of(left_names, name) >= 0 ? name + sfx
                                                            : name);
        out.fills.push_back(-1);
    }
    return out;
}

}  // namespace

dstd::Result<JoinRightLayout> join_right_layout(
    const dstd::Vector<dstd::String>& left_names,
    const dstd::Vector<dstd::String>& right_names,
    const dstd::Vector<dstd::String>& left_on,
    const dstd::Vector<dstd::String>& right_on, JoinHow how,
    const dstd::String& suffix) {
    if (left_on.size() != right_on.size())
        return invalid_argument<
            "join: left_on and right_on differ in length">();
    return right_layout(left_names, right_names, left_on, right_on, how,
                        suffix);
}

dstd::Result<dstd::Vector<dstd::String>> join_out_names(
    const dstd::Vector<dstd::String>& left_names,
    const dstd::Vector<dstd::String>& right_names,
    const dstd::Vector<dstd::String>& left_on,
    const dstd::Vector<dstd::String>& right_on, JoinHow how,
    const dstd::String& suffix) {
    for (const dstd::String& k : left_on)
        if (index_of(left_names, k) < 0)
            return not_found<"join: no left column named {}">(k);
    for (const dstd::String& k : right_on)
        if (index_of(right_names, k) < 0)
            return not_found<"join: no right column named {}">(k);
    dstd::Vector<dstd::String> out = left_names;
    if (how == JoinHow::Nest) {
        if (suffix.empty())
            return invalid_argument<
                "join: a nest join names its list column with `suffix`">();
        out.push_back(suffix);
        return out;
    }
    auto layout = join_right_layout(left_names, right_names, left_on, right_on,
                                    how, suffix);
    if (!layout) return layout.error();
    for (std::size_t i = 0; i < layout->names.size(); ++i)
        if (layout->fills[i] < 0) out.push_back(layout->names[i]);
    return out;
}

dstd::Result<dstd::Vector<Field>> join_out_fields(
    const dstd::Vector<Field>& left, const dstd::Vector<Field>& right,
    const dstd::Vector<dstd::String>& left_on,
    const dstd::Vector<dstd::String>& right_on, JoinHow how,
    const dstd::String& suffix) {
    dstd::Vector<dstd::String> left_names;
    left_names.reserve(left.size());
    for (const Field& f : left) left_names.push_back(f.name);
    dstd::Vector<dstd::String> right_names;
    right_names.reserve(right.size());
    for (const Field& f : right) right_names.push_back(f.name);
    auto names =
        join_out_names(left_names, right_names, left_on, right_on, how, suffix);
    if (!names) return names.error();
    dstd::Vector<Field> out;
    out.reserve(left.size() + right.size());
    for (const Field& f : left) out.push_back(Field{f.name, f.type, true});
    if (how == JoinHow::Nest) {
        dstd::Vector<Field> members;
        for (const Field& f : right)
            members.push_back(Field{f.name, f.type, true});
        out.push_back(
            Field{suffix, list_of(struct_of(std::move(members))), true});
        return out;
    }
    const JoinRightLayout layout =
        right_layout(left_names, right_names, left_on, right_on, how, suffix);
    for (std::size_t i = 0; i < layout.keep.size(); ++i) {
        const Field f{layout.names[i], right[layout.keep[i]].type, true};
        if (layout.fills[i] >= 0)
            out[static_cast<std::size_t>(layout.fills[i])] = f;
        else
            out.push_back(f);
    }
    return out;
}

dstd::Result<DataFrame> HashJoin::probe(
    const DataFrame& left, dstd::Vector<std::uint8_t>* left_matched,
    std::int64_t left_base) {
    auto indices = left_key_indices(left.names);
    if (!indices) return indices.error();
    const dstd::Vector<std::int64_t>& key_idx = *indices;
    if (by_value(how_)) {
        if (left_matched && how_ == JoinHow::Lookup)
            dstd::fatal("join: a lookup join cannot be probed block by block");
        return probe_values(left, key_idx, left_matched, left_base);
    }
    std::uint8_t* matched =
        left_matched ? left_matched->data() + left_base : nullptr;
    dstd::Vector<Series> left_keys;
    left_keys.reserve(key_idx.size());
    for (std::size_t p = 0; p < key_idx.size(); ++p) {
        const Series& lc = left.columns[static_cast<std::size_t>(key_idx[p])];
        const Series& rc = right_keys_[p];
        if (lc.type() != rc.type())
            return invalid_argument<
                "join: key '{}' is {} on the left but '{}' is {} on the "
                "right">(left_on_[p], type_name(lc.type()), right_on_[p],
                         type_name(rc.type()));
        left_keys.push_back(flat(lc));
    }

    const std::int64_t n = left.num_rows();
    const std::int64_t nright = right_.num_rows();

    // The pair lists a probe writes, then the gathers over them. The index
    // width is the caller's: 32-bit when both sides fit, since the lists
    // and the gathers are memory-bound and polars' are 32-bit.
    auto probe_into = [&]<class Idx>(dstd::Vector<Idx>& left_idx,
                                     dstd::Vector<Idx>& right_idx) {
        const dstd::Vector<KeyView> lviews = key_views(left_keys, nulls_equal_);
        const dstd::Vector<KeyView> rviews =
            key_views(right_keys_, nulls_equal_);
        // Probe in parallel: each chunk of left rows writes its own pair
        // lists, stitched in chunk order so the output stays in left order.
        constexpr std::int64_t GRAIN = std::int64_t{1} << 15;
        const std::int64_t chunks = n == 0 ? 0 : (n + GRAIN - 1) / GRAIN;
        dstd::Vector<dstd::Vector<Idx>> lparts(
            static_cast<std::size_t>(chunks));
        dstd::Vector<dstd::Vector<Idx>> rparts(
            static_cast<std::size_t>(chunks));
        const bool track_right = right_matched_n_ != 0;
        // The row loop over three probes: is the key null, the first
        // candidate row, does candidate r equal row i. Each mode below
        // passes its own three, inlined, so the common single 8-byte key
        // reads one word per row and compares one word per candidate.
        // A chunk's pair lists grow by direct stores through a cursor;
        // push_back is a call per row that the loop cannot afford.
        struct Appender {
            dstd::Vector<Idx>& v;
            std::size_t n = 0;
            void push(Idx x) {
                if (n == v.size()) v.resize(v.size() * 2 + 1024);
                v[n++] = x;
            }
        };
        auto run = [&](auto is_null, auto first_of, auto equal) {
            parallel_for(n, GRAIN, [&](std::int64_t b, std::int64_t e) {
                Appender lp{lparts[static_cast<std::size_t>(b / GRAIN)]};
                Appender rp{rparts[static_cast<std::size_t>(b / GRAIN)]};
                lp.v.resize(static_cast<std::size_t>(e - b));
                if (!left_only(how_))
                    rp.v.resize(static_cast<std::size_t>(e - b));
                for (std::int64_t i = b; i < e; ++i) {
                    std::int64_t r = -1;
                    if (!is_null(i)) {
                        r = first_of(i);
                        // The first chain row whose key really equals; the
                        // rest of the chain is walked below the same way.
                        while (r >= 0 && !equal(i, r))
                            r = next_[static_cast<std::size_t>(r)];
                    }
                    if (r < 0) {
                        if (matched) continue;
                        if (how_ == JoinHow::Anti) lp.push(static_cast<Idx>(i));
                        if (keeps_unmatched_left(how_)) {
                            lp.push(static_cast<Idx>(i));
                            rp.push(Idx{-1});
                        }
                        continue;
                    }
                    if (matched) matched[i] = 1;
                    if (how_ == JoinHow::Semi) {
                        if (!matched) lp.push(static_cast<Idx>(i));
                        continue;
                    }
                    if (how_ == JoinHow::Anti) continue;
                    for (; r >= 0; r = next_[static_cast<std::size_t>(r)]) {
                        if (!equal(i, r)) continue;
                        lp.push(static_cast<Idx>(i));
                        rp.push(static_cast<Idx>(r));
                        if (track_right)
                            right_matched_[static_cast<std::size_t>(r)].store(
                                1, std::memory_order_relaxed);
                    }
                }
                lp.v.resize(lp.n);
                rp.v.resize(rp.n);
            });
        };
        const bool one_word = !nulls_equal_ && lviews.size() == 1 &&
                              lviews[0].kind == KeyView::Fixed &&
                              lviews[0].width == 8;
        if (one_word && !direct_.empty()) {
            const auto* lk =
                reinterpret_cast<const std::uint64_t*>(lviews[0].data);
            const auto* rk =
                reinterpret_cast<const std::uint64_t*>(rviews[0].data);
            const std::uint8_t* lv = lviews[0].validity;
            const std::uint64_t base = direct_base_;
            const std::uint64_t span = direct_.size();
            const std::int64_t* direct = direct_.data();
            run(
                [&](std::int64_t i) {
                    return lv && !((lv[i >> 3] >> (i & 7)) & 1);
                },
                [&](std::int64_t i) {
                    const std::uint64_t at = lk[i] - base;
                    return at < span ? direct[at] : std::int64_t{-1};
                },
                [&](std::int64_t i, std::int64_t r) { return lk[i] == rk[r]; });
        } else if (one_word) {
            const auto* lk =
                reinterpret_cast<const std::uint64_t*>(lviews[0].data);
            const auto* rk =
                reinterpret_cast<const std::uint64_t*>(rviews[0].data);
            const std::uint8_t* lv = lviews[0].validity;
            run(
                [&](std::int64_t i) {
                    return lv && !((lv[i >> 3] >> (i & 7)) & 1);
                },
                [&](std::int64_t i) {
                    auto it = first_.find(lk[i]);
                    return it != first_.end() ? it->second : std::int64_t{-1};
                },
                [&](std::int64_t i, std::int64_t r) { return lk[i] == rk[r]; });
        } else {
            run([&](std::int64_t i) { return any_null(lviews, i); },
                [&](std::int64_t i) {
                    const std::uint64_t h = row_hash(lviews, i);
                    if (!direct_.empty()) {
                        const std::uint64_t at = h - direct_base_;
                        return at < direct_.size()
                                   ? direct_[static_cast<std::size_t>(at)]
                                   : std::int64_t{-1};
                    }
                    auto it = first_.find(h);
                    return it != first_.end() ? it->second : std::int64_t{-1};
                },
                [&](std::int64_t i, std::int64_t r) {
                    return rows_equal(lviews, i, rviews, r);
                });
        }
        dstd::Vector<std::size_t> at(lparts.size() + 1, 0);
        for (std::size_t c = 0; c < lparts.size(); ++c)
            at[c + 1] = at[c] + lparts[c].size();
        left_idx.resize(at.back());
        if (!left_only(how_)) right_idx.resize(at.back());
        parallel_for(
            static_cast<std::int64_t>(lparts.size()), 1,
            [&](std::int64_t b, std::int64_t e) {
                for (std::int64_t c = b; c < e; ++c) {
                    const auto ci = static_cast<std::size_t>(c);
                    std::copy(
                        lparts[ci].begin(), lparts[ci].end(),
                        left_idx.begin() + static_cast<std::ptrdiff_t>(at[ci]));
                    if (!rparts[ci].empty())
                        std::copy(rparts[ci].begin(), rparts[ci].end(),
                                  right_idx.begin() +
                                      static_cast<std::ptrdiff_t>(at[ci]));
                }
            });
    };
    auto emit = [&]<class Idx>(const dstd::Vector<Idx>& left_idx,
                               const dstd::Vector<Idx>& right_idx) {
        auto gather = [](const Series& c, const dstd::Vector<Idx>& idx) {
            if constexpr (std::is_same_v<Idx, std::int32_t>)
                return take32(c, idx);
            else
                return take(c, idx);
        };
        DataFrame out;
        out.names = left.names;
        out.columns.reserve(left.columns.size() + right_.columns.size());
        for (const Series& c : left.columns)
            out.columns.push_back(gather(c, left_idx));
        JoinRightLayout layout = right_layout(
            left.names, right_.names, left_on_, right_on_, how_, suffix_);
        for (std::size_t i = 0; i < layout.keep.size(); ++i) {
            out.names.push_back(layout.names[i]);
            out.columns.push_back(
                gather(right_.columns[layout.keep[i]], right_idx));
        }
        return dstd::Result<DataFrame>(std::move(out));
    };

    if (how_ == JoinHow::Cross) {
        dstd::Vector<std::int64_t> left_idx;
        dstd::Vector<std::int64_t> right_idx;
        left_idx.reserve(static_cast<std::size_t>(n * nright));
        right_idx.reserve(static_cast<std::size_t>(n * nright));
        for (std::int64_t i = 0; i < n; ++i)
            for (std::int64_t r = 0; r < nright; ++r) {
                left_idx.push_back(i);
                right_idx.push_back(r);
            }
        return emit(left_idx, right_idx);
    }
    constexpr std::int64_t FITS32 = std::numeric_limits<std::int32_t>::max();
    if (n < FITS32 && nright < FITS32) {
        dstd::Vector<std::int32_t> left_idx;
        dstd::Vector<std::int32_t> right_idx;
        probe_into(left_idx, right_idx);
        return emit(left_idx, right_idx);
    }
    dstd::Vector<std::int64_t> left_idx;
    dstd::Vector<std::int64_t> right_idx;
    probe_into(left_idx, right_idx);
    return emit(left_idx, right_idx);
}

void HashJoin::build_value_index() {
    if (value_index_) return;
    const std::int64_t n = right_.num_rows();
    dstd::String key;
    for (std::int64_t r = 0; r < n; ++r)
        if (value_key(key, right_keys_, r)) by_value_[key].push_back(r);
    if (how_ == JoinHow::Lookup && !conflicts_given_)
        value_conflicts_ = conflicts(false);
    value_index_ = true;
}

void HashJoin::set_lookup_conflicts(dstd::Vector<std::int64_t> per_row) {
    if (how_ != JoinHow::Lookup ||
        per_row.size() != static_cast<std::size_t>(right_.num_rows()))
        dstd::fatal("join: lookup conflicts do not match the right rows");
    typed_conflicts_ = per_row;
    value_conflicts_ = std::move(per_row);
    conflicts_given_ = true;
}

dstd::Vector<std::int64_t> HashJoin::conflicts(bool typed) const {
    const std::int64_t n = right_.num_rows();
    dstd::Vector<std::int64_t> out(static_cast<std::size_t>(n), -1);
    dstd::Vector<std::size_t> values;
    for (std::size_t c = 0; c < right_.columns.size(); ++c)
        if (std::find(right_on_.begin(), right_on_.end(), right_.names[c]) ==
            right_on_.end())
            values.push_back(c);
    auto check = [&](std::int64_t head, std::int64_t r) {
        std::int64_t& slot = out[static_cast<std::size_t>(head)];
        if (slot >= 0) return;
        for (const std::size_t c : values)
            if (!dataframe::query::same_cell(right_.columns[c], head,
                                             right_.columns[c], r)) {
                slot = static_cast<std::int64_t>(c);
                return;
            }
    };
    if (!typed) {
        for (const auto& [key, rows] : by_value_)
            for (std::size_t i = 1; i < rows.size(); ++i)
                check(rows.front(), rows[i]);
        return out;
    }
    const dstd::Vector<KeyView> views = key_views(right_keys_);
    for (std::int64_t r = 0; r < n; ++r) {
        if (any_null(views, r)) continue;
        std::int64_t head = -1;
        const std::uint64_t h = row_hash(views, r);
        if (!direct_.empty()) {
            head = direct_[static_cast<std::size_t>(h - direct_base_)];
        } else if (auto it = first_.find(h); it != first_.end()) {
            head = it->second;
        }
        while (head >= 0 && !rows_equal(views, r, views, head))
            head = next_[static_cast<std::size_t>(head)];
        if (head >= 0 && head != r) check(head, r);
    }
    return out;
}

dstd::Vector<std::int64_t> HashJoin::heads(
    const dstd::Vector<Series>& left_keys, bool typed) {
    const std::int64_t n = left_keys.empty() ? 0 : left_keys.front().length();
    dstd::Vector<std::int64_t> out(static_cast<std::size_t>(n), -1);
    constexpr std::int64_t GRAIN = std::int64_t{1} << 15;
    if (!typed) {
        build_value_index();
        parallel_for(n, GRAIN, [&](std::int64_t b, std::int64_t e) {
            dstd::String key;
            for (std::int64_t i = b; i < e; ++i)
                if (value_key(key, left_keys, i))
                    if (auto it = by_value_.find(key); it != by_value_.end())
                        out[static_cast<std::size_t>(i)] = it->second.front();
        });
        return out;
    }
    const dstd::Vector<KeyView> lviews = key_views(left_keys);
    const dstd::Vector<KeyView> rviews = key_views(right_keys_);
    parallel_for(n, GRAIN, [&](std::int64_t b, std::int64_t e) {
        for (std::int64_t i = b; i < e; ++i) {
            if (any_null(lviews, i)) continue;
            const std::uint64_t h = row_hash(lviews, i);
            std::int64_t r = -1;
            if (!direct_.empty()) {
                const std::uint64_t at = h - direct_base_;
                if (at < direct_.size())
                    r = direct_[static_cast<std::size_t>(at)];
            } else if (auto it = first_.find(h); it != first_.end()) {
                r = it->second;
            }
            while (r >= 0 && !rows_equal(lviews, i, rviews, r))
                r = next_[static_cast<std::size_t>(r)];
            out[static_cast<std::size_t>(i)] = r;
        }
    });
    return out;
}

void HashJoin::members(std::int64_t head, bool typed,
                       dstd::Vector<std::int64_t>& out) const {
    if (!typed) {
        dstd::String key;
        value_key(key, right_keys_, head);
        const auto& rows = by_value_.find(key)->second;
        out.insert(out.end(), rows.begin(), rows.end());
        return;
    }
    const dstd::Vector<KeyView> views = key_views(right_keys_);
    for (std::int64_t r = head; r >= 0; r = next_[static_cast<std::size_t>(r)])
        if (rows_equal(views, head, views, r)) out.push_back(r);
}

dstd::Result<DataFrame> HashJoin::probe_values(
    const DataFrame& left, const dstd::Vector<std::int64_t>& key_idx,
    dstd::Vector<std::uint8_t>* left_matched, std::int64_t left_base) {
    dstd::Vector<Series> left_keys;
    bool typed = true;
    for (std::size_t p = 0; p < key_idx.size(); ++p) {
        const Series& lc = left.columns[static_cast<std::size_t>(key_idx[p])];
        const Series& rc = right_keys_[p];
        typed = typed && lc.type() == rc.type() && !is_float(lc.type()) &&
                is_orderable_type(lc.type());
        left_keys.push_back(flat(lc));
    }
    const std::int64_t n = left.num_rows();
    const dstd::Vector<std::int64_t> hs = heads(left_keys, typed);

    DataFrame out;
    out.names = left.names;
    for (const Series& c : left.columns) out.columns.push_back(c.share());
    if (how_ == JoinHow::Nest) {
        dstd::Vector<std::int32_t> offsets{0};
        dstd::Vector<std::int64_t> rows;
        dstd::Vector<std::int64_t> kept;
        for (std::int64_t i = 0; i < n; ++i) {
            const std::int64_t h = hs[static_cast<std::size_t>(i)];
            if (left_matched) {
                if (h < 0) continue;
                (*left_matched)[static_cast<std::size_t>(left_base + i)] = 1;
                kept.push_back(i);
            }
            if (h >= 0) members(h, typed, rows);
            offsets.push_back(static_cast<std::int32_t>(rows.size()));
        }
        if (left_matched)
            for (Series& c : out.columns) c = c.take(kept);
        dstd::Vector<Series> parts;
        for (const Series& c : right_.columns) parts.push_back(c.take(rows));
        out.names.push_back(suffix_);
        out.columns.push_back(Series::list(
            offsets, Series::structs(right_.names, std::move(parts))));
        return out;
    }

    const dstd::Vector<std::int64_t>& conf =
        typed ? typed_conflicts_ : value_conflicts_;
    for (std::int64_t i = 0; i < n; ++i) {
        const std::int64_t h = hs[static_cast<std::size_t>(i)];
        if (h < 0 || conf[static_cast<std::size_t>(h)] < 0) continue;
        dstd::String key;
        value_key(key, right_keys_, h);
        dstd::String on;
        for (const dstd::String& k : right_on_) {
            if (!on.empty()) on += ", ";
            on += dstd::StringView(k);
        }
        return invalid_argument<
            "join: lookup key {} of '{}' has right rows with different "
            "values of '{}'">(duql::key_text(key), on,
                              right_.names[static_cast<std::size_t>(
                                  conf[static_cast<std::size_t>(h)])]);
    }
    const JoinRightLayout layout = right_layout(
        left.names, right_.names, left_on_, right_on_, how_, suffix_);
    for (std::size_t k = 0; k < layout.keep.size(); ++k) {
        Series values = right_.columns[layout.keep[k]].take(hs);
        const std::int64_t at = layout.fills[k];
        if (at < 0) {
            out.names.push_back(layout.names[k]);
            out.columns.push_back(std::move(values));
            continue;
        }
        if (out.columns[static_cast<std::size_t>(at)].null_count() != n)
            return invalid_argument<
                "join: lookup fills '{}', which some left rows already hold">(
                layout.names[k]);
        out.columns[static_cast<std::size_t>(at)] = std::move(values);
    }
    return out;
}

DataFrame HashJoin::flush(const dstd::Vector<dstd::String>& left_names,
                          const dstd::Vector<Series>& left_templates) {
    DataFrame out;
    if (flushed_ || !keeps_unmatched_right(how_)) return out;
    flushed_ = true;
    if (left_templates.size() != left_names.size())
        dstd::fatal("join: left templates do not match the left column count");
    dstd::Vector<std::int64_t> right_idx;
    for (std::size_t r = 0; r < right_matched_n_; ++r)
        if (!right_matched_[r].load(std::memory_order_relaxed))
            right_idx.push_back(static_cast<std::int64_t>(r));
    if (right_idx.empty()) return out;
    const dstd::Vector<std::int64_t> nulls(right_idx.size(), -1);

    out.names = left_names;
    for (std::size_t li = 0; li < left_names.size(); ++li) {
        const std::int64_t p = shared_key_of(left_names, li);
        out.columns.push_back(
            p >= 0 ? take(right_keys_[static_cast<std::size_t>(p)], right_idx)
                   : take(left_templates[li], nulls));
    }
    JoinRightLayout layout = right_layout(left_names, right_.names, left_on_,
                                          right_on_, how_, suffix_);
    for (std::size_t i = 0; i < layout.keep.size(); ++i) {
        out.names.push_back(layout.names[i]);
        out.columns.push_back(take(right_.columns[layout.keep[i]], right_idx));
    }
    return out;
}

dstd::Result<DataFrame> join(const DataFrame& left, const DataFrame& right,
                             const dstd::Vector<dstd::String>& left_on,
                             const dstd::Vector<dstd::String>& right_on,
                             JoinHow how, const dstd::String& suffix,
                             bool nulls_equal) {
    dstd::Vector<Series> shared;
    shared.reserve(right.columns.size());
    for (const Series& c : right.columns) shared.push_back(c.share());
    auto hj = HashJoin::make(DataFrame{right.names, std::move(shared)}, left_on,
                             right_on, how, suffix, nulls_equal);
    if (!hj) return hj.error();
    auto matched = hj->probe(left);
    if (!matched) return matched.error();
    DataFrame rest = hj->flush(left.names, left.columns);
    if (rest.num_rows() == 0) return matched;
    return concat({&*matched, &rest});
}

namespace {

DataFrame prefix_metrics(const DataFrame& b, std::size_t n_key,
                         const char* prefix) {
    DataFrame out;
    out.names.reserve(b.names.size());
    out.columns.reserve(b.columns.size());
    for (std::size_t i = 0; i < b.names.size(); ++i) {
        out.names.push_back(i < n_key ? b.names[i]
                                      : dstd::fmt<"{}{}">(prefix, b.names[i]));
        out.columns.push_back(b.columns[i].share());
    }
    return out;
}

}  // namespace

dstd::Result<DataFrame> compare_agg(const DataFrame& base,
                                    const DataFrame& variant,
                                    std::int64_t n_key) {
    if (n_key < 1) return invalid_argument<"compare_agg: n_key < 1">();
    const auto nk = static_cast<std::size_t>(n_key);
    if (nk > base.names.size() || nk > variant.names.size())
        return invalid_argument<
            "compare_agg: n_key exceeds a frame's column count">();
    dstd::Vector<dstd::String> keys;
    for (std::size_t i = 0; i < nk; ++i) {
        if (base.names[i] != variant.names[i])
            return invalid_argument<
                "compare_agg: key column {} is '{}' vs '{}'">(i, base.names[i],
                                                              variant.names[i]);
        keys.push_back(base.names[i]);
    }

    auto outer =
        join(prefix_metrics(base, nk, "l_"), prefix_metrics(variant, nk, "r_"),
             keys, keys, JoinHow::Outer, "_right");
    if (!outer) return outer.error();
    auto sorted = sort_by_multi(*outer, keys, false);
    if (!sorted) return sorted.error();
    DataFrame joined = std::move(*sorted);

    const std::int64_t n = joined.num_rows();
    dstd::Vector<double> hundred(static_cast<std::size_t>(n), 100.0);
    Series scale = Series::flat_f64(hundred.data(), n);
    for (std::size_t i = nk; i < base.names.size(); ++i) {
        const dstd::String& m = base.names[i];
        if (joined.column_index(dstd::fmt<"r_{}">(m)) < 0) continue;
        Series l = joined.column(dstd::fmt<"l_{}">(m)).materialize();
        Series r = joined.column(dstd::fmt<"r_{}">(m)).materialize();
        if (!is_numeric_dispatchable(l.type()) ||
            !is_numeric_dispatchable(r.type()))
            continue;
        Series delta = r.sub(l);
        Series pct =
            delta.cast(TypeId::Float64).div(l.cast(TypeId::Float64)).mul(scale);
        joined = with_column(joined, dstd::fmt<"delta_{}">(m), delta);
        joined = with_column(joined, dstd::fmt<"pct_{}">(m), pct);
    }
    return joined;
}

}  // namespace dftracer::utils::dataframe
